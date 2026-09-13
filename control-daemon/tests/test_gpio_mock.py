"""Tests for the GPIO input abstraction (gpio.py): MockFootswitchBackend,
FootswitchInputController (which layers debounce on top), and the lazy-import
guard on GpioZeroFootswitchBackend.
"""

import sys

import pytest

from control_daemon.gpio import (
    FootswitchInputController,
    GpioZeroFootswitchBackend,
    MockFootswitchBackend,
)


class FakeClock:
    def __init__(self, start: float = 0.0) -> None:
        self.now = start

    def advance(self, dt: float) -> None:
        self.now += dt

    def __call__(self) -> float:
        return self.now


def test_mock_backend_requires_start_before_inject():
    backend = MockFootswitchBackend()
    with pytest.raises(RuntimeError):
        backend.inject(0, 0)


def test_controller_collapses_bounce_to_single_press_callback():
    clock = FakeClock()
    backend = MockFootswitchBackend()
    presses = []
    controller = FootswitchInputController(
        backend=backend,
        on_press=presses.append,
        debounce_window_s=0.02,
        clock=clock,
    )
    controller.start()

    for level in [1, 0, 1, 0, 0, 0]:
        clock.advance(0.003)
        backend.inject(switch_index=2, raw_level=level)

    assert presses == [], "still bouncing, should not have fired yet"

    clock.advance(0.03)
    backend.inject(switch_index=2, raw_level=0)

    assert presses == [2]

    # Holding it down must not cause repeat callbacks.
    clock.advance(0.05)
    backend.inject(switch_index=2, raw_level=0)
    assert presses == [2]

    controller.stop()


def test_controller_tracks_multiple_switches_independently():
    clock = FakeClock()
    backend = MockFootswitchBackend()
    presses = []
    controller = FootswitchInputController(
        backend=backend, on_press=presses.append, debounce_window_s=0.01, clock=clock
    )
    controller.start()

    backend.inject(0, 0)
    clock.advance(0.02)
    backend.inject(0, 0)  # confirm switch 0's press

    backend.inject(1, 0)
    clock.advance(0.02)
    backend.inject(1, 0)  # confirm switch 1's press

    assert presses == [0, 1]


def test_gpiozero_backend_raises_clear_error_without_hardware(monkeypatch):
    # Force `import gpiozero` to fail regardless of what's actually
    # installed in this environment, so the "no hardware available" path is
    # deterministically exercised.
    monkeypatch.setitem(sys.modules, "gpiozero", None)

    with pytest.raises(RuntimeError, match="not available on this platform"):
        GpioZeroFootswitchBackend(pins={0: 17})
