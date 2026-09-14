"""Tests for UnixSocketAudioEngineClient against a real Unix domain socket
server -- not an in-process fake -- for the same reason
test_upload_memory.py and the mobile app's DaemonClient tests use real
sockets: this is exactly the kind of framing/protocol code that an
in-process fake can accidentally paper over."""

from __future__ import annotations

import json
import socket
import tempfile
import threading
import uuid
from pathlib import Path
from typing import List

import pytest

from control_daemon.audio_engine_client import UnixSocketAudioEngineClient
from control_daemon.models import Asset, AssetKind, Preset


class FakeEngineServer:
    """A minimal stand-in for the real audio_engine process: accepts one
    connection, reads newline-delimited JSON commands, records them, and
    replies with a canned {"ok": true, ...} unless told to reject."""

    def __init__(self, socket_path: Path) -> None:
        self.socket_path = socket_path
        self.received: List[dict] = []
        self.reject_next = False
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._listener.bind(str(socket_path))
        self._listener.listen(1)
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        try:
            conn, _ = self._listener.accept()
        except OSError:
            # close() was called (e.g. a test that never connects, like
            # the set_tempo no-op case) while accept() was still pending.
            return
        with conn:
            buf = b""
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    return
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    command = json.loads(line)
                    self.received.append(command)
                    if self.reject_next:
                        reply = {"ok": False, "code": "validation_error", "message": "nope"}
                        self.reject_next = False
                    else:
                        reply = {"ok": True, "cmd": command.get("cmd")}
                    conn.sendall((json.dumps(reply) + "\n").encode("utf-8"))

    def close(self) -> None:
        self._listener.close()


@pytest.fixture()
def fake_engine():
    # Deliberately not pytest's tmp_path: macOS's AF_UNIX path limit
    # (~104 bytes) is shorter than the nested pytest-of-<user>/pytest-N/
    # <test-name>/ directories tmp_path builds, so a socket bound there can
    # fail with "AF_UNIX path too long" depending on test name length.
    socket_path = Path(tempfile.gettempdir()) / f"ae-{uuid.uuid4().hex[:8]}.sock"
    server = FakeEngineServer(socket_path)
    yield server
    server.close()
    socket_path.unlink(missing_ok=True)


def test_load_preset_sends_matching_json(fake_engine):
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)
    preset = Preset(name="Ambient Swell")

    client.load_preset(preset)
    client.close()

    assert len(fake_engine.received) == 1
    command = fake_engine.received[0]
    assert command["cmd"] == "load_preset"
    assert command["preset"]["id"] == preset.id
    assert command["preset"]["name"] == "Ambient Swell"


def test_set_bypass_and_register_asset(fake_engine):
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)
    asset = Asset(kind=AssetKind.NAM, filename="amp.nam", stored_path="/data/assets/amp.nam")

    client.set_bypass(True)
    client.register_asset(asset)
    client.close()

    assert [c["cmd"] for c in fake_engine.received] == ["set_bypass", "register_asset"]
    assert fake_engine.received[0]["bypass"] is True
    assert fake_engine.received[1]["asset"]["id"] == asset.id
    assert fake_engine.received[1]["asset"]["kind"] == "nam"


def test_reused_connection_across_calls(fake_engine):
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)

    client.set_bypass(True)
    client.set_bypass(False)
    client.close()

    assert len(fake_engine.received) == 2


def test_engine_unreachable_does_not_raise(tmp_path: Path):
    # No listener at this path at all -- a real scenario during dev/test
    # before the engine process has been started.
    client = UnixSocketAudioEngineClient(tmp_path / "no_such.sock", timeout=0.2)

    client.set_bypass(True)  # must not raise
    client.close()


def test_set_tempo_is_a_documented_noop(fake_engine):
    # The engine's control-socket protocol has no "set_tempo" command yet
    # (see audio_engine_client.py docstring) -- this must not send anything
    # or raise.
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)

    client.set_tempo(120.0)
    client.close()

    assert fake_engine.received == []
