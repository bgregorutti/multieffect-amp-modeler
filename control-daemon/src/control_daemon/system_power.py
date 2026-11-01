"""Rebooting or powering off the Pi on the app's request.

The daemon runs as an unprivileged user with NoNewPrivileges=true, so
``sudo`` is out (it is setuid, which that setting disables). Instead it asks
systemd-logind over D-Bus, the same way a desktop's "Restart" menu does:
``systemctl reboot`` run as a non-root user goes through logind, which
consults polkit. deploy/install.sh installs a polkit rule granting exactly
these two actions to the service user, and nothing else.

Before replying, the request is checked with logind's ``CanReboot`` /
``CanPowerOff``: "yes" only if it would actually go through without a
password prompt. The action itself runs a moment after the reply, so the
app gets its answer before the daemon is stopped by the shutdown.
"""

from __future__ import annotations

import logging
import shutil
import subprocess
import threading
from typing import Callable, Dict, Tuple

logger = logging.getLogger("control_daemon.system_power")

# action -> (logind "Can..." method, systemctl verb)
ACTIONS: Dict[str, Tuple[str, str]] = {
    "reboot": ("CanReboot", "reboot"),
    "poweroff": ("CanPowerOff", "poweroff"),
}


class SystemPowerError(Exception):
    pass


class SystemPower:
    def __init__(
        self,
        delay_s: float = 1.0,
        run: Callable[..., subprocess.CompletedProcess] = subprocess.run,
    ) -> None:
        self._delay_s = delay_s
        self._run = run

    def check(self, action: str) -> None:
        """Raise SystemPowerError unless ``action`` would be allowed."""
        method, _ = ACTIONS[action]
        if shutil.which("busctl") is None:
            raise SystemPowerError("not available on this machine (no systemd)")
        result = self._run(
            [
                "busctl", "call", "org.freedesktop.login1", "/org/freedesktop/login1",
                "org.freedesktop.login1.Manager", method,
            ],
            capture_output=True, text=True, timeout=5,
        )
        answer = result.stdout.strip()  # e.g. 's "yes"'
        if result.returncode != 0 or answer != 's "yes"':
            detail = answer or result.stderr.strip() or f"exit {result.returncode}"
            raise SystemPowerError(
                f"the pedal refused to {action} ({detail}) -- is the polkit rule from "
                "deploy/install.sh installed?"
            )

    def schedule(self, action: str) -> None:
        timer = threading.Timer(self._delay_s, self._execute, args=(action,))
        timer.daemon = True
        timer.start()

    def _execute(self, action: str) -> None:
        _, verb = ACTIONS[action]
        logger.warning("%s requested from the app", action)
        result = self._run(["systemctl", verb], capture_output=True, text=True, timeout=10)
        if result.returncode != 0:
            logger.error("systemctl %s failed: %s", verb, result.stderr.strip())
