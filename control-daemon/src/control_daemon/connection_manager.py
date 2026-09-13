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


class ConnectionManager:
    def __init__(self) -> None:
        self._roles: Dict[WebSocket, str] = {}
        self._lock = asyncio.Lock()

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

    async def broadcast(self, message: dict) -> None:
        async with self._lock:
            targets: List[WebSocket] = list(self._roles.keys())

        stale: List[WebSocket] = []
        for ws in targets:
            try:
                await ws.send_json(message)
            except Exception:
                # Client disconnected between snapshot and send -- drop it,
                # the receive loop for that socket will also clean itself up.
                stale.append(ws)

        if stale:
            async with self._lock:
                for ws in stale:
                    self._roles.pop(ws, None)

    @property
    def connection_count(self) -> int:
        return len(self._roles)
