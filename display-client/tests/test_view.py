from display_client.view import Chip, block_label, live_view


def _state(**overrides):
    state = {
        "active_rig_index": 0,
        "active_preset_index": 1,
        "bypass": False,
        "engine_error": None,
        "rigs": [
            {
                "name": "Plexi",
                "chain": [
                    {"id": "amp", "type": "nam", "pinned": True},
                    {"id": "ts", "type": "tube_screamer", "enabled": False},
                    {"id": "dly", "type": "delay", "enabled": True},
                ],
                "presets": [
                    {"name": "Rhythm", "block_states": {}},
                    {"name": "Lead", "block_states": {
                        "ts": {"enabled": True},
                        "dly": {"enabled": False},
                        "amp": {"enabled": False},
                    }},
                ],
            },
            {"name": "Clean", "chain": [], "presets": []},
        ],
    }
    state.update(overrides)
    return state


def test_resolves_active_rig_and_preset_positions():
    view = live_view(_state())
    assert (view.rig_name, view.rig_number, view.rig_count) == ("Plexi", 1, 2)
    assert (view.preset_name, view.preset_number, view.preset_count) == ("Lead", 2, 2)


def test_chain_follows_preset_overrides_but_pinned_blocks_stay_on():
    # Same rule as the daemon's resolve_preset: a preset can't switch off
    # the amp, and its overrides beat the rig block's own default.
    assert live_view(_state()).chain == [
        Chip("AMP", True, "amp"), Chip("TS", True, "drv"), Chip("DLY", False, "dly"),
    ]


def test_chain_without_override_uses_rig_default():
    assert live_view(_state(active_preset_index=0)).chain == [
        Chip("AMP", True, "amp"), Chip("TS", False, "drv"), Chip("DLY", True, "dly"),
    ]


def test_rig_without_presets():
    view = live_view(_state(active_rig_index=1, active_preset_index=0))
    assert view.rig_name == "Clean"
    assert view.preset_number == 0 and view.preset_name == ""


def test_no_rigs_is_empty_but_keeps_flags():
    view = live_view({"rigs": [], "bypass": True, "engine_error": "boom"})
    assert view.empty and view.bypass and view.engine_error == "boom"


def test_unknown_block_type_still_gets_a_label():
    assert block_label("chorus") == "CHOR"
    assert block_label("") == "?"


def _backline_state(preset_params=None):
    return {
        "active_rig_index": 0,
        "active_preset_index": 0,
        "assets": {
            "a1": {"filename": "plexi.nam", "display_name": "Plexi crunch"},
            "c1": {"filename": "v30_4x12.wav"},
        },
        "rigs": [{
            "name": "Plexi",
            "chain": [
                {"id": "input_trim", "type": "gain", "pinned": True, "params": {"gain_db": 3.0}},
                {"id": "amp", "type": "nam", "pinned": True, "asset_id": "a1"},
                {"id": "cab", "type": "ir", "pinned": True, "asset_id": "c1"},
                {"id": "tone_stack", "type": "tone_stack", "pinned": True},
                {"id": "gate", "type": "noise_gate", "enabled": True},
                {"id": "output_volume", "type": "volume", "pinned": True},
            ],
            "presets": [{"name": "Rhythm", "block_states": {
                "output_volume": {"enabled": True, "params": preset_params or {}},
            }}],
        }],
    }


def test_gain_and_volume_leave_the_strip_and_become_values():
    view = live_view(_backline_state())
    assert [c.label for c in view.chain] == ["AMP", "CAB", "TONE", "GATE"]
    assert (view.input_gain_db, view.volume_db) == (3.0, 0.0)


def test_level_follows_the_preset_override():
    assert live_view(_backline_state({"gain_db": -4.5})).volume_db == -4.5


def test_block_without_an_image_keeps_its_text_chip():
    chain = live_view(_backline_state()).chain
    assert [c.icon for c in chain] == ["amp", "cab", "eq", None]


def test_amp_and_cab_names_prefer_display_name_then_filename():
    view = live_view(_backline_state())
    assert (view.amp_name, view.cab_name) == ("Plexi crunch", "v30_4x12.wav")


def test_unassigned_amp_shows_a_dash_and_missing_blocks_show_nothing():
    state = _backline_state()
    chain = state["rigs"][0]["chain"]
    chain[1].pop("asset_id")
    del chain[2], chain[0]
    view = live_view(state)
    assert (view.amp_name, view.cab_name, view.input_gain_db) == ("–", None, None)
