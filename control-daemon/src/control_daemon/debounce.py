"""Software debounce for a momentary footswitch wired active-low with a
pull-up resistor (idle level = 1 / HIGH, pressed level = 0 / LOW).

The physical wiring is just a switch shorting a GPIO pin to ground when
pressed; mechanical contacts bounce for a few milliseconds around every
transition, which would otherwise register as several presses for one
physical footswitch stomp. Debouncing is explicitly the daemon's job (the
hardware does nothing about it), so it lives here as a small, dependency-free,
unit-testable utility that both the real GPIO backend and any other raw
input source can reuse.

Algorithm
---------
A candidate level is tracked separately from the last *stable* level. Every
time a new raw reading differs from the current candidate, the candidate
(and its timer) resets. Only once a candidate level has been observed
continuously for at least ``debounce_window_s`` does it get promoted to the
stable level -- and only then is an edge event emitted. This means:

* A burst of raw flips within the debounce window never produces more than
  one stable transition (bounce collapses to one logical press).
* A press held well past the debounce window only ever fires once, right
  when it first becomes stable -- continuing to feed the same raw level
  produces no further events. Only a release (stable transition back to the
  idle level) followed by a fresh press fires again.

The clock is injectable so tests can drive time deterministically without
real sleeping.
"""

from __future__ import annotations

import time
from typing import Callable, Optional

PRESSED_LEVEL = 0
IDLE_LEVEL = 1

Edge = str  # "press" | "release"


class Debouncer:
    def __init__(
        self,
        debounce_window_s: float = 0.02,
        clock: Callable[[], float] = time.monotonic,
        initial_level: int = IDLE_LEVEL,
    ) -> None:
        self._window = debounce_window_s
        self._clock = clock
        self._stable_level = initial_level
        self._candidate_level = initial_level
        self._candidate_since = clock()

    @property
    def stable_level(self) -> int:
        return self._stable_level

    def feed(self, raw_level: int) -> Optional[Edge]:
        """Feed one raw pin-level reading.

        Returns ``"press"`` or ``"release"`` when this reading causes the
        debounced/stable level to change, otherwise ``None``.
        """
        now = self._clock()

        if raw_level != self._candidate_level:
            # The input just changed (or bounced) -- restart the candidacy
            # timer. Bounce mid-window keeps resetting this, so a noisy
            # train of flips never promotes to "stable" until it settles.
            self._candidate_level = raw_level
            self._candidate_since = now
            return None

        if raw_level == self._stable_level:
            # Already stable at this level (including a press held well
            # past the window) -- nothing to report.
            return None

        if (now - self._candidate_since) >= self._window:
            self._stable_level = raw_level
            return "press" if raw_level == PRESSED_LEVEL else "release"

        return None
