"""Tests for DaemonStateManager: rig/preset CRUD, chain resolution,
selection transitions, bypass, asset dedup, and footswitch mapping -> state
change (including tap tempo)."""

from pathlib import Path
from typing import List, Optional

import pytest

from control_daemon.audio_engine_client import AudioEngineClient, AudioEngineRejected
from control_daemon.models import (
    Asset,
    AssetKind,
    BlockParamDescriptor,
    EffectBlock,
    NextPresetAction,
    NextRigAction,
    PresetBlockState,
    PrevPresetAction,
    PrevRigAction,
    SelectPresetAction,
    TapTempoAction,
    ToggleBypassAction,
    resolve_preset,
)
from control_daemon.state import DaemonStateManager, StateError


class RecordingAudioEngine(AudioEngineClient):
    def __init__(self) -> None:
        self.loaded_presets: List[str] = []
        self.loaded_chains: List[list] = []
        self.bypass_calls: List[bool] = []
        self.tempo_calls: List[float] = []
        self.registered_assets: List[str] = []
        self.block_param_calls: List[tuple] = []
        # Test hook: set to a list before calling register_asset to make it
        # return that as the introspected VST3 parameter schema (mirrors a
        # real engine's register_asset reply for a "vst3" asset).
        self.next_register_asset_parameters: Optional[List[BlockParamDescriptor]] = None

        # Test hook: when set, load_preset refuses the way a real engine does
        # for a preset naming an asset it has never been told about.
        self.reject_loads_with: Optional[str] = None

    def load_preset(self, preset) -> None:
        if self.reject_loads_with is not None:
            raise AudioEngineRejected("load_preset", self.reject_loads_with)
        self.loaded_presets.append(preset.id)
        self.loaded_chains.append(
            [(b.type, b.enabled) for b in preset.blocks]
        )

    def set_bypass(self, bypass: bool) -> None:
        self.bypass_calls.append(bypass)

    def set_tempo(self, bpm: float) -> None:
        self.tempo_calls.append(bpm)

    def register_asset(self, asset: Asset) -> Optional[List[BlockParamDescriptor]]:
        self.registered_assets.append(asset.id)
        return self.next_register_asset_parameters

    def set_block_param(self, block_id: str, param_key: str, value: float) -> None:
        self.block_param_calls.append((block_id, param_key, value))

    def list_block_types(self) -> List[dict]:
        return [{"type": "gain", "parameters": [{"key": "gain_db", "label": "Gain", "unit": "dB"}]}]


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


def _bass_rig(manager, name="Ampeg SVT"):
    """A representative rig: pinned amp + cab, two switchable effects."""
    return manager.create_rig(
        name=name,
        chain=[
            EffectBlock(id="amp", type="nam", pinned=True),
            EffectBlock(id="cab", type="ir", pinned=True),
            EffectBlock(id="dist", type="distortion", enabled=False),
            EffectBlock(id="fuzz", type="fuzz", enabled=False),
        ],
    )


# -- rigs ---------------------------------------------------------------------


def test_create_rig(manager):
    rig = _bass_rig(manager)
    assert manager.state.rigs == [rig]
    assert manager.changes[-1] == "create_rig"


def test_create_rig_with_no_chain_gets_the_default_gain_amp_cab_tone_volume_skeleton(
    manager,
):
    rig = manager.create_rig(name="New Rig")

    types = [b.type for b in rig.chain]
    assert types == ["gain", "nam", "ir", "tone_stack", "volume"]
    assert all(b.pinned for b in rig.chain)
    assert all(b.asset_id is None for b in rig.chain)
    # amp/cab keep the ids scripts/load_test_preset.py and the app already
    # address by convention, so filling in a real asset later replaces the
    # placeholder rather than adding a duplicate block.
    ids = {b.id for b in rig.chain}
    assert {"amp", "cab"} <= ids


def test_create_rig_with_explicit_empty_chain_is_honored_as_is(manager):
    rig = manager.create_rig(name="Truly Empty", chain=[])
    assert rig.chain == []


def test_create_rig_rejects_unknown_asset_ref(manager):
    with pytest.raises(StateError) as exc:
        manager.create_rig(
            name="Bad", chain=[EffectBlock(type="nam", asset_id="nope")]
        )
    assert exc.value.code == "not_found"


def test_update_rig_chain_drops_preset_states_for_removed_blocks(manager):
    rig = _bass_rig(manager)
    preset = manager.create_preset(
        rig.id,
        name="Drive",
        block_states={"dist": PresetBlockState(enabled=True)},
    )

    # Remove the distortion block entirely.
    manager.update_rig(
        rig.id,
        chain=[
            EffectBlock(id="amp", type="nam", pinned=True),
            EffectBlock(id="cab", type="ir", pinned=True),
        ],
    )

    assert preset.block_states == {}


def test_update_rig_reloads_engine_when_active(manager):
    rig = _bass_rig(manager)
    manager.create_preset(rig.id, name="Clean")
    manager.select_preset(rig_index=0, preset_index=0)
    loads_before = len(manager.engine.loaded_presets)

    manager.update_rig(rig.id, name="Ampeg SVT II")

    assert manager.state.rigs[0].name == "Ampeg SVT II"
    assert len(manager.engine.loaded_presets) == loads_before + 1


def test_delete_rig_clamps_active_position(manager):
    first = _bass_rig(manager, name="One")
    second = _bass_rig(manager, name="Two")
    manager.create_preset(second.id, name="P")
    manager.select_preset(rig_index=1, preset_index=0)

    manager.delete_rig(second.id)

    assert [r.id for r in manager.state.rigs] == [first.id]
    assert manager.state.active_rig_index == 0


def test_reorder_rigs_follows_the_active_rig(manager):
    one = _bass_rig(manager, name="One")
    two = _bass_rig(manager, name="Two")
    manager.create_preset(two.id, name="P")
    manager.select_preset(rig_index=1, preset_index=0)

    manager.reorder_rigs([two.id, one.id])

    assert [r.name for r in manager.state.rigs] == ["Two", "One"]
    # The active rig moved to index 0, so the index must follow it rather
    # than continue pointing at whatever is now in slot 1.
    assert manager.state.active_rig_index == 0
    assert manager.active_rig().id == two.id


def test_reorder_rigs_rejects_incomplete_list(manager):
    one = _bass_rig(manager, name="One")
    _bass_rig(manager, name="Two")
    with pytest.raises(StateError):
        manager.reorder_rigs([one.id])


def test_select_preset_on_a_rig_with_no_presets_activates_the_rig_without_erroring(
    manager,
):
    """A freshly created rig has no presets yet (create_preset is a
    separate call) -- selecting it (e.g. the mobile app's "tap a rig to
    make it active", which always sends preset_index=0) must not hard-fail
    just because index 0 doesn't exist yet. Regression test: this used to
    raise validation_error, which silently broke selecting any rig beyond
    the first one in the app (RigListScreen's tap handler swallows the
    resulting WS error)."""
    _bass_rig(manager, name="One")
    manager.create_rig(name="Two", chain=[])

    manager.select_preset(rig_index=1, preset_index=0)

    assert manager.state.active_rig_index == 1
    assert manager.active_rig().name == "Two"
    assert manager.active_preset() is None


def test_select_preset_still_rejects_out_of_range_index_on_a_non_empty_rig(manager):
    rig = _bass_rig(manager)
    manager.create_preset(rig.id, name="Clean")

    with pytest.raises(StateError) as exc:
        manager.select_preset(rig_index=0, preset_index=5)
    assert exc.value.code == "validation_error"


# -- presets ------------------------------------------------------------------


def test_create_preset_inside_a_rig(manager):
    rig = _bass_rig(manager)
    preset = manager.create_preset(rig.id, name="Clean")
    assert manager.state.rigs[0].presets == [preset]
    assert manager.changes[-1] == "create_preset"


def test_create_preset_rejects_block_states_for_unknown_blocks(manager):
    rig = _bass_rig(manager)
    with pytest.raises(StateError) as exc:
        manager.create_preset(
            rig.id,
            name="Bogus",
            block_states={"not-a-block": PresetBlockState(enabled=True)},
        )
    assert exc.value.code == "validation_error"


def test_update_preset_reloads_engine_when_active(manager):
    rig = _bass_rig(manager)
    preset = manager.create_preset(rig.id, name="Clean")
    manager.select_preset(rig_index=0, preset_index=0)
    loads_before = len(manager.engine.loaded_presets)

    manager.update_preset(
        rig.id, preset.id, block_states={"dist": PresetBlockState(enabled=True)}
    )

    assert len(manager.engine.loaded_presets) == loads_before + 1
    assert manager.engine.loaded_chains[-1] == [
        ("nam", True),
        ("ir", True),
        ("distortion", True),
        ("fuzz", False),
    ]


def test_update_preset_missing_raises_not_found(manager):
    rig = _bass_rig(manager)
    with pytest.raises(StateError) as exc:
        manager.update_preset(rig.id, "does-not-exist", name="x")
    assert exc.value.code == "not_found"


def test_delete_preset_clamps_active_position(manager):
    rig = _bass_rig(manager)
    manager.create_preset(rig.id, name="A")
    second = manager.create_preset(rig.id, name="B")
    manager.select_preset(rig_index=0, preset_index=1)

    manager.delete_preset(rig.id, second.id)

    assert [p.name for p in manager.state.rigs[0].presets] == ["A"]
    assert manager.state.active_preset_index == 0


def test_select_preset_requires_a_target(manager):
    with pytest.raises(StateError) as exc:
        manager.select_preset()
    assert exc.value.code == "validation_error"


def test_select_preset_out_of_range(manager):
    rig = _bass_rig(manager)
    manager.create_preset(rig.id, name="Only")
    with pytest.raises(StateError):
        manager.select_preset(rig_index=0, preset_index=5)


# -- chain resolution ---------------------------------------------------------


def test_resolve_forces_pinned_blocks_on(manager):
    rig = _bass_rig(manager)
    # A preset that (wrongly) tries to switch the amp and cab off.
    preset = manager.create_preset(
        rig.id,
        name="Broken",
        block_states={
            "amp": PresetBlockState(enabled=False),
            "cab": PresetBlockState(enabled=False),
        },
    )

    resolved = resolve_preset(rig, preset)

    by_id = {b.id: b for b in resolved.blocks}
    assert by_id["amp"].enabled is True
    assert by_id["cab"].enabled is True


def test_resolve_falls_back_to_block_defaults(manager):
    rig = _bass_rig(manager)
    preset = manager.create_preset(rig.id, name="Untouched")

    resolved = resolve_preset(rig, preset)

    # dist/fuzz default to disabled on the block itself.
    assert [(b.id, b.enabled) for b in resolved.blocks] == [
        ("amp", True),
        ("cab", True),
        ("dist", False),
        ("fuzz", False),
    ]


def test_resolve_merges_params_as_overrides(manager):
    rig = manager.create_rig(
        name="R",
        chain=[EffectBlock(id="dist", type="distortion", params={"gain": 3.0, "tone": 5.0})],
    )
    preset = manager.create_preset(
        rig.id,
        name="Hot",
        block_states={"dist": PresetBlockState(enabled=True, params={"gain": 9.0})},
    )

    resolved = resolve_preset(rig, preset)

    # gain overridden, tone inherited from the block.
    assert resolved.blocks[0].params == {"gain": 9.0, "tone": 5.0}


def test_resolved_chain_preserves_order(manager):
    rig = _bass_rig(manager)
    preset = manager.create_preset(rig.id, name="P")
    resolved = resolve_preset(rig, preset)
    assert [b.id for b in resolved.blocks] == ["amp", "cab", "dist", "fuzz"]


# -- bypass -------------------------------------------------------------------


def test_set_bypass_calls_engine_and_persists(manager):
    manager.set_bypass(True)
    assert manager.state.bypass is True
    assert manager.engine.bypass_calls == [True]


# -- assets -------------------------------------------------------------------


def test_register_asset_forwards_to_engine(manager):
    asset = manager.register_asset(
        kind=AssetKind.NAM, filename="amp.nam", stored_path="/data/assets/amp.nam"
    )
    assert asset.id in manager.state.assets
    assert manager.engine.registered_assets == [asset.id]


def test_register_asset_refuses_duplicate_content(manager):
    first = manager.register_asset(
        kind=AssetKind.IR,
        filename="cab_8x10.wav",
        stored_path="/data/assets/a.wav",
        sha256="a" * 64,
    )

    # Same bytes, different filename and path -- still a duplicate.
    with pytest.raises(StateError) as exc:
        manager.register_asset(
            kind=AssetKind.IR,
            filename="ampeg_fridge.wav",
            stored_path="/data/assets/b.wav",
            sha256="a" * 64,
        )

    assert exc.value.code == "duplicate_asset"
    assert first.id in str(exc.value)
    assert list(manager.state.assets) == [first.id]
    assert manager.engine.registered_assets == [first.id]


def test_register_asset_allows_distinct_content(manager):
    first = manager.register_asset(
        kind=AssetKind.IR, filename="a.wav", stored_path="/x/a.wav", sha256="a" * 64
    )
    second = manager.register_asset(
        kind=AssetKind.IR, filename="b.wav", stored_path="/x/b.wav", sha256="b" * 64
    )
    assert {first.id, second.id} == set(manager.state.assets)


def test_register_asset_without_checksum_is_not_deduped(manager):
    """A missing sha256 is unknown content, not identical content."""
    first = manager.register_asset(
        kind=AssetKind.NAM, filename="a.nam", stored_path="/x/a.nam"
    )
    second = manager.register_asset(
        kind=AssetKind.NAM, filename="b.nam", stored_path="/x/b.nam"
    )
    assert {first.id, second.id} == set(manager.state.assets)


def test_register_asset_captures_vst3_parameters_from_engine(manager):
    manager.engine.next_register_asset_parameters = [
        BlockParamDescriptor(key="0", label="Gain", unit="x", min=0.0, max=2.0, default=1.0)
    ]
    asset = manager.register_asset(
        kind=AssetKind.VST3, filename="Test.vst3", stored_path="/plugins/Test.vst3"
    )
    assert asset.parameters == manager.engine.next_register_asset_parameters


def test_register_asset_leaves_parameters_none_for_native_kinds(manager):
    asset = manager.register_asset(kind=AssetKind.NAM, filename="a.nam", stored_path="/x/a.nam")
    assert asset.parameters is None


def test_register_asset_defaults_display_name_to_filename(manager):
    """display_name is optional on the wire but never actually null in
    stored state -- an omitted one defaults to the raw filename."""
    asset = manager.register_asset(
        kind=AssetKind.NAM, filename="my_amp.nam", stored_path="/x/my_amp.nam"
    )
    assert asset.display_name == "my_amp.nam"


def test_register_asset_honors_explicit_display_name(manager):
    asset = manager.register_asset(
        kind=AssetKind.NAM,
        filename="my_amp.nam",
        stored_path="/x/my_amp.nam",
        display_name="Crunch lampes vintage",
    )
    assert asset.display_name == "Crunch lampes vintage"


def test_rename_asset_updates_display_name_and_broadcasts(manager):
    asset = manager.register_asset(
        kind=AssetKind.NAM, filename="my_amp.nam", stored_path="/x/my_amp.nam"
    )
    manager.changes.clear()

    renamed = manager.rename_asset(asset.id, "Crunch lampes vintage")

    assert renamed.id == asset.id
    assert renamed.display_name == "Crunch lampes vintage"
    assert manager.state.assets[asset.id].display_name == "Crunch lampes vintage"
    assert manager.changes == ["rename_asset"]


def test_rename_asset_unknown_id_raises_not_found(manager):
    with pytest.raises(StateError) as exc:
        manager.rename_asset("nope", "New Name")
    assert exc.value.code == "not_found"


def test_block_can_reference_a_registered_asset(manager):
    asset = manager.register_asset(
        kind=AssetKind.IR, filename="cab.wav", stored_path="/x/cab.wav"
    )
    rig = manager.create_rig(
        name="R", chain=[EffectBlock(type="ir", asset_id=asset.id, pinned=True)]
    )
    assert rig.chain[0].asset_id == asset.id


# -- set_block_param / list_block_types --------------------------------------


def test_set_block_param_mutates_live_preset_and_forwards_when_active(manager):
    rig = _bass_rig(manager)
    preset = manager.create_preset(
        rig_id=rig.id, name="P1", block_states={"dist": PresetBlockState(enabled=True)}
    )
    manager.select_preset(rig_index=0, preset_index=0)

    updated = manager.set_block_param(rig.id, preset.id, "dist", "gain_db", 6.0)

    assert updated.block_states["dist"].params["gain_db"] == 6.0
    assert updated.block_states["dist"].enabled is True  # existing override not clobbered
    assert manager.engine.block_param_calls == [("dist", "gain_db", 6.0)]


def test_set_block_param_does_not_forward_when_preset_is_not_active(manager):
    rig = _bass_rig(manager)
    preset_a = manager.create_preset(rig_id=rig.id, name="A")
    preset_b = manager.create_preset(rig_id=rig.id, name="B")
    manager.select_preset(rig_index=0, preset_index=0)  # preset_a is active, not preset_b

    manager.set_block_param(rig.id, preset_b.id, "dist", "gain_db", 6.0)

    assert manager.engine.block_param_calls == []
    assert preset_b.block_states["dist"].params["gain_db"] == 6.0  # still persisted


def test_set_block_param_creates_a_block_state_when_none_existed(manager):
    rig = _bass_rig(manager)
    preset = manager.create_preset(rig_id=rig.id, name="P1")  # no block_states at all

    updated = manager.set_block_param(rig.id, preset.id, "fuzz", "gain_db", 3.0)

    assert updated.block_states["fuzz"].params == {"gain_db": 3.0}


def test_set_block_param_unknown_block_id_is_a_validation_error(manager):
    rig = _bass_rig(manager)
    preset = manager.create_preset(rig_id=rig.id, name="P1")

    with pytest.raises(StateError) as exc:
        manager.set_block_param(rig.id, preset.id, "nope", "gain_db", 1.0)
    assert exc.value.code == "validation_error"


def test_set_block_param_unknown_rig_or_preset_is_not_found(manager):
    rig = _bass_rig(manager)
    preset = manager.create_preset(rig_id=rig.id, name="P1")

    with pytest.raises(StateError) as exc:
        manager.set_block_param("no-such-rig", preset.id, "dist", "gain_db", 1.0)
    assert exc.value.code == "not_found"

    with pytest.raises(StateError) as exc:
        manager.set_block_param(rig.id, "no-such-preset", "dist", "gain_db", 1.0)
    assert exc.value.code == "not_found"


def test_list_block_types_proxies_to_engine(manager):
    assert manager.list_block_types() == manager.engine.list_block_types()


# -- footswitch mapping -> state change --------------------------------------


def test_footswitch_next_and_prev_rig_wraps(manager):
    one = _bass_rig(manager, name="One")
    two = _bass_rig(manager, name="Two")
    manager.create_preset(one.id, name="P")
    manager.create_preset(two.id, name="P")
    manager.set_footswitch_mapping({0: NextRigAction(), 1: PrevRigAction()})

    manager.apply_footswitch_press(0)
    assert manager.state.active_rig_index == 1

    manager.apply_footswitch_press(0)  # wraps back to the first rig
    assert manager.state.active_rig_index == 0
    assert manager.changes[-1] == "footswitch_next_rig"

    manager.apply_footswitch_press(1)  # prev wraps to the last rig
    assert manager.state.active_rig_index == 1
    assert manager.changes[-1] == "footswitch_prev_rig"


def test_footswitch_rig_change_clamps_preset_index(manager):
    """Landing on a rig with fewer presets must not dangle past the end."""
    one = _bass_rig(manager, name="One")
    two = _bass_rig(manager, name="Two")
    manager.create_preset(one.id, name="A")
    manager.create_preset(one.id, name="B")
    manager.create_preset(one.id, name="C")
    manager.create_preset(two.id, name="Only")

    manager.select_preset(rig_index=0, preset_index=2)
    manager.set_footswitch_mapping({0: NextRigAction()})
    manager.apply_footswitch_press(0)

    assert manager.state.active_rig_index == 1
    assert manager.state.active_preset_index == 0
    assert manager.active_preset().name == "Only"


def test_footswitch_next_and_prev_preset_wraps_within_the_rig(manager):
    rig = _bass_rig(manager)
    manager.create_preset(rig.id, name="Clean")
    manager.create_preset(rig.id, name="Drive")
    manager.create_preset(rig.id, name="Solo")
    manager.set_footswitch_mapping({0: NextPresetAction(), 1: PrevPresetAction()})

    manager.apply_footswitch_press(0)
    assert manager.active_preset().name == "Drive"

    manager.apply_footswitch_press(0)
    assert manager.active_preset().name == "Solo"

    manager.apply_footswitch_press(0)  # wraps
    assert manager.active_preset().name == "Clean"
    assert manager.changes[-1] == "footswitch_next_preset"

    manager.apply_footswitch_press(1)  # prev wraps to the last
    assert manager.active_preset().name == "Solo"
    assert manager.changes[-1] == "footswitch_prev_preset"


def test_footswitch_preset_stepping_never_crosses_rigs(manager):
    """Stepping presets must stay inside the current rig -- a switch that
    silently swapped the amp mid-song would be a bug, not a feature."""
    one = _bass_rig(manager, name="One")
    two = _bass_rig(manager, name="Two")
    manager.create_preset(one.id, name="Only A")
    manager.create_preset(two.id, name="Only B")
    manager.set_footswitch_mapping({0: NextPresetAction()})

    manager.apply_footswitch_press(0)
    manager.apply_footswitch_press(0)

    assert manager.state.active_rig_index == 0
    assert manager.active_preset().name == "Only A"


def test_footswitch_preset_change_does_not_reload_a_different_rig(manager):
    """The real-time point of the rig/preset split: stepping presets keeps
    the same amp and cab, so the chain the engine gets keeps the same
    pinned blocks and only its enable flags move."""
    rig = _bass_rig(manager)
    manager.create_preset(rig.id, name="Clean")
    manager.create_preset(
        rig.id, name="Drive", block_states={"dist": PresetBlockState(enabled=True)}
    )
    manager.set_footswitch_mapping({0: NextPresetAction()})

    manager.apply_footswitch_press(0)

    assert manager.engine.loaded_chains[-1] == [
        ("nam", True),
        ("ir", True),
        ("distortion", True),
        ("fuzz", False),
    ]


def test_footswitch_select_preset_by_index(manager):
    rig = _bass_rig(manager)
    manager.create_preset(rig.id, name="A")
    manager.create_preset(rig.id, name="B")
    manager.set_footswitch_mapping({0: SelectPresetAction(index=1)})

    manager.apply_footswitch_press(0)

    assert manager.active_preset().name == "B"


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


def test_footswitch_preset_step_is_a_noop_with_no_presets(manager):
    _bass_rig(manager)
    manager.set_footswitch_mapping({0: NextPresetAction()})
    changes_before = list(manager.changes)

    manager.apply_footswitch_press(0)

    assert manager.changes == changes_before
    assert manager.active_preset() is None


def test_footswitch_rig_step_is_a_noop_with_no_rigs(manager):
    manager.set_footswitch_mapping({0: NextRigAction()})
    changes_before = list(manager.changes)

    manager.apply_footswitch_press(0)

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


# -- derived state view -------------------------------------------------------


def test_state_view_exposes_derived_active_ids(manager):
    rig = _bass_rig(manager)
    preset = manager.create_preset(rig.id, name="Clean")
    manager.select_preset(rig_index=0, preset_index=0)

    view = manager.state_view()

    assert view["active_rig_id"] == rig.id
    assert view["active_preset_id"] == preset.id


def test_state_view_active_ids_are_none_when_nothing_selected(manager):
    view = manager.state_view()
    assert view["active_rig_id"] is None
    assert view["active_preset_id"] is None


# -- engine load failures must not be silent ---------------------------------
#
# The bug these cover: the engine validates a preset's asset references before
# swapping anything in, so a rejected load leaves it playing its *previous*
# chain. The daemon used to log that and carry on, which meant select_preset
# reported success, the active index moved, and the app showed a preset as
# live while something else was audible -- indistinguishable, from the user's
# side, from presets having the wrong effects in them.

_REJECTION = "preset 'p1' block 'amp' references unknown asset_id 'a1'"


def _rig_with_two_presets(manager):
    rig = _bass_rig(manager)
    manager.create_preset(rig.id, name="Clean")
    manager.create_preset(rig.id, name="Lead")
    return rig


def test_select_preset_raises_when_the_engine_refuses(manager):
    rig = _rig_with_two_presets(manager)
    manager.engine.reject_loads_with = _REJECTION

    with pytest.raises(StateError) as excinfo:
        manager.select_preset(rig_index=0, preset_index=1)

    assert excinfo.value.code == "engine_error"
    assert "unknown asset_id" in excinfo.value.message


def test_selection_still_stands_and_is_broadcast_after_a_refusal(manager):
    """The user asked for this preset; the UI should show it as the intended
    one *and* show that it is not actually playing."""
    rig = _rig_with_two_presets(manager)
    manager.engine.reject_loads_with = _REJECTION

    with pytest.raises(StateError):
        manager.select_preset(rig_index=0, preset_index=1)

    assert manager.state.active_preset_index == 1
    assert "select_preset" in manager.changes
    view = manager.state_view()
    assert view["active_preset_id"] == rig.presets[1].id
    assert view["engine_error"] == _REJECTION


def test_engine_error_clears_once_a_load_succeeds(manager):
    rig = _rig_with_two_presets(manager)
    manager.engine.reject_loads_with = _REJECTION
    with pytest.raises(StateError):
        manager.select_preset(rig_index=0, preset_index=1)
    assert manager.engine_error is not None

    manager.engine.reject_loads_with = None
    manager.select_preset(rig_index=0, preset_index=0)

    assert manager.engine_error is None
    assert manager.state_view()["engine_error"] is None


def test_structural_edits_record_the_error_without_failing_the_edit(manager):
    """A rig edit that the engine then refuses is still a valid, persisted
    edit -- raising would wrongly report the edit itself as having failed."""
    rig = _rig_with_two_presets(manager)
    manager.engine.reject_loads_with = _REJECTION

    manager.update_rig(rig.id, name="Renamed")  # must not raise

    assert manager.state.rigs[0].name == "Renamed"
    assert manager.engine_error == _REJECTION


def test_healthy_state_view_reports_no_engine_error(manager):
    _rig_with_two_presets(manager)
    manager.select_preset(rig_index=0, preset_index=0)
    assert manager.state_view()["engine_error"] is None


def test_assets_are_offered_to_the_engine_for_re_registration(manager, tmp_path):
    """The daemon must hand the engine a way to repopulate its in-memory asset
    registry after a restart, or presets referencing an asset uploaded before
    it fail forever."""
    captured = {}

    class ProviderCapturingEngine(RecordingAudioEngine):
        def set_asset_provider(self, provider):
            captured["provider"] = provider

    mgr = DaemonStateManager(
        store_path=tmp_path / "provider.json", audio_engine=ProviderCapturingEngine()
    )
    assert "provider" in captured, "daemon never offered its assets to the engine"

    asset = mgr.register_asset(
        kind=AssetKind.NAM, filename="amp.nam", stored_path=str(tmp_path / "amp.nam"),
        size_bytes=10, sha256="x",
    )
    assert [a.id for a in captured["provider"]()] == [asset.id]
