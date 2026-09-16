"""Core DSP primitives for the Python pedal models.

Every processor here is **block-based and stateful**: feeding a signal through
in consecutive blocks produces the same output as feeding the whole signal
through in one call. That equivalence is what makes these models a trustworthy
reference for the C++ engine, where audio always arrives one block at a time,
and it is asserted directly in ``tests/test_dsp.py``.

Filter state lives in ``scipy.signal.lfilter``'s ``zi`` vectors rather than in
hand-rolled delay variables, so the recursions run at C speed instead of in a
Python loop -- a few seconds of 4x-oversampled audio is otherwise painfully
slow to render.
"""

from __future__ import annotations

import numpy as np
from scipy.signal import firwin, lfilter

__all__ = ["OnePole", "Oversampler", "db_to_gain", "soft_clip"]


def db_to_gain(db: float) -> float:
    """Convert decibels to a linear amplitude multiplier."""
    return float(10.0 ** (db / 20.0))


def soft_clip(x: np.ndarray, bias: float = 0.0) -> np.ndarray:
    """Smooth, compressive saturation modelling diodes in a feedback loop.

    ``tanh`` is the right shape for the Big Muff's clipping stages. Its diodes
    sit in each transistor's feedback path, so gain falls off progressively as
    the signal grows rather than the signal slamming into a hard rail -- which
    is what a Rat-style diodes-to-ground topology does, and is a good part of
    why a Rat sounds raspy where a Muff sounds smooth and compressed.

    ``bias`` offsets the waveform before clipping so the curve squashes the two
    half-cycles by different amounts, generating even-order harmonics; a real
    transistor stage is never perfectly symmetric. The ``tanh(bias)`` term
    subtracts the DC that the offset would otherwise introduce at rest, so
    silence in still gives exactly silence out.
    """
    if bias == 0.0:
        return np.tanh(x)
    return np.tanh(x + bias) - np.tanh(bias)


class OnePole:
    """A one-pole (6 dB/octave) lowpass or highpass filter.

    The highpass is derived as ``x - lowpass(x)`` rather than as its own
    recursion: same response, and it keeps a single code path holding state.
    """

    def __init__(
        self, sample_rate: float, cutoff_hz: float, mode: str = "lowpass"
    ) -> None:
        if mode not in ("lowpass", "highpass"):
            raise ValueError(f"mode must be 'lowpass' or 'highpass', got {mode!r}")
        if not 0.0 < cutoff_hz < sample_rate / 2.0:
            raise ValueError(
                f"cutoff {cutoff_hz} Hz out of range for a {sample_rate} Hz rate"
            )
        self.sample_rate = float(sample_rate)
        self.cutoff_hz = float(cutoff_hz)
        self.mode = mode
        # y[n] = a*x[n] + (1-a)*y[n-1], i.e. b = [a], a = [1, a-1].
        a = 1.0 - np.exp(-2.0 * np.pi * cutoff_hz / sample_rate)
        self._b = np.array([a])
        self._a = np.array([1.0, a - 1.0])
        self._zi = np.zeros(1)

    def reset(self) -> None:
        self._zi = np.zeros(1)

    def process(self, x: np.ndarray) -> np.ndarray:
        x = np.asarray(x, dtype=np.float64)
        if x.size == 0:
            return x.copy()
        lowpassed, self._zi = lfilter(self._b, self._a, x, zi=self._zi)
        return lowpassed if self.mode == "lowpass" else x - lowpassed


class Oversampler:
    """Integer-ratio up/downsampling to wrap around a nonlinear section.

    A Big Muff generates enormous amounts of harmonic content: at high Sustain
    the clipping stages are driven hard enough to approximate a square wave,
    whose harmonics extend far past Nyquist. Distorting at the base rate folds
    all of that back down as inharmonic aliasing, which is the single biggest
    difference between a distortion model that sounds like a pedal and one that
    sounds like a buzzing DSP artifact. Running the nonlinearity oversampled
    moves the fold-back point up and lets the decimation filter discard the
    rest -- ``tests/test_bigmuff.py`` measures the difference.

    Both directions are stateful, so they can be called per block. Only the
    nonlinear part of a chain belongs between them: linear filtering (a tone
    stack, an output level) is unaffected by aliasing and is cheaper outside.
    """

    def __init__(
        self, factor: int = 4, numtaps: int = 65, transition: float = 0.9
    ) -> None:
        if factor < 1:
            raise ValueError(f"factor must be >= 1, got {factor}")
        if numtaps % 2 == 0:
            raise ValueError("numtaps must be odd, for an integer group delay")
        self.factor = int(factor)
        self.numtaps = int(numtaps)
        if self.factor == 1:
            self._fir = np.array([1.0])
        else:
            # firwin's cutoff is normalized to the *oversampled* Nyquist, so
            # transition/factor places it at 90% of the base-rate Nyquist:
            # high enough to leave the audible band alone, low enough to give
            # the filter room to roll off before it wraps.
            self._fir = firwin(numtaps, transition / factor)
        self._up_zi = np.zeros(len(self._fir) - 1)
        self._down_zi = np.zeros(len(self._fir) - 1)

    @property
    def latency_samples(self) -> float:
        """Round-trip group delay, in base-rate samples."""
        if self.factor == 1:
            return 0.0
        # Each FIR contributes (numtaps-1)/2 samples at the oversampled rate.
        return (self.numtaps - 1) / self.factor

    def reset(self) -> None:
        self._up_zi = np.zeros(len(self._fir) - 1)
        self._down_zi = np.zeros(len(self._fir) - 1)

    def upsample(self, x: np.ndarray) -> np.ndarray:
        x = np.asarray(x, dtype=np.float64)
        if self.factor == 1 or x.size == 0:
            return x.copy()
        stuffed = np.zeros(x.size * self.factor)
        # Zero-stuffing divides the signal's energy across `factor` samples,
        # so the interpolation filter needs that gain handed back.
        stuffed[:: self.factor] = x * self.factor
        y, self._up_zi = lfilter(self._fir, [1.0], stuffed, zi=self._up_zi)
        return y

    def downsample(self, x: np.ndarray) -> np.ndarray:
        x = np.asarray(x, dtype=np.float64)
        if self.factor == 1 or x.size == 0:
            return x.copy()
        y, self._down_zi = lfilter(self._fir, [1.0], x, zi=self._down_zi)
        return y[:: self.factor]
