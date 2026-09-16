"""Tests for the Tube Screamer model.

The circuit-behaviour tests are the interesting ones: they assert the two
things that make a TS808 a Tube Screamer rather than a generic overdrive --
the dry signal surviving underneath, and bass never reaching the clipper.
"""

import numpy as np
import pytest

from pedals import BigMuff, TubeScreamer

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


def harmonic_share(y, fundamental):
    """Energy above the fundamental, relative to the fundamental itself."""
    lower, upper = fundamental * 0.8, fundamental * 1.2
    return band_energy(y, upper, 12000.0) / band_energy(y, lower, upper)


# --- Plumbing ----------------------------------------------------------------


def test_bypass_is_an_exact_passthrough():
    pedal = TubeScreamer(SR)
    pedal.set_params(bypass=True)
    x = noise()
    assert np.array_equal(pedal.process(x), x)


def test_silence_in_silence_out():
    assert TubeScreamer(SR).process(np.zeros(4096)) == pytest.approx(0.0, abs=1e-12)


def test_empty_block_is_handled():
    assert TubeScreamer(SR).process(np.array([])).size == 0


def test_rejects_non_mono_input():
    with pytest.raises(ValueError, match="mono"):
        TubeScreamer(SR).process(np.zeros((2, 256)))


def test_output_is_finite_and_bounded_under_extreme_input():
    pedal = TubeScreamer(SR)
    pedal.set_params(drive=1.0, tone=1.0, level=1.0)
    x = noise(amplitude=50.0)
    y = pedal.process(x)

    assert np.all(np.isfinite(y))
    # The dry path passes the input through by design, so the output scales
    # with the input rather than being clamped -- there is no absolute ceiling
    # to assert. What must hold is that the pedal adds no runaway gain on top:
    # everything the clipper contributes is bounded by the diode clamp.
    # (Measured here: a 237 peak in gives 120 out, the tone filtering taking
    # the rest.)
    assert np.max(np.abs(y)) < np.max(np.abs(x)) * 1.2


@pytest.mark.parametrize("block", [1, 32, 64, 441, 1024])
def test_is_block_size_invariant(block):
    x = noise(seconds=0.25, seed=7)

    whole = TubeScreamer(SR)
    whole.set_params(drive=0.8, tone=0.3, level=0.9)
    expected = whole.process(x)

    chunked = TubeScreamer(SR)
    chunked.set_params(drive=0.8, tone=0.3, level=0.9)
    actual = np.concatenate(
        [chunked.process(x[i:i + block]) for i in range(0, len(x), block)]
    )
    assert actual == pytest.approx(expected, abs=1e-12)


def test_reset_restores_initial_state():
    pedal = TubeScreamer(SR)
    x = noise(seconds=0.1)
    first = pedal.process(x)
    pedal.reset()
    assert pedal.process(x) == pytest.approx(first, abs=1e-15)


def test_latency_matches_the_oversampler():
    assert TubeScreamer(SR, oversample=4).latency_samples == 16.0
    assert TubeScreamer(SR, oversample=1).latency_samples == 0.0


# --- Parameters --------------------------------------------------------------


def test_parameter_descriptors_cover_every_control():
    keys = {p["key"] for p in TubeScreamer.PARAMETERS}
    assert keys == {"drive", "tone", "level", "bypass"}
    for descriptor in TubeScreamer.PARAMETERS:
        assert descriptor["min"] <= descriptor["default"] <= descriptor["max"]


def test_parameter_defaults_match_descriptors():
    pedal = TubeScreamer(SR)
    for descriptor in TubeScreamer.PARAMETERS:
        assert float(getattr(pedal.params, descriptor["key"])) == descriptor["default"]


def test_set_params_rejects_unknown_names_and_out_of_range_values():
    pedal = TubeScreamer(SR)
    with pytest.raises(ValueError, match="unknown parameter"):
        pedal.set_params(sustain=0.5)
    with pytest.raises(ValueError, match="0..1"):
        pedal.set_params(drive=-0.1)


def test_level_is_a_pure_output_scaling():
    x = noise(seconds=0.1, seed=11)

    loud = TubeScreamer(SR)
    loud.set_params(level=1.0)
    quiet = TubeScreamer(SR)
    quiet.set_params(level=0.5)

    assert quiet.process(x) * 4.0 == pytest.approx(loud.process(x), abs=1e-12)


# --- Circuit behaviour -------------------------------------------------------


def level_growth(pedal_factory, freq, quiet=0.1, loud=0.4):
    """How much the output grows when the input is raised 4x.

    4.0 would be perfectly linear; 1.0 means fully saturated, the output no
    longer depending on input level at all. This measures clipping directly,
    without counting harmonics -- which would otherwise compare a low note
    (dozens of harmonics inside the audio band) against a high one (a handful,
    most of them above the pedal's own 5.5 kHz rolloff) and learn nothing.
    """
    a, b = pedal_factory(), pedal_factory()
    return float(
        np.max(np.abs(b.process(sine(freq, amplitude=loud))))
        / np.max(np.abs(a.process(sine(freq, amplitude=quiet))))
    )


def test_dry_signal_survives_underneath_the_clipping():
    """The defining TS808 trait: a non-inverting stage never loses the input.

    Driven far past clipping a Big Muff's output is a square wave whose size no
    longer depends on the input at all. A Tube Screamer's output keeps growing,
    because the clipped path is added to the dry one rather than replacing it.
    Asserted as a contrast between the two models, which is the claim that
    actually matters -- an absolute threshold here would just be a magic number.
    """

    def screamer():
        pedal = TubeScreamer(SR)
        pedal.set_params(drive=1.0, tone=0.5, level=1.0)
        return pedal

    def muff():
        pedal = BigMuff(SR)
        pedal.set_params(sustain=1.0, tone=0.5, volume=1.0)
        return pedal

    # Asserted as a ratio between the two, because the absolute figure moves
    # with input level (1.42x here at 0.1 -> 0.4; 1.73x at 0.2 -> 0.8). What
    # does not move is that the Big Muff is pinned at 1.0 and the Tube
    # Screamer is not.
    muff_growth = level_growth(muff, 440.0)
    assert muff_growth == pytest.approx(1.0, abs=0.1)
    assert level_growth(screamer, 440.0) > muff_growth * 1.3


def test_bass_stays_cleaner_than_treble():
    """The feedback highpass means low notes never reach full gain.

    Measured growth at drive=0.8: 1.67x at 82 Hz falling to 1.31x at 2 kHz --
    the lower the note, the more linearly the pedal behaves.
    """

    def factory():
        pedal = TubeScreamer(SR)
        pedal.set_params(drive=0.8, tone=0.5, level=1.0)
        return pedal

    growth = [level_growth(factory, f) for f in (82.0, 220.0, 880.0, 2000.0)]
    assert growth == sorted(growth, reverse=True), growth


def test_more_drive_adds_harmonic_content():
    x = sine(440.0, amplitude=0.1)

    def share(drive):
        pedal = TubeScreamer(SR)
        pedal.set_params(drive=drive, tone=0.5, level=1.0)
        return harmonic_share(pedal.process(x), 440.0)

    assert share(0.0) < share(0.5) < share(1.0)


def test_tone_sweeps_from_dark_to_bright_without_scooping_mids():
    x = noise(seconds=0.5, seed=19)

    def bands(tone):
        pedal = TubeScreamer(SR)
        pedal.set_params(drive=0.6, tone=tone, level=1.0)
        y = pedal.process(x)
        return (
            band_energy(y, 100.0, 300.0),
            band_energy(y, 700.0, 1300.0),
            band_energy(y, 3000.0, 6000.0),
        )

    treble_ratios = []
    for tone in (0.0, 0.5, 1.0):
        low, mid, high = bands(tone)
        treble_ratios.append(high / low)
        # Unlike a Big Muff, mids are never the quietest band: no scoop.
        assert mid > min(low, high)

    assert treble_ratios[0] < treble_ratios[1] < treble_ratios[2]


def test_oversampling_suppresses_aliasing():
    fundamental = 3137.0
    x = sine(fundamental, seconds=1.0, amplitude=0.3)

    def inharmonic(oversample):
        pedal = TubeScreamer(SR, oversample=oversample)
        pedal.set_params(drive=1.0, tone=0.5, level=1.0)
        y = pedal.process(x)
        spectrum = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
        freqs = np.fft.rfftfreq(len(y), 1.0 / SR)
        on_grid = np.zeros_like(freqs, dtype=bool)
        for harmonic in range(1, int(SR / 2 / fundamental) + 1):
            on_grid |= np.abs(freqs - harmonic * fundamental) < 60.0
        return float(np.sum(spectrum[~on_grid]) / np.sum(spectrum))

    assert inharmonic(4) < inharmonic(1) / 3.0
