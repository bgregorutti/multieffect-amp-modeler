"""Boot progress, derived from real service readiness (spec section 5.3/5.4).

Both services are ``Type=notify``: systemd keeps a unit in ``activating``
until its process sends ``READY=1`` (the audio engine once its control
socket and audio stream are open, the daemon once uvicorn is listening), so
a unit's ``ActiveState`` is an honest "is it ready" signal. This module
turns those states -- plus "have we received the daemon's first state
snapshot" -- into a bar fraction and a label.

A unit that keeps crashing never shows up as ``failed``: between
``Restart=`` attempts systemd reports it as ``activating`` (sub-state
``auto-restart``) for as long as the start rate limit isn't hit. The audio
engine does exactly that when its interface isn't plugged in, so a
restarting unit counts as settled (with a warning), and the boot screen
stops waiting for the engine after ``ENGINE_WAIT_S`` regardless.

Only the userspace part of the boot can be shown: the panel is blank until
the kernel has loaded its overlay, and this client is itself a service.
"""

from __future__ import annotations

import asyncio
import logging
import shutil
from dataclasses import dataclass
from typing import Dict, Iterable, Optional

logger = logging.getLogger("display_client.boot")

# Steps the bar is divided into. The first one (this client is running) is
# already complete when the first frame is drawn.
TOTAL_STEPS = 4

# Once the daemon's state has arrived, how long to keep waiting for an
# engine that is neither ready nor visibly failing (e.g. hung opening an
# audio device) before showing the live screen anyway.
ENGINE_WAIT_S = 15.0

# A step taking longer than this shows its elapsed time, so a stuck boot is
# visibly stuck rather than looking like a slow bar.
SHOW_ELAPSED_AFTER_S = 5.0


@dataclass(frozen=True)
class UnitState:
    load: str = "loaded"  # systemd LoadState: "loaded", "not-found", ...
    active: str = "inactive"  # systemd ActiveState
    sub: str = ""  # systemd SubState: "running", "auto-restart", ...

    @property
    def missing(self) -> bool:
        return self.load == "not-found"

    @property
    def ready(self) -> bool:
        return self.active == "active"

    @property
    def failed(self) -> bool:
        return self.active == "failed"

    @property
    def restarting(self) -> bool:
        """Crashed and waiting for its next ``Restart=`` attempt."""
        return self.sub == "auto-restart"

    @property
    def settled(self) -> bool:
        """Nothing more to wait for: ready, failed, crash-looping, or not
        installed."""
        return self.ready or self.failed or self.restarting or self.missing


UNKNOWN = UnitState(load="unknown", active="unknown", sub="unknown")


@dataclass(frozen=True)
class BootStatus:
    fraction: float
    label: str
    warning: Optional[str] = None
    error: Optional[str] = None
    elapsed_s: Optional[int] = None  # time spent on the current step, once it's slow


def boot_status(
    engine: UnitState, daemon: UnitState, connected: bool, step_s: float = 0.0
) -> BootStatus:
    """``step_s``: how long the boot has been on the current step."""
    done = 1
    done += engine.settled
    done += (daemon.ready or daemon.failed or daemon.missing) or connected
    done += connected

    warning = None
    if engine.failed or engine.restarting:
        warning = "Audio engine not running - is the interface plugged in?"
    if daemon.restarting and not connected:
        warning = "Control daemon is restarting"
    elapsed = int(step_s) if step_s >= SHOW_ELAPSED_AFTER_S else None
    if daemon.failed and not connected:
        return BootStatus(done / TOTAL_STEPS, "Control daemon failed", warning,
                          "journalctl -u multieffect-control-daemon")
    if not engine.settled:
        label = "Starting audio engine"
    elif not ((daemon.ready or daemon.failed or daemon.missing) or connected):
        label = "Starting control daemon"
    elif not connected:
        label = "Connecting to control daemon"
    else:
        label = "Ready"
    return BootStatus(done / TOTAL_STEPS, label, warning, elapsed_s=None if label == "Ready" else elapsed)


def boot_complete(engine: UnitState, connected: bool, connected_s: float = 0.0) -> bool:
    """Leave the boot screen once the daemon's state has arrived and the
    engine has finished starting one way or the other -- so the first live
    screen isn't followed a second later by the sound coming on. An engine
    that is still starting ``ENGINE_WAIT_S`` after the daemon connected
    (``connected_s``) isn't waited for any longer: the live screen works
    without it."""
    return connected and (engine.settled or engine == UNKNOWN or connected_s >= ENGINE_WAIT_S)


class SmoothProgress:
    """Eases the drawn bar toward the real fraction, and creeps forward
    inside a step that is taking a while, so a slow step still looks alive
    without ever overtaking the next real milestone."""

    def __init__(self, creep_per_s: float = 0.25, ease: float = 0.35) -> None:
        self.value = 0.0
        self._creep_per_s = creep_per_s
        self._ease = ease
        self._target = 0.0
        self._since_target = 0.0

    def update(self, target: float, dt: float) -> float:
        if target != self._target:
            self._target = target
            self._since_target = 0.0
        self._since_target += dt
        step = 1.0 / TOTAL_STEPS
        # Approaches 80% of the next step asymptotically, never reaches it.
        creep = 0.8 * step * (1.0 - 1.0 / (1.0 + self._creep_per_s * self._since_target))
        goal = min(1.0, target + creep) if target < 1.0 else 1.0
        self.value = max(self.value, self.value + (goal - self.value) * self._ease)
        return self.value


def parse_systemctl_show(output: str, units: Iterable[str]) -> Dict[str, UnitState]:
    """Parse ``systemctl show -p LoadState -p ActiveState -p SubState u1 u2 ...``: one
    blank-line-separated block of KEY=VALUE lines per unit, in order."""
    units = list(units)
    blocks = [b for b in output.strip().split("\n\n")] if output.strip() else []
    result: Dict[str, UnitState] = {}
    for unit, block in zip(units, blocks):
        fields = dict(line.split("=", 1) for line in block.splitlines() if "=" in line)
        result[unit] = UnitState(
            load=fields.get("LoadState", "unknown"),
            active=fields.get("ActiveState", "unknown"),
            sub=fields.get("SubState", "unknown"),
        )
    return result


class SystemdProbe:
    """Reads unit and system state through ``systemctl`` (read-only queries
    any user may make). Every failure degrades to "unknown" rather than
    raising: early in boot the system bus may not be up yet, and on a dev
    machine there's no systemd at all."""

    def __init__(self, timeout_s: float = 2.0) -> None:
        self._systemctl = shutil.which("systemctl")
        self._timeout_s = timeout_s

    async def _run(self, *args: str) -> Optional[str]:
        if self._systemctl is None:
            return None
        try:
            proc = await asyncio.create_subprocess_exec(
                self._systemctl, *args,
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
            )
            stdout, _ = await asyncio.wait_for(proc.communicate(), self._timeout_s)
        except (OSError, asyncio.TimeoutError) as exc:
            logger.debug("systemctl %s failed: %s", " ".join(args), exc)
            return None
        return stdout.decode(errors="replace")

    async def unit_states(self, *units: str) -> Dict[str, UnitState]:
        output = await self._run("show", "-p", "LoadState", "-p", "ActiveState", "-p", "SubState", *units)
        parsed = parse_systemctl_show(output, units) if output else {}
        return {unit: parsed.get(unit, UNKNOWN) for unit in units}

    async def system_state(self) -> str:
        """``systemctl is-system-running``: "starting", "running",
        "degraded", "stopping", ... (exits non-zero for most of them, but
        still prints the state)."""
        output = await self._run("is-system-running")
        return output.strip() if output else "unknown"
