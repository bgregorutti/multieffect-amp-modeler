"""Tests for the Big Muff model.

The two load-bearing tests here are ``test_is_block_size_invariant`` (state is
handled correctly, so this is a valid reference for a block-based C++ engine)
and ``test_oversampling_suppresses_aliasing`` (the model actually earns the
oversampling it pays for).
"""

import numpy as np
import pytest

from pedals import BigMuff
from pedals.dsp import db_to_gain

SR = 48000.0


def sine(freq, seconds=1.0, sample_rate=SR, amplitude=0.2):
    t = np.arange(int(seconds * sample_rate)) / sample_rate
    return amplitude * np.sin(2.0 * np.pi * freq * t)


def noise(seconds=1.0, sample_rate=SR, amplitude=0.2, seed=0):
    rng = np.random.default_rng(seed)
    return amplitude * rng.standard_normal(int(seconds * sample_rate))


def band_energy(y, low, high, sample_rate=SR):
    spectrum = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    freqs = np.fft.rfftfreq(len(y), 1.0 / sample_rate)
    band = (freqs >= low) & (freqs < high)
    return float(np.sum(spectrum[band] ** 2))


def inharmonic_fraction(y, fundamental, sample_rate=SR, tolerance=60.0):
    """Share of spectral energy that is *not* near a harmonic of `fundamental`.

    With a memoryless nonlinearity the only things off the harmonic grid are
    aliases, so this is a direct measure of fold-back.
    """
    spectrum = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
    freqs = np.fft.rfftfreq(len(y), 1.0 / sample_rate)

    on_grid = np.zeros_like(freqs, dtype=bool)
    for harmonic in range(1, int(sample_rate / 2 / fundamental) + 1):
        on_grid |= np.abs(freqs - harmonic * fundamental) < tolerance

    total = np.sum(spectrum)
    return float(np.sum(spectrum[~on_grid]) / total)


# --- Plumbing ----------------------------------------------------------------


def test_bypass_is_an_exact_passthrough():
    pedal = BigMuff(SR)
    pedal.set_params(bypass=True)
    x = noise()
    assert np.array_equal(pedal.process(x), x)


def test_silence_in_silence_out():
    pedal = BigMuff(SR)
    assert pedal.process(np.zeros(4096)) == pytest.approx(0.0, abs=1e-12)


def test_empty_block_is_handled():
    assert BigMuff(SR).process(np.array([])).size == 0


def test_rejects_non_mono_input():
    with pytest.raises(ValueError, match="mono"):
        BigMuff(SR).process(np.zeros((2, 256)))


@pytest.mark.parametrize("tone", [0.0, 0.5, 1.0])
def test_output_is_finite_and_bounded_under_extreme_input(tone):
    """A 50x-over-range input must not produce runaway or non-finite output."""
    pedal = BigMuff(SR)
    pedal.set_params(sustain=1.0, tone=tone, volume=1.0)
    y = pedal.process(noise(amplitude=50.0))

    assert np.all(np.isfinite(y))
    # tanh bounds each clipping stage at +/-1, but the tone stack's treble
    # branch is a highpass, and a highpass fed a near-square wave overshoots
    # slightly at the edges (measured: ~1.02 at tone=1.0, ~0.68 at tone=0.0).
    # That is ordinary filter ringing rather than a gain-staging error, so the
    # assertion is "no runaway", not "never exceeds unity"; the C++ chain keeps
    # a clamp as its final safety net regardless.
    assert np.max(np.abs(y)) < 1.5


@pytest.mark.parametrize("block", [1, 32, 64, 441, 1024])
def test_is_block_size_invariant(block):
    x = noise(seconds=0.25, seed=7)

    whole = BigMuff(SR)
    whole.set_params(sustain=0.8, tone=0.3, volume=0.9)
    expected = whole.process(x)

    chunked = BigMuff(SR)
    chunked.set_params(sustain=0.8, tone=0.3, volume=0.9)
    actual = np.concatenate(
        [chunked.process(x[i:i + block]) for i in range(0, len(x), block)]
    )
    assert actual == pytest.approx(expected, abs=1e-12)


def test_reset_restores_initial_state():
    pedal = BigMuff(SR)
    x = noise(seconds=0.1)
    first = pedal.process(x)
    pedal.reset()
    assert pedal.process(x) == pytest.approx(first, abs=1e-15)


def test_latency_is_reported_and_comes_only_from_oversampling():
    assert BigMuff(SR, oversample=4).latency_samples == 16.0
    assert BigMuff(SR, oversample=1).latency_samples == 0.0


# --- Parameters --------------------------------------------------------------


def test_parameter_descriptors_cover_every_control():
    keys = {p["key"] for p in BigMuff.PARAMETERS}
    assert keys == {"sustain", "tone", "volume", "pad_15db", "bypass"}
    for descriptor in BigMuff.PARAMETERS:
        assert descriptor["min"] <= descriptor["default"] <= descriptor["max"]
    switches = {p["key"] for p in BigMuff.PARAMETERS if p["step_count"] == 1}
    assert switches == {"pad_15db", "bypass"}


def test_parameter_defaults_match_descriptors():
    pedal = BigMuff(SR)
    for descriptor in BigMuff.PARAMETERS:
        actual = getattr(pedal.params, descriptor["key"])
        assert float(actual) == descriptor["default"]


def test_set_params_rejects_unknown_names_and_out_of_range_values():
    pedal = BigMuff(SR)
    with pytest.raises(ValueError, match="unknown parameter"):
        pedal.set_params(gain=0.5)
    with pytest.raises(ValueError, match="0..1"):
        pedal.set_params(tone=1.5)


def test_volume_is_a_pure_output_scaling():
    """Volume sits after the nonlinearity, so it may only scale the result."""
    x = noise(seconds=0.1, seed=11)

    loud = BigMuff(SR)
    loud.set_params(volume=1.0)
    quiet = BigMuff(SR)
    quiet.set_params(volume=0.5)

    # A squared taper: 0.5 -> 0.25, i.e. exactly a quarter of full volume.
    assert quiet.process(x) * 4.0 == pytest.approx(loud.process(x), abs=1e-12)


def test_pad_attenuates_the_input_by_15db():
    """The pad is pre-gain, so it must equal feeding a 15 dB quieter signal."""
    x = noise(seconds=0.1, seed=13)

    padded = BigMuff(SR)
    padded.set_params(pad_15db=True)

    attenuated = BigMuff(SR)
    attenuated.set_params(pad_15db=False)

    assert padded.process(x) == pytest.approx(
        attenuated.process(x * db_to_gain(-15.0)), abs=1e-12
    )


def test_pad_reduces_distortion_not_just_level():
    """Because it lands ahead of the clippers, the pad should clean up too."""
    x = sine(220.0, amplitude=0.3)

    hot = BigMuff(SR)
    hot.set_params(sustain=0.5)
    soft = BigMuff(SR)
    soft.set_params(sustain=0.5, pad_15db=True)

    def harmonic_share(y):
        fundamental = band_energy(y, 180.0, 260.0)
        return band_energy(y, 300.0, 12000.0) / fundamental

    assert harmonic_share(soft.process(x)) < harmonic_share(hot.process(x))


# --- Circuit behaviour -------------------------------------------------------


def test_more_sustain_adds_harmonic_content():
    x = sine(220.0, amplitude=0.1)

    def harmonic_share(sustain):
        pedal = BigMuff(SR)
        pedal.set_params(sustain=sustain, tone=0.5, volume=1.0)
        y = pedal.process(x)
        return band_energy(y, 300.0, 12000.0) / band_energy(y, 180.0, 260.0)

    low, mid, high = (harmonic_share(s) for s in (0.0, 0.5, 1.0))
    assert low < mid < high


def test_tone_at_centre_scoops_the_mids():
    """The Big Muff's signature: both tone branches roll off in the midrange."""
    x = noise(seconds=1.0, seed=17)

    def mid_share(tone):
        pedal = BigMuff(SR)
        pedal.set_params(sustain=0.6, tone=tone, volume=1.0)
        y = pedal.process(x)
        mids = band_energy(y, 700.0, 1300.0)
        edges = band_energy(y, 100.0, 300.0) + band_energy(y, 3000.0, 6000.0)
        return mids / edges

    assert mid_share(0.5) < mid_share(0.0)
    assert mid_share(0.5) < mid_share(1.0)


def test_tone_sweeps_from_dark_to_bright():
    x = noise(seconds=0.5, seed=19)

    def treble_to_bass(tone):
        pedal = BigMuff(SR)
        pedal.set_params(sustain=0.6, tone=tone, volume=1.0)
        y = pedal.process(x)
        return band_energy(y, 3000.0, 6000.0) / band_energy(y, 100.0, 300.0)

    assert treble_to_bass(0.0) < treble_to_bass(0.5) < treble_to_bass(1.0)


def test_oversampling_suppresses_aliasing():
    """The reason the oversampler exists, measured rather than assumed.

    3137 Hz is deliberately not a rational fraction of the sample rate: with a
    tidier fundamental the aliases would fold back onto the harmonic grid and
    hide inside the very bins this test excludes.
    """
    fundamental = 3137.0
    x = sine(fundamental, seconds=1.0, amplitude=0.3)

    def aliasing(oversample):
        pedal = BigMuff(SR, oversample=oversample)
        pedal.set_params(sustain=1.0, tone=0.5, volume=1.0)
        return inharmonic_fraction(pedal.process(x), fundamental)

    assert aliasing(4) < aliasing(1) / 3.0
