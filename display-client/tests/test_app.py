"""End to end against a stand-in daemon: a real WebSocket server speaking
the daemon's display-role protocol."""

import asyncio
import json

import websockets

from display_client.app import DisplayApp
from display_client.boot import UnitState
from display_client.config import Config

ACTIVE = UnitState(active="active")


class _Capture:
    size = (320, 240)

    def __init__(self):
        self.frames = []

    def show(self, image):
        self.frames.append(image)

    def close(self):
        pass


class _Probe:
    def __init__(self, units=ACTIVE, system="running"):
        self.units = units
        self.system = system

    async def unit_states(self, *units):
        return {unit: self.units for unit in units}

    async def system_state(self):
        return self.system


def _state(preset_index):
    return {
        "active_rig_index": 0,
        "active_preset_index": preset_index,
        "rigs": [{"name": "Rig", "chain": [], "presets": [{"name": "One"}, {"name": "Two"}]}],
    }


async def _wait_for(predicate, timeout=5.0):
    deadline = asyncio.get_running_loop().time() + timeout
    while not predicate():
        assert asyncio.get_running_loop().time() < deadline, "timed out"
        await asyncio.sleep(0.02)


async def _serve(handler):
    server = await websockets.serve(handler, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    return server, f"ws://127.0.0.1:{port}/ws"


async def test_hello_as_display_then_follows_broadcasts():
    hellos = []
    push = asyncio.Queue()

    async def handler(ws, *_):
        hellos.append(json.loads(await ws.recv()))
        await ws.send(json.dumps({"type": "state_snapshot", "state": _state(0)}))
        while True:
            get = asyncio.ensure_future(push.get())
            closed = asyncio.ensure_future(ws.wait_closed())
            done, _ = await asyncio.wait({get, closed}, return_when=asyncio.FIRST_COMPLETED)
            if closed in done:
                get.cancel()
                return
            closed.cancel()
            await ws.send(json.dumps(get.result()))

    server, uri = await _serve(handler)
    sink = _Capture()
    app = DisplayApp(Config(uri=uri), sink, probe=_Probe())
    task = asyncio.create_task(app.run())
    try:
        await _wait_for(lambda: not app.booting and app.view is not None)
        assert hellos == [{"type": "hello", "role": "display", "client_name": "onboard-display"}]
        assert app.view.preset_name == "One"

        frames_before = len(sink.frames)
        await push.put({"type": "state_changed", "state": _state(1), "reason": "footswitch"})
        await _wait_for(lambda: app.view.preset_name == "Two")
        await _wait_for(lambda: len(sink.frames) > frames_before)

        # Idle: no new state means no redraw.
        settled = len(sink.frames)
        await asyncio.sleep(0.5)
        assert len(sink.frames) == settled
    finally:
        app.request_shutdown()
        await task
        server.close()
        await server.wait_closed()


async def test_disconnect_while_system_stopping_shows_shutdown():
    async def handler(ws, *_):
        await ws.recv()
        await ws.send(json.dumps({"type": "state_snapshot", "state": _state(0)}))
        await ws.close()

    server, uri = await _serve(handler)
    probe = _Probe(system="stopping")
    app = DisplayApp(Config(uri=uri), _Capture(), probe=probe)
    task = asyncio.create_task(app.run())
    try:
        await _wait_for(lambda: app.shutting_down)
    finally:
        app.request_shutdown()
        await task
        server.close()
        await server.wait_closed()


async def test_disconnect_while_running_keeps_last_state_offline():
    connections = 0

    async def handler(ws, *_):
        nonlocal connections
        connections += 1
        await ws.recv()
        await ws.send(json.dumps({"type": "state_snapshot", "state": _state(1)}))
        if connections == 1:
            await ws.close()
        else:
            await ws.wait_closed()

    server, uri = await _serve(handler)
    app = DisplayApp(Config(uri=uri), _Capture(), probe=_Probe())
    task = asyncio.create_task(app.run())
    try:
        await _wait_for(lambda: connections >= 2 and app.connected)
        assert not app.shutting_down
        assert app.view.preset_name == "Two"
    finally:
        app.request_shutdown()
        await task
        server.close()
        await server.wait_closed()


async def test_boot_screen_until_engine_ready():
    async def handler(ws, *_):
        await ws.recv()
        await ws.send(json.dumps({"type": "state_snapshot", "state": _state(0)}))
        await ws.wait_closed()

    server, uri = await _serve(handler)
    probe = _Probe(units=UnitState(active="activating"))
    app = DisplayApp(Config(uri=uri), _Capture(), probe=probe)
    task = asyncio.create_task(app.run())
    try:
        await _wait_for(lambda: app.connected)
        await asyncio.sleep(0.5)
        assert app.booting
        probe.units = ACTIVE
        await _wait_for(lambda: not app.booting)
    finally:
        app.request_shutdown()
        await task
        server.close()
        await server.wait_closed()
