"""Tracks connected WebSocket clients and their declared role, and provides
a broadcast helper so every connected UI (app, footswitch relay, display)
stays in sync whenever daemon state changes -- including changes triggered
by the footswitch itself.
"""

from __future__ import annotations

import asyncio
import logging
from typing import Dict, List, Optional

from fastapi import WebSocket

logger = logging.getLogger("control_daemon.connections")


# How long one client may take to accept a message. A phone whose Wi-Fi
# link died silently (a hotspot dropping it, the OS switching it to mobile
# data) leaves a socket that never closes and eventually stops draining:
# without a limit, the send to it never returns, and everything queued
# behind it -- every other client, and every later command -- waits too.
SEND_TIMEOUT_S = 2.0


class ConnectionManager:
    def __init__(self, send_timeout_s: float = SEND_TIMEOUT_S) -> None:
        self._roles: Dict[WebSocket, str] = {}
        self._lock = asyncio.Lock()
        self._send_timeout_s = send_timeout_s

    async def connect(self, ws: WebSocket) -> None:
        await ws.accept()

    async def set_role(self, ws: WebSocket, role: str) -> None:
        async with self._lock:
            self._roles[ws] = role

    def role_of(self, ws: WebSocket) -> Optional[str]:
        return self._roles.get(ws)

    async def disconnect(self, ws: WebSocket) -> None:
        async with self._lock:
            self._roles.pop(ws, None)

    async def send(self, ws: WebSocket, message: dict) -> bool:
        """Send to one client, giving up after the send timeout. Returns
        False if the client didn't take it: it is then dropped and closed,
        and the client's own receive loop ends."""
        try:
            await asyncio.wait_for(ws.send_json(message), self._send_timeout_s)
            return True
        except Exception as exc:
            # Includes the client having disconnected between snapshot and
            # send, and a send that timed out (a dead link).
            logger.warning(
                "dropping client %s (role %s): send failed: %s",
                _peer(ws), self._roles.get(ws), type(exc).__name__,
            )
            await self._drop(ws)
            return False

    async def broadcast(self, message: dict) -> None:
        async with self._lock:
            targets: List[WebSocket] = list(self._roles.keys())
        # Concurrently, so one dead client costs one timeout rather than
        # holding up every client after it in the list.
        await asyncio.gather(*(self.send(ws, message) for ws in targets))

    async def _drop(self, ws: WebSocket) -> None:
        async with self._lock:
            self._roles.pop(ws, None)
        try:
            # Closing a dead socket can hang too; the receive loop for it
            # ends once it is closed.
            await asyncio.wait_for(ws.close(), self._send_timeout_s)
        except Exception:
            pass

    @property
    def connection_count(self) -> int:
        return len(self._roles)


def _peer(ws: WebSocket) -> str:
    client = ws.client
    return f"{client.host}:{client.port}" if client else "?"
