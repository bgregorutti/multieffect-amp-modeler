"""Unit tests for the software debounce utility (debounce.py).

Uses an injected fake clock so the tests are instant and deterministic --
no real sleeping, no timing flakiness.
"""

from control_daemon.debounce import Debouncer


class FakeClock:
    def __init__(self, start: float = 0.0) -> None:
        self.now = start

    def advance(self, dt: float) -> None:
        self.now += dt

    def __call__(self) -> float:
        return self.now


def test_bouncy_press_collapses_to_one_logical_press():
    clock = FakeClock()
    debouncer = Debouncer(debounce_window_s=0.02, clock=clock)

    # Idle (1) is the initial stable level; a real bouncy contact closure
    # looks like a train of flips before settling low (pressed).
    bounce_sequence = [1, 0, 1, 0, 1, 0, 0, 0]
    events = []
    for level in bounce_sequence:
        clock.advance(0.003)  # 3ms between raw reads, well under the window
        events.append(debouncer.feed(level))

    assert events.count("press") == 0, "should not have settled yet, window not elapsed"

    # Now let the final (0) candidate sit for the rest of the window.
    clock.advance(0.02)
    assert debouncer.feed(0) == "press"

    # Continuing to feed the same settled level fires nothing further.
    clock.advance(0.05)
    assert debouncer.feed(0) is None


def test_held_press_only_fires_once():
    clock = FakeClock()
    debouncer = Debouncer(debounce_window_s=0.02, clock=clock)

    assert debouncer.feed(0) is None  # level changed -- starts a new candidacy
    clock.advance(0.03)
    assert debouncer.feed(0) == "press"  # candidate held long enough -> stable

    # Held down well past the debounce window -- repeated reads of the same
    # raw level must not re-fire "press".
    for _ in range(50):
        clock.advance(0.01)
        assert debouncer.feed(0) is None


def test_release_then_fresh_press_fires_again():
    clock = FakeClock()
    debouncer = Debouncer(debounce_window_s=0.02, clock=clock)

    assert debouncer.feed(0) is None
    clock.advance(0.03)
    assert debouncer.feed(0) == "press"

    assert debouncer.feed(1) is None  # bounce candidate for release, not yet stable
    clock.advance(0.005)
    assert debouncer.feed(1) is None  # still within the window

    clock.advance(0.03)
    assert debouncer.feed(1) == "release"

    assert debouncer.feed(0) is None
    clock.advance(0.03)
    assert debouncer.feed(0) == "press"


def test_short_bounce_within_window_does_not_promote():
    """A single glitchy blip that reverts before the window elapses must
    never be mistaken for a stable transition."""
    clock = FakeClock()
    debouncer = Debouncer(debounce_window_s=0.02, clock=clock)

    clock.advance(0.005)
    assert debouncer.feed(0) is None  # candidate = 0, only 5ms in

    clock.advance(0.005)
    assert debouncer.feed(1) is None  # reverted early -- candidate resets to 1

    clock.advance(0.005)
    assert debouncer.feed(1) is None  # only 5ms since candidate reset

    clock.advance(0.02)
    assert debouncer.feed(1) is None  # 1 is already the stable level -- no event
