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
        Chip("AMP", True), Chip("TS", True), Chip("DLY", False),
    ]


def test_chain_without_override_uses_rig_default():
    assert live_view(_state(active_preset_index=0)).chain == [
        Chip("AMP", True), Chip("TS", False), Chip("DLY", True),
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
