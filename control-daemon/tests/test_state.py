"""Tests for DaemonStateManager: preset/bank CRUD, selection transitions,
bypass, and footswitch mapping -> state change (including tap tempo)."""

from pathlib import Path
from typing import List

import pytest

from control_daemon.audio_engine_client import AudioEngineClient
from control_daemon.models import (
    NextBankAction,
    PrevBankAction,
    SelectSlotAction,
    TapTempoAction,
    ToggleBypassAction,
)
from control_daemon.state import DaemonStateManager, StateError


class RecordingAudioEngine(AudioEngineClient):
    def __init__(self) -> None:
        self.loaded_presets: List[str] = []
        self.bypass_calls: List[bool] = []
        self.tempo_calls: List[float] = []

    def load_preset(self, preset) -> None:
        self.loaded_presets.append(preset.id)

    def set_bypass(self, bypass: bool) -> None:
        self.bypass_calls.append(bypass)

    def set_tempo(self, bpm: float) -> None:
        self.tempo_calls.append(bpm)


class FakeClock:
    def __init__(self, start: float = 0.0) -> None:
        self.now = start

    def advance(self, dt: float) -> None:
        self.now += dt

    def __call__(self) -> float:
        return self.now


@pytest.fixture()
def manager(tmp_path: Path):
    engine = RecordingAudioEngine()
    changes: List[str] = []
    clock = FakeClock()
    mgr = DaemonStateManager(
        store_path=tmp_path / "state.json",
        audio_engine=engine,
        on_change=changes.append,
        clock=clock,
    )
    mgr.engine = engine  # convenience handle for assertions
    mgr.changes = changes
    mgr.fake_clock = clock
    return mgr


# -- presets ---------------------------------------------------------------


def test_create_and_get_preset(manager):
    preset = manager.create_preset(name="Clean Tone")
    assert preset.id in manager.state.presets
    assert manager.changes[-1] == "create_preset"


def test_update_preset_reloads_engine_when_active(manager):
    preset = manager.create_preset(name="Lead")
    manager.select_preset(preset_id=preset.id)
    assert manager.engine.loaded_presets == [preset.id]

    manager.update_preset(preset.id, name="Lead v2")
    assert manager.state.presets[preset.id].name == "Lead v2"
    # engine reloaded because the active preset changed shape
    assert manager.engine.loaded_presets == [preset.id, preset.id]


def test_update_preset_missing_raises_not_found(manager):
    with pytest.raises(StateError) as exc_info:
        manager.update_preset("does-not-exist", name="x")
    assert exc_info.value.code == "not_found"


def test_delete_preset_clears_bank_slots_and_active(manager):
    preset = manager.create_preset(name="To Delete")
    bank = manager.create_bank(name="A", num_slots=2)
    manager.update_bank(bank.id, slots=[preset.id, None])
    manager.select_preset(preset_id=preset.id)

    manager.delete_preset(preset.id)

    assert preset.id not in manager.state.presets
    assert manager.state.banks[0].slots == [None, None]
    assert manager.state.active_preset_id is None


def test_select_preset_by_bank_and_slot(manager):
    preset = manager.create_preset(name="P1")
    bank = manager.create_bank(name="A", num_slots=4)
    manager.update_bank(bank.id, slots=[None, preset.id, None, None])

    manager.select_preset(bank_index=0, slot=1)

    assert manager.state.active_preset_id == preset.id
    assert manager.state.active_bank_index == 0
    assert manager.state.active_slot == 1
    assert manager.engine.loaded_presets == [preset.id]


def test_select_preset_requires_a_target(manager):
    with pytest.raises(StateError) as exc_info:
        manager.select_preset()
    assert exc_info.value.code == "validation_error"


def test_select_preset_out_of_range_slot(manager):
    manager.create_bank(name="A", num_slots=2)
    with pytest.raises(StateError):
        manager.select_preset(bank_index=0, slot=5)


# -- banks ------------------------------------------------------------------


def test_create_bank_defaults_to_four_empty_slots(manager):
    bank = manager.create_bank(name="Main")
    assert bank.slots == [None, None, None, None]


def test_reorder_banks(manager):
    b1 = manager.create_bank(name="One")
    b2 = manager.create_bank(name="Two")
    manager.reorder_banks([b2.id, b1.id])
    assert [b.name for b in manager.state.banks] == ["Two", "One"]


def test_reorder_banks_rejects_incomplete_list(manager):
    b1 = manager.create_bank(name="One")
    manager.create_bank(name="Two")
    with pytest.raises(StateError):
        manager.reorder_banks([b1.id])


# -- bypass -------------------------------------------------------------------


def test_set_bypass_calls_engine_and_persists(manager):
    manager.set_bypass(True)
    assert manager.state.bypass is True
    assert manager.engine.bypass_calls == [True]


# -- footswitch mapping -> state change --------------------------------------


def test_footswitch_select_slot(manager):
    preset = manager.create_preset(name="P")
    bank = manager.create_bank(name="A", num_slots=1)
    manager.update_bank(bank.id, slots=[preset.id])
    manager.set_footswitch_mapping({0: SelectSlotAction(slot=0)})

    manager.apply_footswitch_press(0)

    assert manager.state.active_preset_id == preset.id
    assert manager.engine.loaded_presets[-1] == preset.id


def test_footswitch_next_and_prev_bank_wraps(manager):
    b1 = manager.create_bank(name="One")
    b2 = manager.create_bank(name="Two")
    manager.set_footswitch_mapping(
        {0: NextBankAction(), 1: PrevBankAction()}
    )

    manager.apply_footswitch_press(0)
    assert manager.state.active_bank_index == manager.state.banks.index(b2)

    manager.apply_footswitch_press(0)  # wraps back to bank 0
    assert manager.state.active_bank_index == manager.state.banks.index(b1)

    manager.apply_footswitch_press(1)  # prev wraps to last bank
    assert manager.state.active_bank_index == manager.state.banks.index(b2)


def test_footswitch_toggle_bypass(manager):
    manager.set_footswitch_mapping({0: ToggleBypassAction()})
    assert manager.state.bypass is False

    manager.apply_footswitch_press(0)
    assert manager.state.bypass is True
    manager.apply_footswitch_press(0)
    assert manager.state.bypass is False


def test_footswitch_unmapped_switch_is_a_silent_noop(manager):
    changes_before = list(manager.changes)
    manager.apply_footswitch_press(99)
    assert manager.changes == changes_before


def test_footswitch_tap_tempo_computes_bpm(manager):
    manager.set_footswitch_mapping({0: TapTempoAction()})

    manager.apply_footswitch_press(0)
    assert manager.state.tempo_bpm is None  # first tap: no interval yet

    manager.fake_clock.advance(0.5)  # 0.5s between taps -> 120 BPM
    manager.apply_footswitch_press(0)
    assert manager.state.tempo_bpm == pytest.approx(120.0)
    assert manager.engine.tempo_calls[-1] == pytest.approx(120.0)


def test_footswitch_tap_tempo_resets_after_long_gap(manager):
    manager.set_footswitch_mapping({0: TapTempoAction()})
    manager.apply_footswitch_press(0)
    manager.fake_clock.advance(0.5)
    manager.apply_footswitch_press(0)
    assert manager.state.tempo_bpm == pytest.approx(120.0)

    manager.fake_clock.advance(10.0)  # long gap -- fresh tap sequence
    manager.apply_footswitch_press(0)
    # Only one tap in the new sequence so far: no new bpm computed yet from it,
    # bpm stays at the last computed value.
    assert manager.state.tempo_bpm == pytest.approx(120.0)

    manager.fake_clock.advance(1.0)  # 1s interval -> 60 BPM
    manager.apply_footswitch_press(0)
    assert manager.state.tempo_bpm == pytest.approx(60.0)
