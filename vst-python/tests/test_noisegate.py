"""Tests for the noise gate.

The load-bearing one is ``test_kills_the_noise_floor_but_not_the_note``: that
is the entire point of the effect, and it is easy to build a gate that achieves
one half of it by sacrificing the other.
"""

import numpy as np
import pytest

from pedals import NoiseGate
from pedals.dsp import db_to_gain

SR = 48000.0


def rms(y):
    return float(np.sqrt(np.mean(np.square(y)))) if len(y) else 0.0


def hiss(seconds, amplitude, seed=0, sample_rate=SR):
    rng = np.random.default_rng(seed)
    return amplitude * rng.standard_normal(int(seconds * sample_rate))


def note(seconds, freq=220.0, amplitude=0.3, sample_rate=SR):
    t = np.arange(int(seconds * sample_rate)) / sample_rate
    return amplitude * np.sin(2.0 * np.pi * freq * t)


# --- Plumbing ----------------------------------------------------------------


def test_bypass_is_an_exact_passthrough():
    gate = NoiseGate(SR)
    gate.set_params(bypass=True)
    x = hiss(0.1, 0.5)
    assert np.array_equal(gate.process(x), x)


def test_empty_block_is_handled():
    assert NoiseGate(SR).process(np.array([])).size == 0


def test_rejects_non_mono_input():
    with pytest.raises(ValueError, match="mono"):
        NoiseGate(SR).process(np.zeros((2, 256)))


def test_rejects_mismatched_sidechain():
    with pytest.raises(ValueError, match="sidechain"):
        NoiseGate(SR).process(np.zeros(256), sidechain=np.zeros(128))


@pytest.mark.parametrize("block", [1, 32, 64, 441, 1024])
def test_is_block_size_invariant(block):
    x = np.concatenate([hiss(0.05, 0.001, seed=1), note(0.05), hiss(0.05, 0.001, seed=2)])

    whole = NoiseGate(SR)
    whole.set_params(threshold_db=-40.0)
    expected = whole.process(x)

    chunked = NoiseGate(SR)
    chunked.set_params(threshold_db=-40.0)
    actual = np.concatenate(
        [chunked.process(x[i:i + block]) for i in range(0, len(x), block)]
    )
    assert actual == pytest.approx(expected, abs=1e-12)


def test_reset_restores_initial_state():
    gate = NoiseGate(SR)
    x = np.concatenate([note(0.05), hiss(0.05, 0.001)])
    first = gate.process(x)
    gate.reset()
    assert gate.process(x) == pytest.approx(first, abs=1e-15)


def test_reports_zero_latency():
    assert NoiseGate(SR).latency_samples == 0.0


# --- Parameters --------------------------------------------------------------


def test_parameter_descriptors_cover_every_control():
    keys = {p["key"] for p in NoiseGate.PARAMETERS}
    assert keys == {
        "threshold_db", "range_db", "attack_ms",
        "hold_ms", "release_ms", "hysteresis_db", "bypass",
    }
    for descriptor in NoiseGate.PARAMETERS:
        assert descriptor["min"] <= descriptor["default"] <= descriptor["max"]


def test_parameter_defaults_match_descriptors():
    gate = NoiseGate(SR)
    for descriptor in NoiseGate.PARAMETERS:
        assert float(getattr(gate.params, descriptor["key"])) == descriptor["default"]


def test_set_params_rejects_unknown_names_and_out_of_range_values():
    gate = NoiseGate(SR)
    with pytest.raises(ValueError, match="unknown parameter"):
        gate.set_params(tone=0.5)
    with pytest.raises(ValueError, match="threshold_db must be in"):
        gate.set_params(threshold_db=10.0)


# --- Gating behaviour --------------------------------------------------------


def test_kills_the_noise_floor_but_not_the_note():
    """The whole point: hiss in the gaps goes, the note passes untouched."""
    noise_floor = 0.002  # about -54 dBFS
    quiet_before = hiss(0.5, noise_floor, seed=1)
    played = note(0.5) + hiss(0.5, noise_floor, seed=2)
    quiet_after = hiss(0.5, noise_floor, seed=3)
    x = np.concatenate([quiet_before, played, quiet_after])

    gate = NoiseGate(SR)
    gate.set_params(threshold_db=-40.0, range_db=-60.0)
    y = gate.process(x)

    n = len(quiet_before)
    # Look past the release ramp at the start of each silent stretch.
    settled = int(0.3 * SR)
    silent_in = rms(x[settled:n])
    silent_out = rms(y[settled:n])
    played_in = rms(x[n:2 * n])
    played_out = rms(y[n:2 * n])

    assert silent_out < silent_in / 100.0, "noise floor should be crushed"
    assert played_out == pytest.approx(played_in, rel=0.02), "note must pass intact"


def test_signal_above_threshold_is_passed_at_unity():
    gate = NoiseGate(SR)
    gate.set_params(threshold_db=-40.0)
    x = note(0.5, amplitude=0.3)
    y = gate.process(x)

    settled = int(0.1 * SR)
    assert y[settled:] == pytest.approx(x[settled:], abs=1e-6)


def test_closed_gate_attenuates_by_the_range_setting():
    gate = NoiseGate(SR)
    gate.set_params(threshold_db=-30.0, range_db=-40.0, release_ms=5.0)
    x = hiss(1.0, 0.001, seed=4)
    y = gate.process(x)

    settled = int(0.5 * SR)
    ratio = rms(y[settled:]) / rms(x[settled:])
    assert ratio == pytest.approx(db_to_gain(-40.0), rel=0.1)


def test_higher_threshold_gates_more_aggressively():
    x = np.concatenate([note(0.3, amplitude=0.05), hiss(0.3, 0.002, seed=5)])

    def passed(threshold_db):
        gate = NoiseGate(SR)
        gate.set_params(threshold_db=threshold_db)
        return rms(gate.process(x))

    assert passed(-20.0) < passed(-40.0) < passed(-70.0)


def test_attack_preserves_the_pick_transient():
    """A slow attack would swallow the front of every note."""
    silence = np.zeros(int(0.2 * SR))
    x = np.concatenate([silence, note(0.3, amplitude=0.4)])

    gate = NoiseGate(SR)
    gate.set_params(threshold_db=-40.0, attack_ms=1.0)
    y = gate.process(x)

    onset = len(silence)
    # Within 5 ms of the note starting, the gate should be essentially open.
    window = slice(onset, onset + int(0.005 * SR))
    assert np.max(np.abs(y[window])) > 0.8 * np.max(np.abs(x[window]))


def test_hold_keeps_a_decaying_note_from_being_chopped():
    t = np.arange(int(0.6 * SR)) / SR
    decaying = 0.4 * np.exp(-t * 6.0) * np.sin(2.0 * np.pi * 220.0 * t)

    def tail_energy(hold_ms):
        gate = NoiseGate(SR)
        gate.set_params(threshold_db=-32.0, hold_ms=hold_ms, release_ms=50.0)
        return rms(gate.process(decaying)[int(0.3 * SR):])

    assert tail_energy(200.0) > tail_energy(0.0)


def gate_transitions(gate, x, block=64):
    """Count open/close flips, sampling the gate's own state per block.

    The initial (closed) state is seeded into the series, so a gate that opens
    during the very first block still registers that as a transition.
    """
    states = [int(gate.is_open)]
    for i in range(0, len(x), block):
        gate.process(x[i:i + block])
        states.append(int(gate.is_open))
    return int(np.sum(np.abs(np.diff(states))))


def decaying_note_through_threshold(seed=3):
    """A note dying away across the threshold, with a noise floor underneath.

    This is where a gate actually chatters in use -- not on a steady tone, but
    on a decay whose envelope drifts back and forth over the threshold while
    noise wobbles it. A steady tone barely sags between peaks (the follower
    tracks abs(x), so it is refreshed twice per cycle) and will not show the
    effect at all.
    """
    rng = np.random.default_rng(seed)
    t = np.arange(int(1.2 * SR)) / SR
    return 0.05 * np.exp(-t * 4.0) * np.sin(2.0 * np.pi * 196.0 * t) + (
        0.0006 * rng.standard_normal(len(t))
    )


def test_hysteresis_prevents_chatter_on_a_decaying_note():
    """Measured: 10 open/close flips without hysteresis, 2 with it."""
    x = decaying_note_through_threshold()

    def flips(hysteresis_db):
        gate = NoiseGate(SR)
        gate.set_params(
            threshold_db=-40.0, hysteresis_db=hysteresis_db,
            hold_ms=0.0, release_ms=5.0, attack_ms=0.5,
        )
        return gate_transitions(gate, x)

    assert flips(0.0) > 5, "the test signal must actually provoke chatter"
    # One open, one close: the gate commits to a decision and stays with it.
    assert flips(6.0) == 2
    assert flips(12.0) == 2


def test_hold_also_suppresses_chatter_on_its_own():
    """Hold and hysteresis are independent defences; either one is enough."""
    x = decaying_note_through_threshold()

    gate = NoiseGate(SR)
    gate.set_params(
        threshold_db=-40.0, hysteresis_db=0.0, hold_ms=40.0,
        release_ms=5.0, attack_ms=0.5,
    )
    assert gate_transitions(gate, x) == 2


def test_default_settings_hold_a_sustained_low_note_steady():
    """A low E is the worst case for envelope sag; stock settings must survive.

    The follower is refreshed twice per cycle, so 82 Hz sags about 2.1 dB
    between peaks -- inside the 6 dB default hysteresis.
    """
    level = db_to_gain(-30.0)
    x = level * np.sin(2.0 * np.pi * 82.0 * np.arange(int(0.5 * SR)) / SR)

    gate = NoiseGate(SR)
    gate.set_params(threshold_db=-40.0)
    assert gate_transitions(gate, x) == 1, "should open once and never close"


def test_sidechain_keys_detection_off_a_separate_signal():
    """Gate a distorted signal from the clean input, smart-gate style."""
    quiet = np.zeros(int(0.3 * SR))
    loud = note(0.3, amplitude=0.4)
    clean_key = np.concatenate([quiet, loud])
    # A compressed "distorted" signal whose level barely drops in the gap.
    distorted = 0.5 * np.sign(np.concatenate([hiss(0.3, 0.01, seed=6), loud]))

    gate = NoiseGate(SR)
    gate.set_params(threshold_db=-30.0, range_db=-60.0)
    y = gate.process(distorted, sidechain=clean_key)

    settled = int(0.2 * SR)
    assert rms(y[settled:len(quiet)]) < rms(distorted[settled:len(quiet)]) / 50.0
    assert rms(y[len(quiet) + int(0.05 * SR):]) > 0.2


def test_is_open_reflects_gate_state():
    gate = NoiseGate(SR)
    gate.set_params(threshold_db=-40.0, hold_ms=0.0)
    assert gate.is_open is False

    gate.process(note(0.1, amplitude=0.3))
    assert gate.is_open is True

    gate.process(np.zeros(int(0.3 * SR)))
    assert gate.is_open is False
