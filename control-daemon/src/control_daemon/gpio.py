"""Footswitch input abstraction.

``FootswitchInputBackend`` hides where raw GPIO pin levels come from, so the
rest of the daemon (and its test suite) never needs real Raspberry Pi
hardware or the ``gpiozero``/``RPi.GPIO`` libraries to run:

* ``MockFootswitchBackend`` -- for tests and desktop development. Test code
  calls ``inject()`` to synthesize raw pin-level readings exactly as real
  active-low, pulled-up wiring would produce them.
* ``GpioZeroFootswitchBackend`` -- for a real Raspberry Pi. Imports
  ``gpiozero`` lazily inside ``__init__`` (never at module import time) so
  importing this module -- and therefore the whole daemon package -- never
  fails on a machine without GPIO hardware/libraries. Instantiating it
  without ``gpiozero`` available raises a clear, typed error instead of an
  opaque ``ImportError`` from deep inside the module.

``FootswitchInputController`` is the glue: it feeds a backend's raw levels
through one ``debounce.Debouncer`` per switch and only calls back with clean,
logical press events -- see debounce.py for why that layering exists.
"""

from __future__ import annotations

from abc import ABC, abstractmethod
from typing import Callable, Dict, Optional

from .debounce import Debouncer

RawEventCallback = Callable[[int, int], None]  # (switch_index, raw_level)
PressCallback = Callable[[int], None]  # (switch_index) -- logical press only


class FootswitchInputBackend(ABC):
    """Reports raw GPIO pin levels for each footswitch index.

    A backend knows nothing about debouncing or what constitutes a logical
    "press" -- it just reports levels as observed. ``FootswitchInputController``
    layers debouncing on top identically regardless of where the levels come
    from.
    """

    @abstractmethod
    def start(self, on_raw_event: RawEventCallback) -> None:
        """Begin reporting raw level changes.

        ``on_raw_event(switch_index, raw_level)`` may be called for every
        observed level, not only on changes -- callers (i.e.
        ``FootswitchInputController``) must tolerate repeated readings of an
        unchanged level.
        """

    @abstractmethod
    def stop(self) -> None:
        """Stop reporting events and release any resources."""


class MockFootswitchBackend(FootswitchInputBackend):
    """In-memory backend for tests and desktop development."""

    def __init__(self) -> None:
        self._callback: Optional[RawEventCallback] = None
        self._started = False

    def start(self, on_raw_event: RawEventCallback) -> None:
        self._callback = on_raw_event
        self._started = True

    def stop(self) -> None:
        self._started = False
        self._callback = None

    def inject(self, switch_index: int, raw_level: int) -> None:
        """Synthesize a raw pin-level reading (0 = pressed, 1 = idle) for
        ``switch_index``, exactly as real active-low wiring would."""
        if not self._started or self._callback is None:
            raise RuntimeError(
                "MockFootswitchBackend.inject() called before start()"
            )
        self._callback(switch_index, raw_level)


class GpioZeroFootswitchBackend(FootswitchInputBackend):
    """Real hardware backend built on ``gpiozero.Button``.

    Only usable on an actual Raspberry Pi with ``gpiozero`` installed.
    Importing *this module* never touches ``gpiozero`` -- the import is
    deferred to ``__init__`` so the daemon stays importable (and fully
    testable) on a normal Linux dev machine with no GPIO library installed
    at all.
    """

    def __init__(
        self,
        pins: Dict[int, int],
        bounce_time: Optional[float] = None,
    ) -> None:
        """
        ``pins``: mapping of switch_index -> BCM GPIO pin number.
        ``bounce_time``: optional hardware-level debounce hint passed
            through to gpiozero. The daemon's own software ``Debouncer``
            still runs on top of this backend regardless (see
            ``FootswitchInputController``), so this is purely an optional
            extra layer, not a substitute.
        """
        try:
            from gpiozero import Button  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "GpioZeroFootswitchBackend requires the 'gpiozero' package "
                "and real GPIO hardware (e.g. a Raspberry Pi). It is not "
                "available on this platform. Use MockFootswitchBackend for "
                "development and tests instead."
            ) from exc

        self._Button = Button
        self._pins = pins
        self._bounce_time = bounce_time
        self._buttons: Dict[int, object] = {}
        self._callback: Optional[RawEventCallback] = None

    def start(self, on_raw_event: RawEventCallback) -> None:
        self._callback = on_raw_event
        for switch_index, pin in self._pins.items():
            button = self._Button(pin, pull_up=True, bounce_time=self._bounce_time)
            button.when_pressed = lambda i=switch_index: self._callback(i, 0)
            button.when_released = lambda i=switch_index: self._callback(i, 1)
            self._buttons[switch_index] = button

    def stop(self) -> None:
        for button in self._buttons.values():
            try:
                button.close()
            except Exception:
                pass
        self._buttons.clear()
        self._callback = None


class FootswitchInputController:
    """Wires a ``FootswitchInputBackend``'s raw pin levels through a
    per-switch software ``Debouncer`` and invokes ``on_press(switch_index)``
    only for clean, debounced logical presses."""

    def __init__(
        self,
        backend: FootswitchInputBackend,
        on_press: PressCallback,
        debounce_window_s: float = 0.02,
        clock: Optional[Callable[[], float]] = None,
    ) -> None:
        self._backend = backend
        self._on_press = on_press
        self._debounce_window_s = debounce_window_s
        self._clock = clock
        self._debouncers: Dict[int, Debouncer] = {}

    def _debouncer_for(self, switch_index: int) -> Debouncer:
        debouncer = self._debouncers.get(switch_index)
        if debouncer is None:
            kwargs: Dict[str, object] = {"debounce_window_s": self._debounce_window_s}
            if self._clock is not None:
                kwargs["clock"] = self._clock
            debouncer = Debouncer(**kwargs)  # type: ignore[arg-type]
            self._debouncers[switch_index] = debouncer
        return debouncer

    def _handle_raw_event(self, switch_index: int, raw_level: int) -> None:
        edge = self._debouncer_for(switch_index).feed(raw_level)
        if edge == "press":
            self._on_press(switch_index)

    def start(self) -> None:
        self._backend.start(self._handle_raw_event)

    def stop(self) -> None:
        self._backend.stop()
