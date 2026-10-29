"""The display client's event loop.

Three concurrent jobs share one small piece of state:

* the WebSocket task holds a ``role: "display"`` connection to the daemon
  (read-only: it never sends anything after ``hello``) and stores every
  ``state_snapshot`` / ``state_changed`` it receives, reconnecting forever;
* the boot task polls systemd a few times a second until boot is complete,
  then stops -- after that nothing polls;
* the render task redraws only when one of the above marks the screen
  dirty, so an idle pedal costs no CPU and no SPI traffic.

Shutdown is detected two ways: the connection dropping while systemd
reports ``stopping`` (the daemon is stopped first, so this is the early
signal), and this process's own SIGTERM (it is ordered to stop last).
Either way the "shutting down" frame is left on the panel, which keeps its
last image until the power board cuts the supply.
"""

from __future__ import annotations

import asyncio
import json
import logging
import signal
import time
from typing import Optional

from .boot import UNKNOWN, BootStatus, SmoothProgress, SystemdProbe, UnitState, boot_complete, boot_status
from .config import Config
from .framebuffer import Sink
from .render import Renderer, render_screen
from .view import LiveView, live_view

logger = logging.getLogger("display_client.app")

BOOT_FRAME_INTERVAL_S = 0.2
RECONNECT_DELAYS_S = (0.25, 0.5, 1.0, 2.0)


class DisplayApp:
    def __init__(self, config: Config, sink: Sink, probe: Optional[SystemdProbe] = None) -> None:
        self.config = config
        self.sink = sink
        self.probe = probe or SystemdProbe()
        self.renderer = Renderer(sink.size)

        self.view: Optional[LiveView] = None
        self.connected = False
        self.booting = True
        self.shutting_down = False
        self.engine: UnitState = UNKNOWN
        self.daemon: UnitState = UNKNOWN
        self.boot: BootStatus = boot_status(UNKNOWN, UNKNOWN, False)
        self.progress = SmoothProgress()

        self._dirty = asyncio.Event()
        self._stop = asyncio.Event()

    # -- state ---------------------------------------------------------------

    def _mark_dirty(self) -> None:
        self._dirty.set()

    def apply_message(self, message: dict) -> None:
        if message.get("type") in ("state_snapshot", "state_changed"):
            self.view = live_view(message.get("state") or {})
            self.connected = True
            self._mark_dirty()

    def draw(self) -> None:
        image = render_screen(
            self.renderer,
            shutting_down=self.shutting_down,
            booting=self.booting,
            boot=self.boot,
            boot_fraction=self.progress.value,
            view=self.view,
            connected=self.connected,
        )
        self.sink.show(image)

    # -- tasks ---------------------------------------------------------------

    async def _websocket_loop(self) -> None:
        # Imported here, not at module level, so the first boot frame is on
        # the panel before paying for this import on a cold Pi.
        import websockets

        attempt = 0
        while not self._stop.is_set():
            try:
                async with websockets.connect(self.config.uri, open_timeout=3) as ws:
                    await ws.send(json.dumps(
                        {"type": "hello", "role": "display", "client_name": "onboard-display"}
                    ))
                    attempt = 0
                    async for raw in ws:
                        self.apply_message(json.loads(raw))
            except (OSError, asyncio.TimeoutError, websockets.exceptions.WebSocketException) as exc:
                logger.debug("daemon connection: %s", exc)
            except json.JSONDecodeError as exc:
                logger.warning("unparseable message from daemon: %s", exc)

            if self.connected:
                logger.info("lost connection to the control daemon")
                self.connected = False
                if await self.probe.system_state() == "stopping":
                    self.shutting_down = True
                self._mark_dirty()
            delay = RECONNECT_DELAYS_S[min(attempt, len(RECONNECT_DELAYS_S) - 1)]
            attempt += 1
            try:
                await asyncio.wait_for(self._stop.wait(), delay)
            except asyncio.TimeoutError:
                pass

    async def _boot_loop(self) -> None:
        started = time.monotonic()
        last = started
        next_poll = 0.0
        while self.booting and not self._stop.is_set():
            now = time.monotonic()
            if now >= next_poll:
                states = await self.probe.unit_states(self.config.engine_unit, self.config.daemon_unit)
                self.engine = states[self.config.engine_unit]
                self.daemon = states[self.config.daemon_unit]
                next_poll = now + 0.5
            self.boot = boot_status(self.engine, self.daemon, self.connected)
            self.progress.update(self.boot.fraction, now - last)
            last = now
            if boot_complete(self.engine, self.connected):
                logger.info("boot complete after %.1fs (userspace)", now - started)
                self.booting = False
            self._mark_dirty()
            await asyncio.sleep(BOOT_FRAME_INTERVAL_S)

    async def _render_loop(self) -> None:
        while not self._stop.is_set():
            await self._dirty.wait()
            self._dirty.clear()
            try:
                self.draw()
            except OSError as exc:
                logger.error("failed to write frame: %s", exc)

    # -- lifecycle -----------------------------------------------------------

    def request_shutdown(self) -> None:
        """SIGTERM: leave the shutdown frame on screen and exit."""
        self.shutting_down = True
        try:
            self.draw()
        except OSError:
            pass
        self._stop.set()

    async def run(self) -> None:
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                loop.add_signal_handler(sig, self.request_shutdown)
            except (NotImplementedError, RuntimeError):
                pass

        self.draw()  # first frame immediately, before any I/O
        tasks = [
            asyncio.create_task(self._render_loop()),
            asyncio.create_task(self._boot_loop()),
            asyncio.create_task(self._websocket_loop()),
        ]
        await self._stop.wait()
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
