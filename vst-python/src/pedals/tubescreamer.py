"""A TS808 Tube Screamer style overdrive.

Structurally this is *not* a milder Big Muff, and modelling it as one is the
usual way to get it wrong. Two things define the circuit:

**The dry signal never leaves.** The clipping diodes sit in the feedback loop
of a *non-inverting* op-amp stage, so the output is the input plus whatever the
boosted-and-clipped path adds. The unity path is always there underneath, which
is why a Tube Screamer sounds like a boost with some hair on it rather than a
fuzz, and why it cleans up when you roll back the guitar's volume.

**Bass is never amplified into the clipper.** The feedback network's input leg
is a resistor in series with a capacitor (4.7k / 0.047uF), so the stage's gain
falls to unity below roughly 720 Hz and only reaches full gain above it. Low
frequencies pass through essentially clean. That single highpass is the entire
reason for the Tube Screamer's famous midrange focus -- it is a *gain* shape,
not a tone control, and it happens before the clipping rather than after.

Signal chain:

    in -> input HPF
       -> [oversampled]  dry + clip(highpass(dry) * Drive)
       -> DC block -> fixed LPF -> Tone -> Level -> out
"""

from __future__ import annotations

from dataclasses import dataclass, fields
from typing import Any

import numpy as np

from .dsp import OnePole, Oversampler, db_to_gain, soft_clip

__all__ = ["TubeScreamer", "TubeScreamerParams"]


# --- Circuit constants -------------------------------------------------------
#
# Unlike the Big Muff's, most of these are derived from the TS808's actual
# component values rather than fitted by ear, and are noted as such.

_INPUT_HP_HZ = 20.0
"""Input coupling. Set low: unlike a Muff, a Tube Screamer is not supposed to
thin the signal out before the gain stage -- the drive path's own highpass does
all the low-end shaping that matters."""

_DRIVE_HP_HZ = 720.0
"""1 / (2*pi * 4.7k * 0.047uF) -- the corner in the op-amp's feedback leg, and
the source of the midrange hump."""

_DRIVE_MIN_DB = 21.5
_DRIVE_MAX_DB = 41.4
"""Stage gain is 1 + (51k + drive_pot) / 4.7k, with a 500k pot: 11.9x at the
minimum, 118x at the maximum. Note that the minimum is still ~21 dB -- a Tube
Screamer is always driving the diodes to some extent, which is why the Drive
knob has a narrower audible range than a Big Muff's Sustain."""

_DIODE_CLAMP = 0.6
"""Where the anti-parallel diodes start limiting the boosted path, standing in
for their forward voltage relative to a nominal guitar level. The dry path is
unaffected, so this sets the *ratio* of clipped to clean in the output."""

_CLIP_BIAS = 0.05
"""Near-symmetric: the TS808 uses two matched silicon diodes. (An asymmetric
variant -- a Boss SD-1, with three diodes -- would raise this considerably.)"""

_TONE_FIXED_LP_HZ = 5500.0
"""Always-on rolloff after the clipper. A Tube Screamer is never harsh at the
top, even with Tone maxed, and this is why."""

_TONE_DARK_LP_HZ = 1200.0
"""The dark end of the Tone sweep. The control is a tilt between this and the
full (already smoothed) bandwidth -- there is no mid scoop anywhere in it."""


@dataclass
class TubeScreamerParams:
    """The pedal's front panel. Knob positions, normalized 0..1."""

    drive: float = 0.5
    tone: float = 0.5
    level: float = 0.5
    bypass: bool = False


class TubeScreamer:
    """Stateful, block-based TS808 model.

    >>> pedal = TubeScreamer(sample_rate=48000)
    >>> pedal.set_params(drive=0.7, tone=0.6, level=0.5)
    >>> out = pedal.process(np.zeros(256))
    """

    PARAMETERS: tuple[dict[str, Any], ...] = (
        {"key": "drive", "label": "Drive", "unit": "",
         "min": 0.0, "max": 1.0, "default": 0.5, "step_count": 0},
        {"key": "tone", "label": "Tone", "unit": "",
         "min": 0.0, "max": 1.0, "default": 0.5, "step_count": 0},
        {"key": "level", "label": "Level", "unit": "",
         "min": 0.0, "max": 1.0, "default": 0.5, "step_count": 0},
        {"key": "bypass", "label": "Bypass", "unit": "",
         "min": 0.0, "max": 1.0, "default": 0.0, "step_count": 1},
    )

    TYPE = "tube_screamer"

    def __init__(self, sample_rate: float = 48000.0, oversample: int = 4) -> None:
        if sample_rate <= 0.0:
            raise ValueError(f"sample_rate must be positive, got {sample_rate}")
        self.sample_rate = float(sample_rate)
        self.params = TubeScreamerParams()

        self._oversampler = Oversampler(factor=oversample)
        os_rate = self.sample_rate * oversample

        self._input_hp = OnePole(self.sample_rate, _INPUT_HP_HZ, "highpass")
        # Runs at the oversampled rate, alongside the clipper it feeds.
        self._drive_hp = OnePole(os_rate, _DRIVE_HP_HZ, "highpass")
        self._dc_blocker = OnePole(self.sample_rate, 10.0, "highpass")
        self._fixed_lp = OnePole(self.sample_rate, _TONE_FIXED_LP_HZ, "lowpass")
        self._tone_dark_lp = OnePole(self.sample_rate, _TONE_DARK_LP_HZ, "lowpass")

    @property
    def latency_samples(self) -> float:
        return self._oversampler.latency_samples

    def reset(self) -> None:
        self._oversampler.reset()
        self._input_hp.reset()
        self._drive_hp.reset()
        self._dc_blocker.reset()
        self._fixed_lp.reset()
        self._tone_dark_lp.reset()

    def set_params(self, **kwargs: Any) -> None:
        valid = {f.name for f in fields(TubeScreamerParams)}
        for key, value in kwargs.items():
            if key not in valid:
                raise ValueError(
                    f"unknown parameter {key!r}; expected one of {sorted(valid)}"
                )
            if key == "bypass":
                self.params.bypass = bool(value)
                continue
            value = float(value)
            if not 0.0 <= value <= 1.0:
                raise ValueError(f"{key} must be in 0..1, got {value}")
            setattr(self.params, key, value)

    def process(self, x: np.ndarray) -> np.ndarray:
        x = np.asarray(x, dtype=np.float64)
        if x.ndim != 1:
            raise ValueError(f"expected a mono 1-D block, got shape {x.shape}")
        if self.params.bypass or x.size == 0:
            return x.copy()

        y = self._input_hp.process(x)

        drive_db = _DRIVE_MIN_DB + self.params.drive * (_DRIVE_MAX_DB - _DRIVE_MIN_DB)
        drive_gain = db_to_gain(drive_db)

        y = self._oversampler.upsample(y)
        # The non-inverting stage: the clipped, frequency-shaped boost is added
        # *to* the dry signal rather than replacing it.
        boost = self._drive_hp.process(y) * drive_gain
        y = y + soft_clip(boost / _DIODE_CLAMP, _CLIP_BIAS) * _DIODE_CLAMP
        y = self._oversampler.downsample(y)

        y = self._dc_blocker.process(y)
        y = self._fixed_lp.process(y)

        tone = self.params.tone
        y = (1.0 - tone) * self._tone_dark_lp.process(y) + tone * y

        return y * (self.params.level**2)
