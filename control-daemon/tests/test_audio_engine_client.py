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

from control_daemon.audio_engine_client import (
    AudioEngineRejected,
    UnixSocketAudioEngineClient,
)
from control_daemon.models import Asset, AssetKind, BlockParamDescriptor, Preset


class FakeEngineServer:
    """A minimal stand-in for the real audio_engine process: accepts one
    connection, reads newline-delimited JSON commands, records them, and
    replies with a canned {"ok": true, ...} unless told to reject."""

    def __init__(self, socket_path: Path) -> None:
        self.socket_path = socket_path
        self.received: List[dict] = []
        self.reject_next = False
        # Merged into the next reply -- lets a test simulate e.g.
        # register_asset's "parameters" field or list_block_types'
        # "block_types" field without a real engine process.
        self.extra_reply_fields: dict = {}
        # Commands seen per connection, so a test can tell what the client
        # sent on *reconnecting* apart from what it sent before.
        self.connections: List[List[dict]] = []
        self._listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._listener.bind(str(socket_path))
        self._listener.listen(1)
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self) -> None:
        # Loops rather than accepting once: the client reconnects after the
        # engine process restarts, and what it sends on that new connection
        # is exactly what the asset-replay test needs to observe.
        while True:
            try:
                conn, _ = self._listener.accept()
            except OSError:
                # close() was called (e.g. a test that never connects, like
                # the set_tempo no-op case) while accept() was still pending.
                return
            self.connections.append([])
            self._handle(conn)

    def _handle(self, conn: socket.socket) -> None:
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
                    self.connections[-1].append(command)
                    if self.reject_next:
                        reply = {"ok": False, "code": "validation_error", "message": "nope"}
                        self.reject_next = False
                    else:
                        reply = {"ok": True, "cmd": command.get("cmd"), **self.extra_reply_fields}
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


def test_register_asset_returns_none_when_reply_has_no_parameters(fake_engine):
    # The real engine only ever adds "parameters" for a "vst3" asset --
    # this is what a nam/ir register_asset reply actually looks like.
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)
    asset = Asset(kind=AssetKind.NAM, filename="amp.nam", stored_path="/data/assets/amp.nam")

    result = client.register_asset(asset)
    client.close()

    assert result is None


def test_register_asset_parses_vst3_parameters_from_the_reply(fake_engine):
    fake_engine.extra_reply_fields = {
        "parameters": [{"key": "0", "label": "Gain", "unit": "x", "min": 0.0, "max": 2.0, "default": 1.0, "step_count": 0}]
    }
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)
    asset = Asset(kind=AssetKind.VST3, filename="Test.vst3", stored_path="/plugins/Test.vst3")

    result = client.register_asset(asset)
    client.close()

    assert result == [BlockParamDescriptor(key="0", label="Gain", unit="x", min=0.0, max=2.0, default=1.0, step_count=0)]


def test_set_block_param_sends_matching_json(fake_engine):
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)

    client.set_block_param("boost", "gain_db", 6.0)
    client.close()

    assert len(fake_engine.received) == 1
    command = fake_engine.received[0]
    assert command["cmd"] == "set_block_param"
    assert command["block_id"] == "boost"
    assert command["param_key"] == "gain_db"
    assert command["value"] == 6.0


def test_list_block_types_returns_the_reply_field(fake_engine):
    fake_engine.extra_reply_fields = {"block_types": [{"type": "gain", "parameters": []}]}
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)

    result = client.list_block_types()
    client.close()

    assert result == [{"type": "gain", "parameters": []}]


def test_list_block_types_returns_empty_when_engine_unreachable(tmp_path: Path):
    client = UnixSocketAudioEngineClient(tmp_path / "no_such.sock", timeout=0.2)

    result = client.list_block_types()
    client.close()

    assert result == []


def test_rejection_raises_rather_than_being_swallowed(fake_engine):
    """A reachable engine that refuses is a different failure from an absent
    one: it means the engine is now playing something other than what the
    daemon believes, so it must not be logged and forgotten."""
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)
    fake_engine.reject_next = True

    with pytest.raises(AudioEngineRejected) as excinfo:
        client.load_preset(Preset(name="Ambient Swell"))
    client.close()

    assert excinfo.value.command == "load_preset"
    assert excinfo.value.message == "nope"


def test_unreachable_engine_still_fails_open():
    """The daemon must keep working with no engine listening at all -- that
    is what lets it run on a dev machine, and it is deliberately unchanged."""
    missing = Path(tempfile.gettempdir()) / f"ae-absent-{uuid.uuid4().hex[:8]}.sock"
    client = UnixSocketAudioEngineClient(missing, timeout=0.2)

    client.load_preset(Preset(name="Ambient Swell"))  # must not raise
    assert client.list_block_types() == []
    client.close()


def test_assets_are_re_registered_on_a_new_connection(fake_engine):
    """The regression that started this: the engine keeps its asset registry
    in memory, so a restart leaves it with none while the daemon still has
    them all -- and every load_preset referencing one then fails permanently
    with "unknown asset_id" until the user re-uploads the file."""
    assets = [
        Asset(kind=AssetKind.NAM, filename="amp.nam", stored_path="/data/assets/amp.nam"),
        Asset(kind=AssetKind.IR, filename="cab.wav", stored_path="/data/assets/cab.wav"),
    ]
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)
    client.set_asset_provider(lambda: assets)

    client.load_preset(Preset(name="A"))
    first = [c["cmd"] for c in fake_engine.connections[0]]
    assert first == ["register_asset", "register_asset", "load_preset"]

    # Simulate the engine process restarting underneath us.
    client.close()
    client.load_preset(Preset(name="A"))
    client.close()

    second = [c["cmd"] for c in fake_engine.connections[1]]
    assert second == ["register_asset", "register_asset", "load_preset"], (
        "a reconnect must replay the asset registry before using it"
    )
    replayed = {c["asset"]["id"] for c in fake_engine.connections[1] if c["cmd"] == "register_asset"}
    assert replayed == {a.id for a in assets}


def test_no_asset_provider_means_no_replay(fake_engine):
    """NullAudioEngineClient and any client without a provider must behave
    exactly as before -- the hook is optional."""
    client = UnixSocketAudioEngineClient(fake_engine.socket_path)
    client.load_preset(Preset(name="A"))
    client.close()
    assert [c["cmd"] for c in fake_engine.connections[0]] == ["load_preset"]
