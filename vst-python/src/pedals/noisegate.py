"""A noise gate (downward expander) for taming hiss from upstream pedals.

**What this can and cannot do.** A gate removes noise *in the gaps* -- it shuts
the signal path when you are not playing, so the hiss a distortion pedal
generates never reaches the amp. It does not, and cannot, remove noise that is
riding underneath a note you are actually playing: while the gate is open it is
passing the signal through untouched, hiss included. Removing noise from inside
a signal needs spectral subtraction against a noise profile, which costs an FFT
of latency and artefacts on transients. Every guitar noise gate ever built --
ISP Decimator, Boss NS-2 -- works the way this one does, because in practice
the hiss is only objectionable in the silences.

**Threshold is the one knob that matters.** Set it just above the noise floor
and just below your quietest intended note. The rest have sane defaults.

Three details separate a gate that works from one that chatters:

* **Hysteresis.** It opens at the threshold but does not close until the signal
  falls a few dB *below* it. Without this, a signal sitting right at the
  threshold flaps the gate open and shut at audio rate.
* **Hold.** Once open it stays open for a minimum time. This is what lets notes
  decay naturally instead of being chopped off.
* **Range.** The closed state attenuates by a set amount rather than going
  absolutely silent. A gate slamming to digital zero is more noticeable than
  one ducking 60 dB, because the noise floor vanishing entirely is itself an
  audible event.

**Side-chaining.** ``process`` takes an optional ``sidechain`` signal to detect
from, while the gain is still applied to the main input. Feeding it the clean
guitar signal while gating the distorted output is how a "smart gate" works
(an ISP Decimator G-String): detection then keys off the dynamics of your
playing rather than off a compressed distortion signal whose level barely moves
between a held note and silence.
"""

from __future__ import annotations

from dataclasses import dataclass, fields
from typing import Any

import numpy as np

from .dsp import db_to_gain

__all__ = ["NoiseGate", "NoiseGateParams"]


_ENV_DECAY_MS = 25.0
"""Peak-follower decay. Set by the lowest note the pedal has to survive, not by
taste: the follower must not sag far between successive peaks of a waveform, or
a sustained low note will chatter the gate.

The follower tracks ``abs(x)``, so peaks arrive at *twice* the note's
frequency. A low E at 82 Hz therefore refreshes every 6.1 ms, and the envelope
sags exp(-6.1/25) between peaks -- about 2.1 dB, comfortably inside the 6 dB
default hysteresis. At a 10 ms decay the same note sags 5.3 dB, which is close
enough to the hysteresis window to be worth the margin."""


@dataclass
class NoiseGateParams:
    """Gate controls, in real units to match the C++ engine's block registry."""

    threshold_db: float = -45.0
    range_db: float = -60.0
    attack_ms: float = 1.0
    hold_ms: float = 40.0
    release_ms: float = 120.0
    hysteresis_db: float = 6.0
    bypass: bool = False


class NoiseGate:
    """Stateful, block-based noise gate.

    >>> gate = NoiseGate(sample_rate=48000)
    >>> gate.set_params(threshold_db=-50.0)
    >>> clean = gate.process(noisy)
    >>> keyed = gate.process(distorted, sidechain=guitar_input)
    """

    PARAMETERS: tuple[dict[str, Any], ...] = (
        {"key": "threshold_db", "label": "Threshold", "unit": "dB",
         "min": -90.0, "max": -10.0, "default": -45.0, "step_count": 0},
        {"key": "range_db", "label": "Range", "unit": "dB",
         "min": -90.0, "max": 0.0, "default": -60.0, "step_count": 0},
        {"key": "attack_ms", "label": "Attack", "unit": "ms",
         "min": 0.1, "max": 50.0, "default": 1.0, "step_count": 0},
        {"key": "hold_ms", "label": "Hold", "unit": "ms",
         "min": 0.0, "max": 500.0, "default": 40.0, "step_count": 0},
        {"key": "release_ms", "label": "Release", "unit": "ms",
         "min": 1.0, "max": 1000.0, "default": 120.0, "step_count": 0},
        {"key": "hysteresis_db", "label": "Hysteresis", "unit": "dB",
         "min": 0.0, "max": 24.0, "default": 6.0, "step_count": 0},
        {"key": "bypass", "label": "Bypass", "unit": "",
         "min": 0.0, "max": 1.0, "default": 0.0, "step_count": 1},
    )

    TYPE = "noise_gate"

    def __init__(self, sample_rate: float = 48000.0) -> None:
        if sample_rate <= 0.0:
            raise ValueError(f"sample_rate must be positive, got {sample_rate}")
        self.sample_rate = float(sample_rate)
        self.params = NoiseGateParams()
        self._env_decay = float(np.exp(-1.0 / (_ENV_DECAY_MS * 1e-3 * sample_rate)))
        self.reset()

    @property
    def latency_samples(self) -> float:
        """Zero: detection is causal, with no lookahead."""
        return 0.0

    @property
    def is_open(self) -> bool:
        """Whether the gate is currently passing signal. Useful for a UI LED."""
        return self._is_open

    def reset(self) -> None:
        self._envelope = 0.0
        self._is_open = False
        self._hold_counter = 0
        # Deliberately not snapped to a floor here: ``reset`` runs from
        # ``__init__`` before ``set_params``, so anything computed from
        # ``range_db`` at this point would be the *default* floor rather than
        # the configured one, and the gate would then spend a release-time
        # ramping from one to the other at the start of every render. ``None``
        # means "snap to whatever the floor is when audio first arrives".
        self._gain: float | None = None

    def set_params(self, **kwargs: Any) -> None:
        valid = {f.name for f in fields(NoiseGateParams)}
        ranges = {p["key"]: (p["min"], p["max"]) for p in self.PARAMETERS}
        for key, value in kwargs.items():
            if key not in valid:
                raise ValueError(
                    f"unknown parameter {key!r}; expected one of {sorted(valid)}"
                )
            if key == "bypass":
                self.params.bypass = bool(value)
                continue
            value = float(value)
            low, high = ranges[key]
            if not low <= value <= high:
                raise ValueError(f"{key} must be in {low}..{high}, got {value}")
            setattr(self.params, key, value)

    def process(
        self, x: np.ndarray, sidechain: np.ndarray | None = None
    ) -> np.ndarray:
        """Gate ``x``, detecting from ``sidechain`` if one is given."""
        x = np.asarray(x, dtype=np.float64)
        if x.ndim != 1:
            raise ValueError(f"expected a mono 1-D block, got shape {x.shape}")
        if self.params.bypass or x.size == 0:
            return x.copy()

        if sidechain is None:
            key = x
        else:
            key = np.asarray(sidechain, dtype=np.float64)
            if key.shape != x.shape:
                raise ValueError(
                    f"sidechain shape {key.shape} does not match input {x.shape}"
                )

        p = self.params
        open_threshold = db_to_gain(p.threshold_db)
        close_threshold = db_to_gain(p.threshold_db - p.hysteresis_db)
        floor_gain = db_to_gain(p.range_db)
        hold_samples = int(p.hold_ms * 1e-3 * self.sample_rate)
        attack_coef = float(np.exp(-1.0 / max(p.attack_ms * 1e-3 * self.sample_rate, 1e-9)))
        release_coef = float(np.exp(-1.0 / max(p.release_ms * 1e-3 * self.sample_rate, 1e-9)))

        # Per-sample: the envelope's attack/release coefficient depends on the
        # signal, and the state machine on its own history, so neither can be
        # expressed as a fixed filter. This is the one model here that is not
        # vectorized -- see the README's note on its cost.
        envelope = self._envelope
        # First block after a reset: start already sitting at the configured
        # floor rather than ramping up to it from a stale default.
        gain = floor_gain if self._gain is None else self._gain
        is_open = self._is_open
        hold_counter = self._hold_counter
        decay = self._env_decay

        magnitude = np.abs(key)
        out = np.empty_like(x)

        for n in range(x.size):
            level = magnitude[n]
            # Peak follower: instant attack, exponential decay.
            envelope = level if level > envelope else envelope * decay

            if envelope > open_threshold:
                is_open = True
                hold_counter = hold_samples
            elif is_open:
                if envelope < close_threshold:
                    if hold_counter > 0:
                        hold_counter -= 1
                    else:
                        is_open = False
                else:
                    # Inside the hysteresis band: still committed to staying
                    # open, so keep the hold budget topped up.
                    hold_counter = hold_samples

            target = 1.0 if is_open else floor_gain
            coef = attack_coef if target > gain else release_coef
            gain = target + (gain - target) * coef
            out[n] = x[n] * gain

        self._envelope = envelope
        self._gain = gain
        self._is_open = is_open
        self._hold_counter = hold_counter
        return out
