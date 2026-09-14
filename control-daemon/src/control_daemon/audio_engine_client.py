"""Abstraction over the audio engine (``audio-engine/``, a sibling package
in this repo -- see its README for the C++ side).

The daemon calls an ``AudioEngineClient`` on every operation that should
affect what's actually playing (loading a preset, toggling bypass, tempo
updates, registering a newly-uploaded asset). ``NullAudioEngineClient`` is
the default (no real engine process needed -- keeps the daemon fully
testable on its own); ``UnixSocketAudioEngineClient`` below is the real
implementation, talking to ``audio-engine``'s control socket -- see
"Control socket protocol" in audio-engine/README.md for the wire format
this mirrors.
"""

from __future__ import annotations

import json
import logging
import socket
import threading
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Union

from .models import Asset, ResolvedPreset

logger = logging.getLogger("control_daemon.audio_engine")


class AudioEngineClient(ABC):
    @abstractmethod
    def load_preset(self, preset: ResolvedPreset) -> None:
        """Load and apply the given resolved chain.

        The engine is handed an already-flattened ``ResolvedPreset`` (rig
        chain + the active preset's overrides applied), so it never needs to
        know about rigs, presets or pinned blocks -- see models.py.
        """

    @abstractmethod
    def set_bypass(self, bypass: bool) -> None:
        """Engage/disengage global bypass."""

    @abstractmethod
    def set_tempo(self, bpm: float) -> None:
        """Update the engine's tempo (e.g. for tempo-synced effects)."""

    @abstractmethod
    def register_asset(self, asset: Asset) -> None:
        """Register a newly-uploaded .nam/IR asset's metadata so a later
        load_preset() referencing its id can resolve it."""


class NullAudioEngineClient(AudioEngineClient):
    """No-op engine client used when no real audio engine is configured.

    Every call is logged at INFO level so behavior is observable in dev and
    in tests without requiring an actual engine process.
    """

    def load_preset(self, preset: ResolvedPreset) -> None:
        logger.info("load_preset(id=%s, name=%r)", preset.id, preset.name)

    def set_bypass(self, bypass: bool) -> None:
        logger.info("set_bypass(%s)", bypass)

    def set_tempo(self, bpm: float) -> None:
        logger.info("set_tempo(%.2f bpm)", bpm)

    def register_asset(self, asset: Asset) -> None:
        logger.info("register_asset(id=%s, kind=%s)", asset.id, asset.kind)


class UnixSocketAudioEngineClient(AudioEngineClient):
    """Talks to a real ``audio_engine`` process over its Unix domain
    control socket (newline-delimited JSON, one command per line in, one
    reply per line out -- see audio-engine/README.md).

    Deliberately resilient to the engine process being absent or
    restarting: every call opens (or reuses) a connection, and any
    connection/protocol failure is logged and swallowed rather than raised.
    The daemon is the source of truth for preset/bank/footswitch state
    regardless of whether an engine is currently listening -- e.g. during
    early bring-up on a dev machine before the engine binary is running, or
    if it crashes and is being restarted -- so a broken engine link must
    not take down the WS API or leave state inconsistent with what was
    actually persisted. This mirrors the same fail-open posture
    NullAudioEngineClient has today, just with a real socket now able to
    fail.

    No ``set_tempo`` command exists in the engine's control-socket protocol
    -- tempo isn't wired to any real effect yet (see TapTempoAction's
    docstring in models.py) -- so that call is a log-only no-op here, same
    as NullAudioEngineClient, until the engine grows one.
    """

    def __init__(self, socket_path: Union[str, Path], timeout: float = 2.0) -> None:
        self.socket_path = str(socket_path)
        self.timeout = timeout
        self._lock = threading.Lock()
        self._sock: socket.socket | None = None

    def load_preset(self, preset: ResolvedPreset) -> None:
        self._send({"cmd": "load_preset", "preset": preset.model_dump(mode="json")})

    def set_bypass(self, bypass: bool) -> None:
        self._send({"cmd": "set_bypass", "bypass": bypass})

    def set_tempo(self, bpm: float) -> None:
        logger.info("set_tempo(%.2f bpm) -- no-op, engine has no tempo command yet", bpm)

    def register_asset(self, asset: Asset) -> None:
        self._send(
            {
                "cmd": "register_asset",
                "asset": {
                    "id": asset.id,
                    "kind": asset.kind.value,
                    "filename": asset.filename,
                    "stored_path": asset.stored_path,
                    "size_bytes": asset.size_bytes,
                    "sha256": asset.sha256,
                },
            }
        )

    def close(self) -> None:
        with self._lock:
            self._close_locked()

    # -- socket plumbing ----------------------------------------------------

    def _send(self, command: dict) -> None:
        line = (json.dumps(command) + "\n").encode("utf-8")
        with self._lock:
            for attempt in (1, 2):
                try:
                    sock = self._ensure_connected_locked()
                    sock.sendall(line)
                    reply_line = self._readline_locked(sock)
                    reply = json.loads(reply_line) if reply_line else {}
                    if not reply.get("ok", False):
                        logger.warning(
                            "audio-engine rejected %s: %s",
                            command.get("cmd"),
                            reply.get("message", reply),
                        )
                    return
                except OSError as exc:
                    logger.warning(
                        "audio-engine socket error on %s (attempt %d): %s",
                        command.get("cmd"),
                        attempt,
                        exc,
                    )
                    self._close_locked()
            logger.warning(
                "audio-engine unreachable at %s -- dropping %s",
                self.socket_path,
                command.get("cmd"),
            )

    def _ensure_connected_locked(self) -> socket.socket:
        if self._sock is not None:
            return self._sock
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(self.timeout)
        sock.connect(self.socket_path)
        self._sock = sock
        return sock

    def _readline_locked(self, sock: socket.socket) -> bytes:
        buf = bytearray()
        while not buf.endswith(b"\n"):
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("audio-engine closed the connection")
            buf.extend(chunk)
        return bytes(buf)

    def _close_locked(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
