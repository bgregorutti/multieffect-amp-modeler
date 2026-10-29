"""Configuration, read from environment variables (set by the systemd unit).

DISPLAY_CLIENT_URI          control daemon WebSocket (default: ws://127.0.0.1:8765/ws)
DISPLAY_CLIENT_OUTPUT       "auto" (default: first SPI/non-HDMI framebuffer),
                            an explicit device such as "/dev/fb1", or
                            "png:DIR" to write PNG frames instead (dev machine)
DISPLAY_CLIENT_WIDTH        logical screen size in pixels, after rotation.
DISPLAY_CLIENT_HEIGHT       Default: the framebuffer's own size. Set both to
                            switch between the 480x320 and 320x240 panels
                            without touching code (spec section 4.4).
DISPLAY_CLIENT_ROTATE       0/90/180/270, clockwise, applied in software on
                            top of whatever the overlay's own rotate= does
DISPLAY_CLIENT_ENGINE_UNIT  systemd unit polled for boot progress
DISPLAY_CLIENT_DAEMON_UNIT  (defaults: the units deploy/install.sh installs)
DISPLAY_CLIENT_LOG_LEVEL    python logging level (default: INFO)
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Mapping, Optional

DEFAULT_URI = "ws://127.0.0.1:8765/ws"
DEFAULT_ENGINE_UNIT = "multieffect-audio-engine.service"
DEFAULT_DAEMON_UNIT = "multieffect-control-daemon.service"


@dataclass(frozen=True)
class Config:
    uri: str = DEFAULT_URI
    output: str = "auto"
    width: Optional[int] = None
    height: Optional[int] = None
    rotate: int = 0
    engine_unit: str = DEFAULT_ENGINE_UNIT
    daemon_unit: str = DEFAULT_DAEMON_UNIT
    log_level: str = "INFO"

    @classmethod
    def from_env(cls, env: Mapping[str, str] = os.environ) -> "Config":
        def opt_int(key: str) -> Optional[int]:
            value = env.get(key, "").strip()
            return int(value) if value else None

        rotate = opt_int("DISPLAY_CLIENT_ROTATE") or 0
        if rotate not in (0, 90, 180, 270):
            raise ValueError(f"DISPLAY_CLIENT_ROTATE must be 0/90/180/270, got {rotate}")
        width = opt_int("DISPLAY_CLIENT_WIDTH")
        height = opt_int("DISPLAY_CLIENT_HEIGHT")
        if (width is None) != (height is None):
            raise ValueError("set both DISPLAY_CLIENT_WIDTH and DISPLAY_CLIENT_HEIGHT, or neither")

        return cls(
            uri=env.get("DISPLAY_CLIENT_URI", DEFAULT_URI),
            output=env.get("DISPLAY_CLIENT_OUTPUT", "auto"),
            width=width,
            height=height,
            rotate=rotate,
            engine_unit=env.get("DISPLAY_CLIENT_ENGINE_UNIT", DEFAULT_ENGINE_UNIT),
            daemon_unit=env.get("DISPLAY_CLIENT_DAEMON_UNIT", DEFAULT_DAEMON_UNIT),
            log_level=env.get("DISPLAY_CLIENT_LOG_LEVEL", "INFO"),
        )
