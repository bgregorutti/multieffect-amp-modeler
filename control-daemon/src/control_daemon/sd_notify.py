"""Minimal systemd ``sd_notify`` -- no libsystemd/python-systemd dependency.

The unit is ``Type=notify`` (spec section 5.3): systemd keeps it in
``activating`` until ``READY=1`` arrives, which is what lets the onboard
display show real boot progress (it polls the unit's ActiveState) and what
orders anything that must wait for the WS API to actually be listening.

A no-op returning ``False`` when ``NOTIFY_SOCKET`` is unset, i.e. whenever
the daemon isn't started by systemd (dev machine, tests).
"""

from __future__ import annotations

import os
import socket


def notify(state: str) -> bool:
    address = os.environ.get("NOTIFY_SOCKET")
    if not address:
        return False
    if address.startswith("@"):  # abstract namespace socket
        address = "\0" + address[1:]
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
            sock.connect(address)
            sock.sendall(state.encode())
    except OSError:
        return False
    return True
