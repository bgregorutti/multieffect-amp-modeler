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
CONTROL_DAEMON_FOOTSWITCH_PINS     physical footswitches, as comma-separated
                                    switch_index:BCM_pin pairs, e.g.
                                    "0:5,1:12,2:6,3:13". Each switch connects
                                    its pin to ground. Unset or empty = no
                                    GPIO footswitches.
"""

from __future__ import annotations

import logging
import os
from pathlib import Path
from typing import Dict, Optional

import uvicorn

from . import sd_notify
from .app import create_app
from .audio_engine_client import AudioEngineClient, NullAudioEngineClient, UnixSocketAudioEngineClient
from .gpio import FootswitchInputBackend, GpioZeroFootswitchBackend

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


def parse_footswitch_pins(spec: str) -> Dict[int, int]:
    """``"0:5,1:12"`` -> ``{0: 5, 1: 12}``. Raises ValueError on a malformed
    pair or a pin used twice."""
    pins: Dict[int, int] = {}
    for pair in spec.split(","):
        pair = pair.strip()
        if not pair:
            continue
        index, sep, pin = pair.partition(":")
        if not sep:
            raise ValueError(f"expected switch_index:pin, got {pair!r}")
        pins[int(index)] = int(pin)
    if len(set(pins.values())) != len(pins):
        raise ValueError(f"a GPIO pin is used by more than one switch in {spec!r}")
    return pins


def _build_footswitch_backend() -> Optional[FootswitchInputBackend]:
    spec = os.environ.get("CONTROL_DAEMON_FOOTSWITCH_PINS", "").strip()
    if not spec:
        return None
    # A bad pin list or a missing GPIO library must not take the whole
    # daemon down with it: the app still works without the switches.
    try:
        pins = parse_footswitch_pins(spec)
        backend = GpioZeroFootswitchBackend(pins)
    except (ValueError, RuntimeError) as exc:
        logger.error("footswitches disabled: %s", exc)
        return None
    logger.info("footswitches on GPIO %s", ", ".join(f"{i}->{p}" for i, p in sorted(pins.items())))
    return backend


def _build_app():
    store_path = Path(
        os.environ.get("CONTROL_DAEMON_STORE_PATH", "./data/state.json")
    ).resolve()
    return create_app(
        store_path=store_path,
        audio_engine=_build_audio_engine(),
        footswitch_backend=_build_footswitch_backend(),
    )


# Importable ASGI app target, e.g. `uvicorn control_daemon.main:app`.
app = None


def run() -> None:
    global app
    host = os.environ.get("CONTROL_DAEMON_HOST", "0.0.0.0")
    port = int(os.environ.get("CONTROL_DAEMON_PORT", "8765"))
    log_level = os.environ.get("CONTROL_DAEMON_LOG_LEVEL", "info")
    # uvicorn only configures its own loggers: without this, every INFO line
    # of the daemon itself (clients connecting and leaving, slow commands)
    # was silently dropped. Before _build_app, which already logs.
    logging.basicConfig(level=log_level.upper(), format="%(levelname)s %(name)s: %(message)s")
    app = _build_app()
    logger.info("starting control daemon on %s:%s", host, port)
    _NotifyingServer(uvicorn.Config(
        app, host=host, port=port, log_level=log_level,
        # No server pings. On the Pi's hotspot a phone or tablet on Wi-Fi
        # power saving answers them late, and the daemon then cut every app
        # connection after exactly interval + timeout (40 s with uvicorn's
        # 20 s + 20 s defaults, 20 s with 10 s + 10 s) although commands were
        # flowing fine. Dead clients are handled without them: every send is
        # time-limited and drops a client that doesn't take it (see
        # ConnectionManager.send), and the app reconnects itself when a
        # command gets no reply.
        ws_ping_interval=None,
    )).run()


if __name__ == "__main__":
    run()
