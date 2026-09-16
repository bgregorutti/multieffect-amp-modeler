"""Tests for the DSP primitives."""

import numpy as np
import pytest

from pedals.dsp import OnePole, Oversampler, db_to_gain, soft_clip

SR = 48000.0


def sine(freq, seconds=0.25, sample_rate=SR, amplitude=1.0):
    t = np.arange(int(seconds * sample_rate)) / sample_rate
    return amplitude * np.sin(2.0 * np.pi * freq * t)


def steady_state_amplitude(y, skip=0.5):
    """Peak of the back half of a signal, past any filter settling."""
    return float(np.max(np.abs(y[int(len(y) * skip):])))


# --- db_to_gain --------------------------------------------------------------


@pytest.mark.parametrize("db,expected", [(0.0, 1.0), (6.0206, 2.0), (-20.0, 0.1)])
def test_db_to_gain(db, expected):
    assert db_to_gain(db) == pytest.approx(expected, rel=1e-4)


# --- soft_clip ---------------------------------------------------------------


def test_soft_clip_is_bounded_under_extreme_drive():
    y = soft_clip(np.linspace(-1000.0, 1000.0, 5000))
    assert np.all(np.abs(y) <= 1.0)
    assert np.all(np.isfinite(y))


def test_soft_clip_is_near_linear_for_small_signals():
    x = np.linspace(-0.01, 0.01, 101)
    assert soft_clip(x) == pytest.approx(x, abs=1e-6)


def test_soft_clip_without_bias_is_symmetric():
    x = np.linspace(-5.0, 5.0, 1001)
    assert soft_clip(x) == pytest.approx(-soft_clip(-x), abs=1e-12)


def test_soft_clip_bias_breaks_symmetry_but_keeps_silence_silent():
    x = np.linspace(-5.0, 5.0, 1001)
    y = soft_clip(x, bias=0.15)
    assert not np.allclose(y, -soft_clip(-x, bias=0.15))
    # Silence in, silence out: the tanh(bias) term must cancel the offset.
    assert soft_clip(np.zeros(16), bias=0.15) == pytest.approx(0.0, abs=1e-15)


def test_soft_clip_bias_generates_even_harmonics():
    """Asymmetry should put energy at 2f, which a symmetric curve cannot."""
    x = sine(500.0, amplitude=2.0)
    freqs = np.fft.rfftfreq(len(x), 1.0 / SR)
    second = np.argmin(np.abs(freqs - 1000.0))

    symmetric = np.abs(np.fft.rfft(soft_clip(x)))[second]
    asymmetric = np.abs(np.fft.rfft(soft_clip(x, bias=0.15)))[second]
    assert asymmetric > symmetric * 10.0


# --- OnePole -----------------------------------------------------------------


def test_one_pole_lowpass_passes_below_cutoff():
    f = OnePole(SR, 1000.0, "lowpass")
    assert steady_state_amplitude(f.process(sine(100.0))) == pytest.approx(1.0, abs=0.02)


def test_one_pole_lowpass_attenuates_above_cutoff():
    f = OnePole(SR, 1000.0, "lowpass")
    # A one-pole rolls off 6 dB/octave; ~3.3 octaves up is about -20 dB.
    assert steady_state_amplitude(f.process(sine(10000.0))) < 0.15


def test_one_pole_lowpass_is_minus_3db_at_cutoff():
    f = OnePole(SR, 1000.0, "lowpass")
    assert steady_state_amplitude(f.process(sine(1000.0))) == pytest.approx(
        0.7071, abs=0.02
    )


def test_one_pole_highpass_attenuates_below_cutoff():
    f = OnePole(SR, 1000.0, "highpass")
    assert steady_state_amplitude(f.process(sine(100.0))) < 0.15


def test_one_pole_highpass_passes_above_cutoff():
    # Measured well clear of Nyquist. A digital one-pole highpass built as
    # x - lowpass(x) never quite reaches unity up near Nyquist -- the lowpass
    # output still carries phase there, so the subtraction leaves a few percent
    # behind (about 0.93 at 10 kHz for a 1 kHz corner). That is the filter
    # behaving correctly, not a passband error, so the passband is checked
    # where the one-pole approximation actually holds.
    # A decade above the corner is within about half a dB of unity, and that
    # residual is the filter, not the measurement -- so assert a floor rather
    # than an equality that would only ever be true in the analog prototype.
    f = OnePole(SR, 200.0, "highpass")
    assert steady_state_amplitude(f.process(sine(2000.0))) > 0.95


def test_one_pole_highpass_removes_dc():
    f = OnePole(SR, 20.0, "highpass")
    y = f.process(np.ones(SR_LEN := 48000))
    assert abs(y[SR_LEN - 1]) < 0.01


@pytest.mark.parametrize("mode", ["lowpass", "highpass"])
@pytest.mark.parametrize("block", [1, 7, 64, 512])
def test_one_pole_is_block_size_invariant(mode, block):
    x = np.random.default_rng(0).standard_normal(3000)

    whole = OnePole(SR, 1200.0, mode).process(x)

    chunked_filter = OnePole(SR, 1200.0, mode)
    chunked = np.concatenate(
        [chunked_filter.process(x[i:i + block]) for i in range(0, len(x), block)]
    )
    assert chunked == pytest.approx(whole, abs=1e-12)


def test_one_pole_reset_restores_initial_state():
    x = np.random.default_rng(1).standard_normal(500)
    f = OnePole(SR, 800.0, "lowpass")
    first = f.process(x)
    f.reset()
    assert f.process(x) == pytest.approx(first, abs=1e-15)


def test_one_pole_rejects_bad_configuration():
    with pytest.raises(ValueError, match="mode"):
        OnePole(SR, 1000.0, "bandpass")
    with pytest.raises(ValueError, match="out of range"):
        OnePole(SR, 30000.0, "lowpass")


def test_one_pole_handles_empty_block():
    assert OnePole(SR, 1000.0).process(np.array([])).size == 0


# --- Oversampler -------------------------------------------------------------


def test_oversampler_upsample_lengthens_by_factor():
    up = Oversampler(factor=4).upsample(np.zeros(128))
    assert len(up) == 512


def test_oversampler_round_trip_preserves_an_audible_sine():
    """With no nonlinearity in between, up->down must be near-transparent."""
    os_ = Oversampler(factor=4)
    x = sine(1000.0, seconds=0.2)
    y = os_.downsample(os_.upsample(x))

    delay = int(os_.latency_samples)
    aligned, reference = y[delay:], x[: len(x) - delay]
    # Compare past the filters' settling region. The tolerance is set by the
    # interpolation filter's passband ripple (firwin's default Hamming window
    # gives a little over 0.1%), not by anything this test could tighten --
    # 3e-3 against a unit-amplitude sine is roughly -50 dB of error.
    half = len(aligned) // 2
    assert aligned[half:] == pytest.approx(reference[half:], abs=3e-3)


def test_oversampler_round_trip_latency_is_as_reported():
    os_ = Oversampler(factor=4, numtaps=65)
    assert os_.latency_samples == 16.0

    impulse = np.zeros(256)
    impulse[0] = 1.0
    y = os_.downsample(os_.upsample(impulse))
    assert int(np.argmax(np.abs(y))) == 16


def test_oversampler_factor_one_is_a_passthrough():
    os_ = Oversampler(factor=1)
    x = np.random.default_rng(2).standard_normal(256)
    assert os_.latency_samples == 0.0
    assert os_.downsample(os_.upsample(x)) == pytest.approx(x, abs=1e-15)


@pytest.mark.parametrize("block", [8, 64, 256])
def test_oversampler_is_block_size_invariant(block):
    x = np.random.default_rng(3).standard_normal(2048)

    whole_os = Oversampler(factor=4)
    whole = whole_os.downsample(whole_os.upsample(x))

    chunked_os = Oversampler(factor=4)
    chunked = np.concatenate(
        [
            chunked_os.downsample(chunked_os.upsample(x[i:i + block]))
            for i in range(0, len(x), block)
        ]
    )
    assert chunked == pytest.approx(whole, abs=1e-12)


def test_oversampler_rejects_even_numtaps():
    with pytest.raises(ValueError, match="odd"):
        Oversampler(factor=4, numtaps=64)
