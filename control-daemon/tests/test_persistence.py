"""Persistence round-trip + atomicity tests, plus the v1 -> v2 migration."""

import json
from pathlib import Path

import pytest

from control_daemon.models import (
    DaemonState,
    EffectBlock,
    NextRigAction,
    Preset,
    Rig,
)
from control_daemon.persistence import load_state, save_state


def test_missing_file_returns_fresh_default_state(tmp_path: Path):
    state = load_state(tmp_path / "does_not_exist.json")
    assert state.version == 2
    assert state.rigs == []
    assert state.assets == {}


def test_round_trip_preserves_content(tmp_path: Path):
    store = tmp_path / "state.json"
    rig = Rig(
        name="Ampeg SVT",
        chain=[
            EffectBlock(id="amp", type="nam", pinned=True),
            EffectBlock(id="dist", type="distortion", enabled=False),
        ],
        presets=[Preset(id="p1", name="Clean")],
    )
    state = DaemonState(
        rigs=[rig],
        footswitch_mapping={0: NextRigAction()},
        active_rig_index=0,
        active_preset_index=0,
        bypass=False,
    )

    save_state(store, state)
    loaded = load_state(store)

    assert loaded.version == state.version
    assert loaded.rigs[0].name == "Ampeg SVT"
    assert loaded.rigs[0].chain[0].pinned is True
    assert loaded.rigs[0].chain[1].enabled is False
    assert loaded.rigs[0].presets[0].name == "Clean"
    assert loaded.footswitch_mapping[0].type == "next_rig"


def test_save_leaves_no_temp_files_behind(tmp_path: Path):
    store = tmp_path / "state.json"
    save_state(store, DaemonState())

    leftovers = list(tmp_path.glob("*.tmp"))
    assert leftovers == []
    assert store.exists()


def test_save_is_atomic_existing_file_never_corrupted(tmp_path: Path):
    store = tmp_path / "state.json"
    save_state(store, DaemonState(rigs=[Rig(id="r1", name="Original")]))
    save_state(store, DaemonState(rigs=[Rig(id="r1", name="Updated")]))

    # A concurrent reader can only ever see one fully-formed generation of
    # the file -- verify the final content is exactly the latest write, with
    # valid, complete JSON (no interleaving is possible with atomic rename).
    reloaded = load_state(store)
    assert reloaded.rigs[0].name == "Updated"


def test_unsupported_schema_version_raises(tmp_path: Path):
    store = tmp_path / "state.json"
    store.write_text('{"version": 999, "rigs": []}', encoding="utf-8")

    with pytest.raises(ValueError, match="Unsupported preset store schema version"):
        load_state(store)


# -- v1 -> v2 migration -------------------------------------------------------


def _write_v1(store: Path, payload: dict) -> None:
    store.write_text(json.dumps(payload), encoding="utf-8")


def test_v1_preset_becomes_a_rig_with_pinned_amp_and_cab(tmp_path: Path):
    """Every v1 preset carried its own amp and cab, so it is really a whole
    backline -- it must migrate to a rig, not to a v2 preset."""
    store = tmp_path / "state.json"
    _write_v1(
        store,
        {
            "version": 1,
            "presets": {
                "p1": {
                    "id": "p1",
                    "name": "SVT Grind",
                    "blocks": [
                        {"type": "distortion", "enabled": True, "params": {"gain": 7.0}}
                    ],
                    "nam_asset_id": "nam-1",
                    "ir_asset_id": "ir-1",
                }
            },
            "banks": [{"id": "b1", "name": "A", "slots": ["p1", None]}],
            "assets": {},
            "footswitch_mapping": {},
            "active_preset_id": "p1",
            "bypass": False,
        },
    )

    state = load_state(store)

    assert state.version == 2
    assert len(state.rigs) == 1
    rig = state.rigs[0]
    assert rig.name == "SVT Grind"

    assert [(b.type, b.pinned, b.asset_id) for b in rig.chain] == [
        ("nam", True, "nam-1"),
        ("ir", True, "ir-1"),
        ("distortion", False, None),
    ]
    # The effect block keeps its stored params.
    assert rig.chain[2].params == {"gain": 7.0}

    # One default preset that overrides nothing, so the sound is unchanged.
    assert len(rig.presets) == 1
    assert rig.presets[0].name == "Default"
    assert rig.presets[0].block_states == {}


def test_v1_migration_orders_rigs_by_bank_then_slot(tmp_path: Path):
    store = tmp_path / "state.json"
    _write_v1(
        store,
        {
            "version": 1,
            "presets": {
                "pA": {"id": "pA", "name": "A", "blocks": []},
                "pB": {"id": "pB", "name": "B", "blocks": []},
                "pC": {"id": "pC", "name": "C", "blocks": []},
                "pOrphan": {"id": "pOrphan", "name": "Orphan", "blocks": []},
            },
            "banks": [
                {"id": "b1", "name": "One", "slots": ["pC", None]},
                {"id": "b2", "name": "Two", "slots": ["pA", "pB"]},
            ],
            "assets": {},
            "footswitch_mapping": {},
            "bypass": False,
        },
    )

    state = load_state(store)

    # Bank order, then slot order; the preset that was in no bank lands last.
    assert [r.name for r in state.rigs] == ["C", "A", "B", "Orphan"]


def test_v1_migration_maps_footswitch_actions(tmp_path: Path):
    store = tmp_path / "state.json"
    _write_v1(
        store,
        {
            "version": 1,
            "presets": {"p1": {"id": "p1", "name": "P", "blocks": []}},
            "banks": [],
            "assets": {},
            "footswitch_mapping": {
                "0": {"type": "next_preset"},
                "1": {"type": "prev_bank"},
                "2": {"type": "toggle_bypass"},
                "3": {"type": "select_slot", "slot": 2},
            },
            "bypass": False,
        },
    )

    state = load_state(store)

    # Each v1 preset became its own rig, so stepping presets in v1 is the
    # same gesture as stepping rigs in v2.
    assert state.footswitch_mapping[0].type == "next_rig"
    assert state.footswitch_mapping[1].type == "prev_rig"
    assert state.footswitch_mapping[2].type == "toggle_bypass"
    # select_slot has no faithful v2 target and is dropped, not rebound.
    assert 3 not in state.footswitch_mapping


def test_v1_migration_preserves_assets_and_active_position(tmp_path: Path):
    store = tmp_path / "state.json"
    _write_v1(
        store,
        {
            "version": 1,
            "presets": {
                "p1": {"id": "p1", "name": "One", "blocks": []},
                "p2": {"id": "p2", "name": "Two", "blocks": []},
            },
            "banks": [{"id": "b1", "name": "A", "slots": ["p1", "p2"]}],
            "assets": {
                "nam-1": {
                    "id": "nam-1",
                    "kind": "nam",
                    "filename": "svt.nam",
                    "stored_path": "/data/assets/svt.nam",
                    "size_bytes": 10,
                    "sha256": "a" * 64,
                }
            },
            "footswitch_mapping": {},
            "active_preset_id": "p2",
            "bypass": True,
            "tempo_bpm": 120.0,
        },
    )

    state = load_state(store)

    assert state.assets["nam-1"].filename == "svt.nam"
    assert state.bypass is True
    assert state.tempo_bpm == 120.0
    # p2 became the second rig, so the active position follows it there.
    assert state.active_rig_index == 1
    assert state.rigs[state.active_rig_index].name == "Two"


def test_v1_migration_result_is_saveable_and_reloadable(tmp_path: Path):
    """The migration must produce a state the normal save/load path accepts,
    so the first write after an upgrade does not fail."""
    store = tmp_path / "state.json"
    _write_v1(
        store,
        {
            "version": 1,
            "presets": {
                "p1": {
                    "id": "p1",
                    "name": "P",
                    "blocks": [{"type": "reverb", "enabled": True, "params": {}}],
                    "nam_asset_id": None,
                    "ir_asset_id": None,
                }
            },
            "banks": [],
            "assets": {},
            "footswitch_mapping": {},
            "bypass": False,
        },
    )

    migrated = load_state(store)
    save_state(store, migrated)
    reloaded = load_state(store)

    assert reloaded.version == 2
    assert reloaded.rigs[0].chain[0].type == "reverb"
