/// Fixture payloads mirroring the control daemon's WebSocket protocol, so
/// this app's model round-tripping is verified against the actual daemon
/// contract rather than against payloads this app's author merely assumed
/// were right.
///
/// These are built field-by-field from
/// `control-daemon/src/control_daemon/models.py` and `ws_protocol.py`. If
/// the daemon's schema changes, update these to match and re-run
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
  "command": "create_rig",
  "result": {
    "rig": {"...": "..."}
  },
};

const errorFixture = {
  "type": "error",
  "code": "role_forbidden",
  "message":
      "role 'display' may not send 'set_bypass'; this is an app-only command",
};

const createRigFixture = {
  "type": "create_rig",
  "name": "Ampeg SVT",
  "chain": [
    {
      "id": "amp",
      "type": "nam",
      "asset_id": "nam1",
      "pinned": true,
      "enabled": true,
      "params": {},
    },
    {
      "id": "dist",
      "type": "distortion",
      "asset_id": null,
      "pinned": false,
      "enabled": false,
      "params": {"gain": 7.0},
    }
  ],
};

const updateRigFixture = {
  "type": "update_rig",
  "rig_id": "rig1",
  "name": "Ampeg SVT II",
};

const deleteRigFixture = {"type": "delete_rig", "rig_id": "rig1"};

const reorderRigsFixture = {
  "type": "reorder_rigs",
  "rig_ids": ["rig2", "rig1"],
};

const createPresetFixture = {
  "type": "create_preset",
  "rig_id": "rig1",
  "name": "Drive",
  "block_states": {
    "dist": {"enabled": true, "params": {}}
  },
};

const updatePresetFixture = {
  "type": "update_preset",
  "rig_id": "rig1",
  "preset_id": "abc123",
  "name": "Drive v2",
};

const deletePresetFixture = {
  "type": "delete_preset",
  "rig_id": "rig1",
  "preset_id": "abc123",
};

const selectPresetFixture = {
  "type": "select_preset",
  "rig_index": 0,
  "preset_index": 2,
};

const selectPresetWithinRigFixture = {
  "type": "select_preset",
  "preset_index": 1,
};

const setBypassFixture = {"type": "set_bypass", "bypass": true};

/// The four-switch layout the pedal is built around: up/down step rigs,
/// left/right step presets within the current rig.
const setFootswitchMappingFixture = {
  "type": "set_footswitch_mapping",
  "mapping": {
    "0": {"type": "next_preset"},
    "1": {"type": "prev_preset"},
    "2": {"type": "next_rig"},
    "3": {"type": "prev_rig"},
    "4": {"type": "toggle_bypass"},
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

/// The daemon's 409 reply when uploaded bytes are already in the library.
/// Dedup is by content checksum, not filename.
const duplicateUploadResponseFixture = {
  "error": "duplicate_asset",
  "message": "identical content already registered as asset nam1 (my_amp.nam)",
  "existing_asset_id": "nam1",
};

/// A full, self-consistent `DaemonState` shaped exactly like the field list
/// in models.py. `active_rig_id`/`active_preset_id` are derived by the
/// daemon into its state view (see `DaemonStateManager.state_view`), so they
/// appear here even though they are not part of the stored schema.
const fullDaemonStateFixture = {
  "version": 2,
  "rigs": [
    {
      "id": "rig1",
      "name": "Ampeg SVT",
      "chain": [
        {
          "id": "amp",
          "type": "nam",
          "asset_id": "nam1",
          "pinned": true,
          "enabled": true,
          "params": {},
        },
        {
          "id": "reverb",
          "type": "reverb",
          "asset_id": null,
          "pinned": false,
          "enabled": true,
          "params": {"decay": 4.2, "mix": 0.5, "label": "hall", "sync": false},
        }
      ],
      "presets": [
        {
          "id": "abc123",
          "name": "Clean",
          "block_states": {
            "reverb": {"enabled": false, "params": {}}
          },
          "created_at": 1000.0,
          "updated_at": 1001.5,
        }
      ],
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
    "0": {"type": "next_preset"},
    "1": {"type": "prev_preset"},
    "2": {"type": "next_rig"},
    "3": {"type": "prev_rig"},
    "4": {"type": "toggle_bypass"},
    "5": {"type": "tap_tempo"},
    "6": {"type": "select_preset", "index": 2},
  },
  "active_rig_index": 0,
  "active_preset_index": 0,
  "active_rig_id": "rig1",
  "active_preset_id": "abc123",
  "bypass": false,
  "tempo_bpm": 120.0,
};
