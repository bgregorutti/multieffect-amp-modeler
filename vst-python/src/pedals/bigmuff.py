"""A Big Muff Pi style fuzz, modelled as DSP rather than as a neural capture.

Why DSP and not a NAM capture: a ``.nam`` file bakes in the knob positions it
was captured at, and two of the Big Muff's three knobs are things a neural
network should never be spent on. Volume is a single multiply. Tone is a
passive filter network -- entirely linear, and exactly modellable. Only Sustain
genuinely reshapes the nonlinearity, and even that amounts to how hard the
clipping stages are driven. Modelling the circuit directly costs a small
fraction of one NAM inference and gives continuously variable knobs for free.

Signal chain, following the circuit:

    in -> pad -> input HPF -> Sustain (pre-gain)
       -> [oversampled] clipping stage 1 -> clipping stage 2
       -> DC block -> tone stack -> Volume -> out

Each clipping stage is a coupling highpass, a fixed gain, a soft saturator
(the diodes in the transistor's feedback loop) and a lowpass -- the cap across
those diodes, which is what keeps a real Muff from sounding like a fizzing
wasp and is the part most often left out of naive distortion models.

The tone stack runs *outside* the oversampled section on purpose: it is linear,
so it cannot alias, and oversampling it would only cost CPU.
"""

from __future__ import annotations

from dataclasses import dataclass, fields
from typing import Any

import numpy as np

from .dsp import OnePole, Oversampler, db_to_gain, soft_clip

__all__ = ["BigMuff", "BigMuffParams"]


# --- Circuit constants -------------------------------------------------------
#
# These are the A/B tuning surface. They are chosen to match the published
# behaviour of the circuit rather than measured against a specific unit, so
# expect to nudge them while comparing against a reference (see the README's
# "Tuning against a reference" section). Keeping them named and in one place is
# what makes that iteration cheap.

_PAD_DB = -15.0
"""The switchable input pad. A modern addition, not on the original circuit."""

_INPUT_HP_HZ = 30.0
"""Input coupling cap: strips DC and subsonic content before any gain."""

_STAGE_HP_HZ = 80.0
"""Interstage coupling caps. Trimming lows *before* each clipper is why the
Muff stays defined on low notes instead of turning to mud."""

_STAGE_LP_HZ = 6000.0
"""The cap across each stage's feedback diodes. Tames the upper harmonics the
clipper generates; without it the model sounds harsh and synthetic."""

_STAGE_GAIN_DB = 14.0
"""Fixed gain into each clipping stage. Two stages cascade, so the pedal's
total gain range is this twice over plus the Sustain pre-gain."""

_STAGE_BIAS = 0.15
"""Clipping asymmetry, generating even-order harmonics."""

_SUSTAIN_MIN_DB = -18.0
_SUSTAIN_MAX_DB = 30.0
"""Sustain is an attenuator ahead of fixed-gain stages on the real pedal, so
it is modelled as pre-gain: at the minimum the stages are barely tickled, at
the maximum they are slammed into a near-square wave."""

_TONE_LP_HZ = 400.0
_TONE_HP_HZ = 2000.0
"""The Big Muff tone control, and the circuit's most recognisable feature: a
lowpassed "bass" branch and a highpassed "treble" branch wired to opposite ends
of the tone pot, with the wiper as the output. Because the highpass corner sits
*above* the lowpass corner the branches never overlap -- at the centre position
both are rolling off through the same region, and the result is the Muff's
signature mid scoop (roughly -9 dB here)."""


@dataclass
class BigMuffParams:
    """The pedal's front panel.

    All three knobs are normalized 0..1, matching the VST3 convention the C++
    engine already uses, so this maps onto a plugin parameter layout unchanged.
    """

    sustain: float = 0.7
    tone: float = 0.5
    volume: float = 0.5
    pad_15db: bool = False
    bypass: bool = False


class BigMuff:
    """Stateful, block-based Big Muff model.

    >>> pedal = BigMuff(sample_rate=48000)
    >>> pedal.set_params(sustain=0.8, tone=0.35)
    >>> out = pedal.process(np.zeros(256))

    ``process`` may be called with any block size; the result is identical to
    processing the concatenated signal in one call.
    """

    #: Parameter descriptors, deliberately in the same shape as the C++ engine's
    #: ``block_type_registry.cpp`` entries so this model can be registered as a
    #: block type without a second source of truth for knob metadata.
    #: ``step_count`` of 1 means a two-position switch, per VST3 convention.
    PARAMETERS: tuple[dict[str, Any], ...] = (
        {"key": "sustain", "label": "Sustain", "unit": "",
         "min": 0.0, "max": 1.0, "default": 0.7, "step_count": 0},
        {"key": "tone", "label": "Tone", "unit": "",
         "min": 0.0, "max": 1.0, "default": 0.5, "step_count": 0},
        {"key": "volume", "label": "Volume", "unit": "",
         "min": 0.0, "max": 1.0, "default": 0.5, "step_count": 0},
        {"key": "pad_15db", "label": "-15 dB Pad", "unit": "",
         "min": 0.0, "max": 1.0, "default": 0.0, "step_count": 1},
        {"key": "bypass", "label": "Bypass", "unit": "",
         "min": 0.0, "max": 1.0, "default": 0.0, "step_count": 1},
    )

    TYPE = "big_muff"

    def __init__(self, sample_rate: float = 48000.0, oversample: int = 4) -> None:
        if sample_rate <= 0.0:
            raise ValueError(f"sample_rate must be positive, got {sample_rate}")
        self.sample_rate = float(sample_rate)
        self.params = BigMuffParams()

        self._oversampler = Oversampler(factor=oversample)
        os_rate = self.sample_rate * oversample

        self._input_hp = OnePole(self.sample_rate, _INPUT_HP_HZ, "highpass")
        # The clipping stages run at the oversampled rate, so their filters do
        # too -- constructing them at the base rate would put their corners in
        # the wrong place by exactly the oversampling factor.
        self._stages = tuple(
            (
                OnePole(os_rate, _STAGE_HP_HZ, "highpass"),
                OnePole(os_rate, _STAGE_LP_HZ, "lowpass"),
            )
            for _ in range(2)
        )
        self._dc_blocker = OnePole(self.sample_rate, 10.0, "highpass")
        self._tone_lp = OnePole(self.sample_rate, _TONE_LP_HZ, "lowpass")
        self._tone_hp = OnePole(self.sample_rate, _TONE_HP_HZ, "highpass")

        self._stage_gain = db_to_gain(_STAGE_GAIN_DB)
        self._pad_gain = db_to_gain(_PAD_DB)

    @property
    def latency_samples(self) -> float:
        """Added latency in base-rate samples, from the oversampling filters."""
        return self._oversampler.latency_samples

    def reset(self) -> None:
        """Clear all filter state, leaving parameters untouched."""
        self._oversampler.reset()
        self._input_hp.reset()
        for highpass, lowpass in self._stages:
            highpass.reset()
            lowpass.reset()
        self._dc_blocker.reset()
        self._tone_lp.reset()
        self._tone_hp.reset()

    def set_params(self, **kwargs: Any) -> None:
        """Update one or more parameters, validating names and ranges."""
        valid = {f.name for f in fields(BigMuffParams)}
        for key, value in kwargs.items():
            if key not in valid:
                raise ValueError(
                    f"unknown parameter {key!r}; expected one of {sorted(valid)}"
                )
            if key in ("pad_15db", "bypass"):
                setattr(self.params, key, bool(value))
                continue
            value = float(value)
            if not 0.0 <= value <= 1.0:
                raise ValueError(f"{key} must be in 0..1, got {value}")
            setattr(self.params, key, value)

    def process(self, x: np.ndarray) -> np.ndarray:
        """Process one block of mono audio, returning a new array."""
        x = np.asarray(x, dtype=np.float64)
        if x.ndim != 1:
            raise ValueError(f"expected a mono 1-D block, got shape {x.shape}")
        if self.params.bypass or x.size == 0:
            return x.copy()

        y = x * self._pad_gain if self.params.pad_15db else x
        y = self._input_hp.process(y)

        sustain_db = _SUSTAIN_MIN_DB + self.params.sustain * (
            _SUSTAIN_MAX_DB - _SUSTAIN_MIN_DB
        )
        y = y * db_to_gain(sustain_db)

        y = self._oversampler.upsample(y)
        for highpass, lowpass in self._stages:
            y = highpass.process(y)
            y = soft_clip(y * self._stage_gain, _STAGE_BIAS)
            y = lowpass.process(y)
        y = self._oversampler.downsample(y)

        # The asymmetric clipper leaves a DC offset that the tone stack would
        # otherwise pass straight through to the output.
        y = self._dc_blocker.process(y)

        tone = self.params.tone
        y = (1.0 - tone) * self._tone_lp.process(y) + tone * self._tone_hp.process(y)

        # A squared taper: the knob's useful range sits in its upper half, as on
        # the real pedal, where a Muff's output is hot enough to push an amp on
        # its own. At volume=1.0 a normal guitar level lands a few dB below full
        # scale, leaving headroom for an amp block downstream.
        return y * (self.params.volume**2)
