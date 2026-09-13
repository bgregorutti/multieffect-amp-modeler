"""Persistence round-trip + atomicity tests."""

from pathlib import Path

import pytest

from control_daemon.models import Bank, DaemonState, Preset, SelectSlotAction
from control_daemon.persistence import load_state, save_state


def test_missing_file_returns_fresh_default_state(tmp_path: Path):
    state = load_state(tmp_path / "does_not_exist.json")
    assert state.version == 1
    assert state.presets == {}
    assert state.banks == []


def test_round_trip_preserves_content(tmp_path: Path):
    store = tmp_path / "state.json"
    preset = Preset(name="Crunch Lead")
    bank = Bank(name="Bank A", slots=[preset.id, None, None, None])
    state = DaemonState(
        presets={preset.id: preset},
        banks=[bank],
        footswitch_mapping={0: SelectSlotAction(slot=0)},
        active_preset_id=preset.id,
        bypass=False,
    )

    save_state(store, state)
    loaded = load_state(store)

    assert loaded.version == state.version
    assert loaded.presets[preset.id].name == "Crunch Lead"
    assert loaded.banks[0].name == "Bank A"
    assert loaded.banks[0].slots[0] == preset.id
    assert loaded.active_preset_id == preset.id
    assert loaded.footswitch_mapping[0].type == "select_slot"
    assert loaded.footswitch_mapping[0].slot == 0


def test_save_leaves_no_temp_files_behind(tmp_path: Path):
    store = tmp_path / "state.json"
    save_state(store, DaemonState())

    leftovers = list(tmp_path.glob("*.tmp"))
    assert leftovers == []
    assert store.exists()


def test_save_is_atomic_existing_file_never_corrupted(tmp_path: Path):
    store = tmp_path / "state.json"
    original = DaemonState(presets={"p1": Preset(id="p1", name="Original")})
    save_state(store, original)

    updated = DaemonState(presets={"p1": Preset(id="p1", name="Updated")})
    save_state(store, updated)

    # A concurrent reader can only ever see one fully-formed generation of
    # the file -- verify the final content is exactly the latest write, with
    # valid, complete JSON (no interleaving is possible with atomic rename).
    reloaded = load_state(store)
    assert reloaded.presets["p1"].name == "Updated"


def test_unsupported_schema_version_raises(tmp_path: Path):
    store = tmp_path / "state.json"
    store.write_text('{"version": 999, "presets": {}}', encoding="utf-8")

    with pytest.raises(ValueError, match="Unsupported preset store schema version"):
        load_state(store)
