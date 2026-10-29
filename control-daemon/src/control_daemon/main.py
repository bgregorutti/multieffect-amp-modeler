"""Entrypoint: reads config from environment variables and runs the daemon
with uvicorn.

Environment variables
----------------------
CONTROL_DAEMON_STORE_PATH          path to the JSON state file (default: ./data/state.json)
CONTROL_DAEMON_HOST                bind host (default: 0.0.0.0)
CONTROL_DAEMON_PORT                bind port (default: 8765)
CONTROL_DAEMON_LOG_LEVEL           uvicorn log level (default: info)
CONTROL_DAEMON_AUDIO_ENGINE_SOCKET path to the audio-engine control socket
                                    (see audio-engine/README.md). If unset,
                                    the daemon runs with NullAudioEngineClient
                                    (no real engine required) -- see
                                    audio_engine_client.py.
"""

from __future__ import annotations

import logging
import os
from pathlib import Path

import uvicorn

from . import sd_notify
from .app import create_app
from .audio_engine_client import AudioEngineClient, NullAudioEngineClient, UnixSocketAudioEngineClient

logger = logging.getLogger("control_daemon.main")


class _NotifyingServer(uvicorn.Server):
    """Tells systemd ``READY=1`` only once the listening socket is bound --
    uvicorn runs the app's lifespan startup *before* binding, so notifying
    from the lifespan would report ready while connections still fail."""

    async def startup(self, sockets=None) -> None:
        await super().startup(sockets=sockets)
        if not self.should_exit:
            sd_notify.notify("READY=1")

    async def shutdown(self, sockets=None) -> None:
        sd_notify.notify("STOPPING=1")
        await super().shutdown(sockets=sockets)


def _build_audio_engine() -> AudioEngineClient:
    socket_path = os.environ.get("CONTROL_DAEMON_AUDIO_ENGINE_SOCKET")
    if not socket_path:
        return NullAudioEngineClient()
    logger.info("connecting to audio engine over control socket %s", socket_path)
    return UnixSocketAudioEngineClient(socket_path)


def _build_app():
    store_path = Path(
        os.environ.get("CONTROL_DAEMON_STORE_PATH", "./data/state.json")
    ).resolve()
    return create_app(store_path=store_path, audio_engine=_build_audio_engine())


# Importable ASGI app target, e.g. `uvicorn control_daemon.main:app`.
app = None


def run() -> None:
    global app
    app = _build_app()
    host = os.environ.get("CONTROL_DAEMON_HOST", "0.0.0.0")
    port = int(os.environ.get("CONTROL_DAEMON_PORT", "8765"))
    log_level = os.environ.get("CONTROL_DAEMON_LOG_LEVEL", "info")
    logger.info("starting control daemon on %s:%s", host, port)
    _NotifyingServer(uvicorn.Config(app, host=host, port=port, log_level=log_level)).run()


if __name__ == "__main__":
    run()
