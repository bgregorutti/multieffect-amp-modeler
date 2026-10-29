"""Entrypoint: build the output sink from the environment and run.

See config.py for the environment variables.
"""

from __future__ import annotations

import asyncio
import logging
import sys
from pathlib import Path

from .app import DisplayApp
from .config import Config
from .framebuffer import FramebufferSink, PngSink, RotatedSink, Sink, wait_for_framebuffer

logger = logging.getLogger("display_client.main")


def build_sink(config: Config) -> Sink:
    if config.output.startswith("png:"):
        size = (config.width or 480, config.height or 320)
        return PngSink(Path(config.output[4:]), size)

    device = config.output
    if device == "auto":
        device = wait_for_framebuffer(timeout_s=30.0)
        if device is None:
            raise SystemExit("no framebuffer found -- is the LCD overlay enabled? (see display-client/README.md)")
    logical = (config.width, config.height) if config.width else None
    return RotatedSink(FramebufferSink(device), config.rotate, logical)


def run() -> None:
    config = Config.from_env()
    logging.basicConfig(
        level=config.log_level.upper(),
        format="%(levelname)s %(name)s: %(message)s",
        stream=sys.stderr,
    )
    sink = build_sink(config)
    logger.info("rendering %dx%d, daemon at %s", *sink.size, config.uri)
    try:
        asyncio.run(DisplayApp(config, sink).run())
    finally:
        sink.close()


if __name__ == "__main__":
    run()
