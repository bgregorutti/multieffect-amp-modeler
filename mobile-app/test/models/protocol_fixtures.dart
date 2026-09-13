/// Fixture payloads copied verbatim from `control-daemon/README.md`'s
/// "WebSocket protocol" section, so this app's model round-tripping is
/// verified against the actual documented daemon contract rather than
/// against payloads this app's author merely assumed were right.
///
/// If the daemon's README examples change, update these to match and re-run
/// `flutter test` -- that's the whole point of keeping them side by side.
library;

const helloFixture = {
  "type": "hello",
  "role": "app",
  "client_name": "iphone-baptiste",
};

const stateSnapshotEnvelopeFixture = {
  "type": "state_snapshot",
  "state": {"...": "full DaemonState, see models.py"},
};

const stateChangedEnvelopeFixture = {
  "type": "state_changed",
  "reason": "select_preset",
  "state": {"...": "full DaemonState"},
};

const commandOkFixture = {
  "type": "command_ok",
  "command": "create_preset",
  "result": {
    "preset": {"...": "..."}
  },
};

const errorFixture = {
  "type": "error",
  "code": "role_forbidden",
  "message":
      "role 'display' may not send 'set_bypass'; this is an app-only command",
};

const createPresetFixture = {
  "type": "create_preset",
  "name": "Ambient Swell",
  "blocks": [
    {
      "type": "reverb",
      "enabled": true,
      "params": {"decay": 4.2},
    }
  ],
  "nam_asset_id": null,
  "ir_asset_id": null,
};

const updatePresetFixture = {
  "type": "update_preset",
  "preset_id": "abc123",
  "name": "Ambient Swell v2",
};

const deletePresetFixture = {"type": "delete_preset", "preset_id": "abc123"};

const selectPresetByIdFixture = {
  "type": "select_preset",
  "preset_id": "abc123",
};

const selectPresetBySlotFixture = {
  "type": "select_preset",
  "bank_index": 0,
  "slot": 2,
};

const createBankFixture = {
  "type": "create_bank",
  "name": "Live Set 1",
  "num_slots": 4,
};

const updateBankFixture = {
  "type": "update_bank",
  "bank_id": "bank1",
  "slots": ["abc123", null, null, null],
};

const reorderBanksFixture = {
  "type": "reorder_banks",
  "bank_ids": ["bank2", "bank1"],
};

const setBypassFixture = {"type": "set_bypass", "bypass": true};

const setFootswitchMappingFixture = {
  "type": "set_footswitch_mapping",
  "mapping": {
    "0": {"type": "select_slot", "slot": 0},
    "1": {"type": "select_slot", "slot": 1},
    "2": {"type": "next_bank"},
    "3": {"type": "toggle_bypass"},
    "4": {"type": "tap_tempo"},
  },
};

const registerAssetFixture = {
  "type": "register_asset",
  "kind": "nam",
  "filename": "my_amp.nam",
  "stored_path": "/data/assets/abc123.nam",
  "size_bytes": 20971520,
  "sha256":
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
};

const footswitchPressFixture = {
  "type": "footswitch_press",
  "switch_index": 0,
};

const uploadResponseFixture = {
  "kind": "nam",
  "filename": "my_amp.nam",
  "stored_path": "/data/assets/<token>.nam",
  "size_bytes": 20971520,
  "sha256":
      "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
};

/// A full, self-consistent `DaemonState` shaped exactly like the field list
/// in models.py (README's summary doesn't spell out a complete example, so
/// this is built directly from `control_daemon/src/control_daemon/models.py`
/// field-by-field rather than invented ad hoc).
const fullDaemonStateFixture = {
  "version": 1,
  "presets": {
    "abc123": {
      "id": "abc123",
      "name": "Ambient Swell",
      "blocks": [
        {
          "type": "reverb",
          "enabled": true,
          "params": {"decay": 4.2, "mix": 0.5, "label": "hall", "sync": false},
        }
      ],
      "nam_asset_id": "nam1",
      "ir_asset_id": null,
      "created_at": 1000.0,
      "updated_at": 1001.5,
    }
  },
  "banks": [
    {
      "id": "bank1",
      "name": "Live Set 1",
      "slots": ["abc123", null, null, null],
    }
  ],
  "assets": {
    "nam1": {
      "id": "nam1",
      "kind": "nam",
      "filename": "my_amp.nam",
      "stored_path": "/data/assets/abc123.nam",
      "size_bytes": 20971520,
      "sha256":
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
      "uploaded_at": 999.0,
    }
  },
  "footswitch_mapping": {
    "0": {"type": "select_slot", "slot": 0},
    "1": {"type": "select_slot", "slot": 1},
    "2": {"type": "next_bank"},
    "3": {"type": "prev_bank"},
    "4": {"type": "toggle_bypass"},
    "5": {"type": "tap_tempo"},
  },
  "active_bank_index": 0,
  "active_slot": 0,
  "active_preset_id": "abc123",
  "bypass": false,
  "tempo_bpm": 120.0,
};
