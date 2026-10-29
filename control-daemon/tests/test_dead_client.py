"""A client whose link died silently must not hold up the others."""

import asyncio
import time

from control_daemon.connection_manager import ConnectionManager


class _Ws:
    client = None

    def __init__(self, hang: bool = False) -> None:
        self.hang = hang
        self.received: list = []
        self.closed = False

    async def send_json(self, message: dict) -> None:
        if self.hang:
            await asyncio.Event().wait()  # a dead peer: the send never completes
        self.received.append(message)

    async def close(self) -> None:
        self.closed = True
        if self.hang:
            await asyncio.Event().wait()


async def test_broadcast_survives_a_client_that_never_accepts_data():
    manager = ConnectionManager(send_timeout_s=0.2)
    dead, display, phone = _Ws(hang=True), _Ws(), _Ws()
    for ws, role in ((dead, "app"), (display, "display"), (phone, "app")):
        await manager.set_role(ws, role)

    started = time.monotonic()
    await manager.broadcast({"type": "state_changed"})
    elapsed = time.monotonic() - started

    # Clients after the dead one in the list still get it, in one timeout's
    # time rather than waiting forever.
    assert display.received and phone.received
    assert elapsed < 1.0
    # And it is gone, so the next broadcast doesn't pay for it again.
    assert manager.connection_count == 2
    assert dead.closed


async def test_send_reports_failure_for_a_dead_client():
    manager = ConnectionManager(send_timeout_s=0.2)
    dead = _Ws(hang=True)
    await manager.set_role(dead, "app")
    assert await manager.send(dead, {"type": "command_ok"}) is False
    assert await manager.send(_Ws(), {"type": "command_ok"}) is True
