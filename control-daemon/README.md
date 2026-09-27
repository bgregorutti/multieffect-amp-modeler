# control-daemon

The control daemon for the DIY AI guitar multi-effects pedal. It is the
single source of truth for "which rig/preset is active" and for all
rig/preset/footswitch configuration:

```
Mobile app (Flutter) <--WebSocket--> Control daemon <--drives--> Audio engine (JUCE/C++ plugin host)
Footswitch (GPIO)     <--GPIO/events--> Control daemon
Onboard display        <--WebSocket/events--> Control daemon
```

All editing complexity lives in the mobile app. The footswitch and onboard
display are read-only/trigger-only clients: they send simple events
(footswitch: "switch N was pressed") and receive state broadcasts; they
never contain configuration logic and the daemon actively rejects
configuration-editing commands from them.

This package is self-contained and fully testable on a normal Linux dev
machine -- no Raspberry Pi, no real GPIO hardware, no JUCE/Flutter needed.

## Engine asset registry and load failures

Two linked behaviours worth knowing about, because together they used to
produce a bug that looked like preset corruption.

**The engine's asset registry is in-memory only.** `audio-engine` holds
registered `.nam`/IR/`.vst3` metadata in a plain map, so a restart of that
process starts it with none -- while this daemon still has every asset in its
persisted state and will happily keep sending presets that reference them.
`register_asset` is otherwise only sent on upload, so before this was fixed an
engine restart made *every* preset with an amp or cab block fail to load, with:

```
audio-engine rejected load_preset: preset 'p1' block 'amp' references unknown asset_id 'a1'
```

permanently, until the user re-uploaded the file. `UnixSocketAudioEngineClient`
now replays the whole asset registry whenever it establishes a connection
(`_replay_assets_locked`, driven by `set_asset_provider`), so an engine restart
self-heals. The daemon supplies the provider in `DaemonStateManager.__init__`.

**A rejection is now distinct from an absent engine.** Fail-open on an
unreachable engine is deliberate and unchanged -- the daemon is the source of
truth for rig/preset/footswitch state whether or not anything is listening,
which is what lets it run on a dev machine with no engine at all. But a
*rejection* is the opposite situation: the engine is right there and has
refused, which means it is still playing its previous chain (it validates
before swapping anything in). That used to be logged and forgotten, so
`select_preset` reported success, the active index moved, and the app showed a
preset as live while something else was audible.

Now `_send` raises `AudioEngineRejected` on a refusal, and:

* `_load_active()` records it in `engine_error`, which `state_view()`
  publishes to every client. It is *not* raised there: `_load_active` runs
  after every structural edit (`update_rig`, `delete_preset`, reorder, ...),
  and those edits are valid and already persisted by that point -- raising
  would wrongly report the edit itself as having failed.
* `select_preset()` additionally raises `StateError("engine_error", ...)`,
  since making a preset audible is that command's entire purpose. The
  selection still stands and is still broadcast, so clients can show which
  preset *should* be live alongside the reason it is not.

The mobile app surfaces this on the preset list ("Not playing: ..."). Covered
by `tests/test_state.py` (the `engine_error` group) and
`tests/test_audio_engine_client.py`.

## Running it

```bash
cd control-daemon
python3 -m venv .venv
.venv/bin/pip install -e ".[dev]"     # or: .venv/bin/pip install -r requirements.txt
.venv/bin/pytest                      # run the test suite
.venv/bin/control-daemon              # run the daemon (uvicorn on :8765 by default)
```

Configuration is via environment variables (see `src/control_daemon/main.py`):

| Variable                     | Default              | Meaning                       |
|-------------------------------|----------------------|-------------------------------|
| `CONTROL_DAEMON_STORE_PATH`   | `./data/state.json`  | Path to the JSON state file   |
| `CONTROL_DAEMON_HOST`         | `0.0.0.0`             | Bind host                     |
| `CONTROL_DAEMON_PORT`         | `8765`                | Bind port                     |
| `CONTROL_DAEMON_LOG_LEVEL`    | `info`                | uvicorn log level             |
| `CONTROL_DAEMON_AUDIO_ENGINE_SOCKET` | unset (uses `NullAudioEngineClient`) | Path to `audio-engine`'s control socket -- see "Audio engine wiring" below |

Uploaded `.nam`/IR binaries are written under `<store dir>/assets/`.

## Audio engine wiring

By default the daemon runs with `NullAudioEngineClient` (every engine call
is just logged) -- no `audio-engine` process required, which is how the
daemon's own test suite runs. To drive a real `audio-engine` process, set
`CONTROL_DAEMON_AUDIO_ENGINE_SOCKET` to the same control-socket path you
started it with (see `audio-engine/README.md`):

```bash
# terminal 1
./audio-engine/build/audio_engine /tmp/audio_engine.sock

# terminal 2
CONTROL_DAEMON_AUDIO_ENGINE_SOCKET=/tmp/audio_engine.sock .venv/bin/control-daemon
```

`UnixSocketAudioEngineClient` (`src/control_daemon/audio_engine_client.py`)
is the real implementation -- see its docstring for the resilience
posture (a missing/restarting engine process logs a warning and is
otherwise a no-op, rather than taking down the WS API) and for why
`set_tempo` is still a no-op there too (the engine's control-socket
protocol has no `set_tempo` command yet -- tempo isn't wired to any real
effect on either side).

## Resource footprint (Raspberry Pi constraint)

This daemon shares an 8GB-RAM-max Raspberry Pi with the real-time JUCE audio
engine, which is the process that actually needs the machine's RAM/CPU
budget. The daemon is designed to be a lightweight background process:

* **Minimal dependencies.** Runtime deps are FastAPI + uvicorn + pydantic +
  websockets -- nothing heavier (no pandas/numpy/ML libraries, no in-memory
  databases, no ORM). The JSON file written by `persistence.py` *is* the
  persistence layer.
* **Streaming asset upload.** `POST /assets/upload` reads the request body
  via `Request.stream()` and writes it to disk in fixed 1MiB chunks (see
  `app.py: upload_asset`), computing a running SHA-256 as it goes. The
  daemon never materializes an uploaded `.nam`/IR file as a single Python
  `bytes` object, however large it is.
* **No asset bytes in daemon memory, ever.** The daemon only ever holds
  `Asset` *metadata* -- id, filename, path, size, checksum (`models.py:
  Asset`). Decoding/loading the actual model/IR bytes is the audio engine's
  job, not the daemon's.
* **Measured**: `tests/test_upload_memory.py` uploads a synthetic ~20MB
  file over a real HTTP connection to a real uvicorn server and measures
  the process's peak RSS (`resource.getrusage(RUSAGE_SELF).ru_maxrss`,
  stdlib only, no `psutil`) before and after. Last measured on this dev
  container: **~4.2MB of RSS growth for a 20MB upload** (well under the
  file size, consistent with genuine streaming rather than whole-file
  buffering). See that test's docstring for why it deliberately runs a real
  socket server rather than driving the app through the in-process
  ASGI-transport test client, which can itself buffer request bodies in a
  way that would confound the measurement.

Anyone changing `upload_asset` (or adding a new binary-upload path) should
keep this property and re-run that test/benchmark.

## Running the tests

```bash
.venv/bin/pytest -v
```

Covers: software debounce (bouncy input + held-press cases), atomic
persistence round-trip (including the v1-\>v2 schema migration from the old
flat-preset shape, see "Rigs and presets" below), rig/preset CRUD + selection
transitions, footswitch mapping -> state change (including next/prev rig
wraparound and tap-tempo BPM estimation), role-based command rejection, a
full WebSocket integration test (app creates a rig + preset and selects it,
a display-role connection observes the broadcasts), the GPIO mock backend +
debounce integration, the `gpiozero` "not available on this platform" guard,
asset dedup-by-checksum, live block-param updates, asset rename, and the
upload memory benchmark above. 101 tests passing as of this writing
(`pytest -q`); re-run to get the current count rather than trusting this
number as it drifts.

## Rigs and presets

A **rig** is a backline: one ordered signal chain (`Rig.chain`) whose amp
and cab blocks are marked `pinned` (always on, never toggled by a preset),
plus whatever effects sit around them. A **preset** belongs to one rig
(`Rig.presets`) and only records which of that rig's *non-pinned* blocks are
enabled (`Preset.block_states`, keyed by block id) -- it never carries its
own amp/cab reference. The active position is `(active_rig_index,
active_preset_index)` rather than a bare preset id, since presets live
inside rigs and two rigs can each have a preset named "Solo".

This split is meant to be a real-time property, not just nicer modelling:
stepping presets within a rig should only flip per-block enable flags, so
it's fast and click-free on the audio thread. Stepping rigs is the
expensive path (a different amp/cab means the engine reloads a NAM model and
re-partitions an IR) -- the footswitch mapping reflects this with separate
`next_rig`/`prev_rig` vs. `next_preset`/`prev_preset` actions, and
preset-stepping deliberately never crosses into another rig (see
`FootswitchAction` below). The daemon still sends a full `load_preset` for
either; the engine keeps the running amp and cab when they're unchanged, so
an in-rig switch reads nothing from disk -- see "What a preset switch
reloads" in `audio-engine/README.md`.

Asset references (`asset_id`) live on individual blocks, not on the preset
or rig as a whole -- an amp block points at a `.nam` asset, a cab block at
an IR -- so chain order is explicit, more than one IR is possible in a
chain, and a future block type (e.g. a VST3 host) needs no new special
case. This replaced an earlier (schema v1) design where every preset was a
standalone chain carrying its own `nam_asset_id`/`ir_asset_id`; `models.py`'s
`_migrate_v1_to_v2` upgrades an old on-disk store by turning each v1 preset
into its own v2 rig automatically on load.

The audio engine is kept ignorant of all of this: the daemon flattens a
rig+preset pair into a `ResolvedPreset` (see `resolve_preset` in
`models.py`) -- the exact, already-resolved chain to play -- before sending
it over the control socket. See `audio-engine/README.md`'s "Shared data
model" for the engine-side shape.

## Architecture / module map

| Module                    | Responsibility |
|----------------------------|----------------|
| `models.py`                 | Pydantic models: `Rig`, `Preset`, `PresetBlockState`, `EffectBlock`, `Asset`, `ResolvedPreset`/`resolve_preset`, footswitch actions, the versioned `DaemonState`, the v1->v2 migration |
| `persistence.py`             | Atomic JSON load/save (temp file + `os.replace`) |
| `debounce.py`                 | Software debounce utility (clock-injectable, unit tested in isolation) |
| `gpio.py`                       | `FootswitchInputBackend` ABC, `MockFootswitchBackend`, `GpioZeroFootswitchBackend` (lazy import), `FootswitchInputController` (wires debounce on top of a backend) |
| `audio_engine_client.py`         | `AudioEngineClient` ABC + `NullAudioEngineClient` |
| `state.py`                        | `DaemonStateManager`: the single source of truth, all mutation methods, applies footswitch actions |
| `ws_protocol.py`                    | Pydantic models for every client<->server WS message |
| `connection_manager.py`              | Tracks connected sockets + roles, broadcast helper |
| `app.py`                              | FastAPI app: WS route + HTTP upload endpoint, wires everything together |
| `main.py`                              | uvicorn entrypoint, reads env config |

## WebSocket protocol

Connect to `ws://<host>:<port>/ws`. **The first message on every connection
must be `hello`**, declaring the client's role:

```json
{"type": "hello", "role": "app", "client_name": "iphone-baptiste"}
```

`role` is one of:

* `"app"` -- the mobile app. Full read/write: every configuration command
  below is app-only.
* `"footswitch"` -- a footswitch input relay. May only send
  `footswitch_press`.
* `"display"` -- the onboard display. Read-only: sends nothing but `hello`,
  receives every broadcast.

A non-`hello` first message, or any later message from a role that isn't
allowed to send it, gets a typed `error` reply (see below) instead of being
silently applied or silently ignored.

Right after a successful `hello`, the server sends a full `state_snapshot`.
From then on, **every state mutation is broadcast to every connected
client** (app, footswitch, display alike -- including the very connection
that caused the change) as `state_changed`, so every UI always stays in
sync, no matter which client (or the footswitch itself) triggered the
change. A command that mutates state also gets a direct `command_ok` reply
sent *only* to the connection that sent it, and that reply always arrives
before the resulting broadcast copy on that same connection.

### Server -> client messages

**`state_snapshot`** -- sent once, right after `hello`:
```json
{"type": "state_snapshot", "state": { "...": "full DaemonState, see models.py" }}
```

**`state_changed`** -- broadcast to all clients on every mutation:
```json
{"type": "state_changed", "reason": "select_preset", "state": { "...": "full DaemonState" }}
```
`reason` is one of: `create_rig`, `update_rig`, `delete_rig`,
`reorder_rigs`, `create_preset`, `update_preset`, `delete_preset`,
`select_preset`, `set_bypass`, `set_footswitch_mapping`, `register_asset`,
`rename_asset`, `set_block_param`, `footswitch_next_rig`,
`footswitch_prev_rig`, `footswitch_next_preset`, `footswitch_prev_preset`,
`tap_tempo`.

**`command_ok`** -- direct reply to the sender of a successful command:
```json
{"type": "command_ok", "command": "create_preset", "result": {"preset": {"...": "..."}}}
```

**`error`** -- typed error, sent to the connection that caused it:
```json
{"type": "error", "code": "role_forbidden", "message": "role 'display' may not send 'set_bypass'; this is an app-only command"}
```
`code` is one of: `hello_required`, `validation_error`, `role_forbidden`,
`unknown_message_type`, `not_found`, `internal_error`.

### Client -> server: app-only commands

```jsonc
// Create a rig: an ordered chain, amp/cab blocks marked pinned (always on)
{
  "type": "create_rig",
  "name": "Ampeg SVT",
  "chain": [
    {"id": "amp", "type": "nam", "asset_id": "nam1", "pinned": true, "enabled": true, "params": {}},
    {"id": "dist", "type": "distortion", "asset_id": null, "pinned": false, "enabled": false, "params": {"drive": 0.4}},
    {"id": "cab", "type": "ir", "asset_id": "ir1", "pinned": true, "enabled": true, "params": {}}
  ]
}

// Update a rig (any field omitted is left unchanged; sending `chain` replaces it wholesale)
{"type": "update_rig", "rig_id": "rig1", "name": "Ampeg SVT v2"}

// Delete a rig (and every preset inside it)
{"type": "delete_rig", "rig_id": "rig1"}

// Reorder rigs: every existing rig id, in the new order
{"type": "reorder_rigs", "rig_ids": ["rig2", "rig1"]}

// Create a preset inside a rig -- block_states only covers non-pinned blocks
{"type": "create_preset", "rig_id": "rig1", "name": "Solo", "block_states": {"dist": {"enabled": true, "params": {}}}}

// Update a preset (any field omitted is left unchanged)
{"type": "update_preset", "rig_id": "rig1", "preset_id": "abc123", "name": "Solo v2"}

// Delete a preset
{"type": "delete_preset", "rig_id": "rig1", "preset_id": "abc123"}

// Move the active position. Either index alone selects within the other's current value.
{"type": "select_preset", "rig_index": 0, "preset_index": 1}
{"type": "select_preset", "preset_index": 2}

// Global bypass
{"type": "set_bypass", "bypass": true}

// Replace the ENTIRE footswitch mapping (not a merge)
{
  "type": "set_footswitch_mapping",
  "mapping": {
    "0": {"type": "next_rig"},
    "1": {"type": "prev_rig"},
    "2": {"type": "next_preset"},
    "3": {"type": "prev_preset"},
    "4": {"type": "select_preset", "index": 0},
    "5": {"type": "toggle_bypass"},
    "6": {"type": "tap_tempo"}
  }
}

// Register metadata for a .nam/IR file already uploaded via HTTP (see
// below), or a .vst3 plugin bundle already installed out of band. For
// "kind": "vst3" the resulting asset also carries the plugin's own
// parameter schema (introspected by the engine as part of registering) --
// see BlockParamDescriptor in models.py. "display_name" is optional --
// the user-facing label a mobile picker UI shows (e.g. "Crunch lampes
// vintage") instead of the raw filename; omit it and the daemon defaults
// it to "filename" so the stored asset's display_name is never actually
// null.
{"type": "register_asset", "kind": "nam", "filename": "my_amp.nam", "stored_path": "/data/assets/abc123.nam", "size_bytes": 20971520, "sha256": "...", "display_name": "Crunch lampes vintage"}

// Rename an already-registered asset's display_name (e.g. the player
// wants a nicer label than the register_asset-time default of
// "filename"). Metadata only -- never touches the file on disk, its
// checksum, or its engine registration.
{"type": "rename_asset", "asset_id": "abc123", "display_name": "Crunch lampes vintage"}

// Live, no-reload parameter tweak on one block within one preset (rig +
// preset addressing -- see models.py's "Rigs and presets" docstring):
// persists into that preset's block_states[block_id].params override,
// forwarded to the engine only if this rig/preset is the one actually
// playing.
{"type": "set_block_param", "rig_id": "rig1", "preset_id": "abc123", "block_id": "dist", "param_key": "gain_db", "value": 6.0}

// Static per-engine-build parameter schema for every native block type
// (label/unit/min/max/default) -- a pure query, fetch once (e.g. on
// connect), not per-rig state. See audio-engine's block_type_registry.hpp.
{"type": "list_block_types"}
```

### Client -> server: footswitch-only event

```json
{"type": "footswitch_press", "switch_index": 0}
```

The daemon looks up `switch_index` in the current footswitch mapping and
applies the mapped action (`next_rig`, `prev_rig`, `next_preset`,
`prev_preset`, `select_preset`, `toggle_bypass`, or `tap_tempo`). A press on
an unmapped switch is a silent no-op. This is intentionally the *only*
thing a footswitch-role connection can send -- all editing happens in the
app.

`next_preset`/`prev_preset` step through the *current rig's* presets only,
wrapping around -- unlike `next_rig`/`prev_rig` (which swap the whole
backline, including its amp/cab, and reset to that rig's first preset).
Presets deliberately never step across a rig boundary: a preset's block ids
only mean anything against its own rig's chain, so a footswitch press that
silently swapped the amp mid-song would be a bug, not a feature. `rig`
changes are the expensive path (NAM model reload + IR re-partitioning);
`preset` changes within a rig keep the loaded amp and cab and only
rebuild the effect blocks.

### HTTP: uploading a `.nam`/IR binary

**Deliberate deviation from a literal reading of the spec:** raw binary
asset upload does **not** go over the WebSocket JSON protocol. Instead:

```
POST /assets/upload?kind=nam&filename=my_amp.nam
Body: raw binary bytes
```
returns:
```json
{"kind": "nam", "filename": "my_amp.nam", "stored_path": "/data/assets/<token>.nam", "size_bytes": 20971520, "sha256": "..."}
```

Why: a JSON WebSocket message is a poor fit for a multi-megabyte binary
blob (base64 inflates it by ~33%, and it would have to be held whole in
memory to encode/decode). A plain streamed HTTP POST is the natural
transport for a file upload, and lets the daemon write it straight to disk
in small chunks (see "Resource footprint" above) instead of ever holding it
whole in memory.

This intentionally splits the operation in two: the HTTP endpoint only
writes bytes to disk and returns their metadata (path, size, checksum) --
it does **not** touch daemon state. The app then sends `register_asset`
over the WS connection with that metadata to actually add the asset to
`DaemonState.assets` (and get it broadcast to everyone). This keeps a
single invariant: *every* daemon state mutation happens through the WS
command path, with one state manager as the only writer -- HTTP is used
purely for the dumb byte transfer, never as a second, parallel way to
mutate state.

**Duplicate uploads are rejected by content, not filename.** The same IR
pack routinely gets uploaded under different names (or the same name gets
reused for unrelated content), so the upload endpoint computes the SHA-256
as it streams and, once the body is fully written, checks it against every
already-registered asset (`DaemonStateManager.find_asset_by_sha256`). A
match deletes the just-written duplicate file and responds `409` instead of
handing back a second set of metadata for the same bytes:
```json
{"error": "duplicate_asset", "message": "identical content already registered as asset nam1 (my_amp.nam)", "existing_asset_id": "nam1"}
```
The caller (e.g. `scripts/load_test_preset.py`) is expected to reuse
`existing_asset_id` rather than treat this as a failure -- pointing two
rigs at the same cab IR is normal use, not an error.

## Design notes / deliberate deviations

* **Preset serialization format: plain JSON.** The spec flags this as an
  open question. JSON was chosen for human-inspectability, diffability, and
  because it is trivial for the not-yet-built mobile app to also parse
  directly if useful. The on-disk schema carries a top-level `"version"`
  field (`models.SCHEMA_VERSION`, currently `2`); `persistence.load_state`
  raises a clear error for an unrecognized version rather than silently
  misinterpreting it. This is no longer just a placeholder: the rig/preset
  restructuring (see "Rigs and presets" above) was exactly the kind of
  breaking schema change this was built for, and `_migrate_v1_to_v2`
  upgrades an old flat-preset store on load.
* **Binary asset upload over HTTP, not WS** -- see above, including
  content-checksum dedup.
* **Asset dedup is by content, not filename** -- see "Duplicate uploads are
  rejected by content, not filename" above.
* **Footswitch mapping is a full replace, not a merge.** `set_footswitch_mapping`
  replaces the entire mapping; the app is expected to always hold (and
  send) its complete desired mapping, so there is no daemon-side diffing of
  partial updates to get subtly wrong.
* **Broadcasts reach the sender too.** The spec says "broadcast ... to ALL
  connected clients ... so every UI stays in sync" -- taken literally, this
  includes the client that caused the change. Rather than special-casing
  "don't echo to the sender", every client (including the sender) gets
  exactly the same `state_changed` broadcast, in addition to the sender's
  own direct `command_ok`. This keeps one broadcast code path uniform for
  every role and every trigger (app edit, footswitch press, or a future
  second app instance).
* **`select_preset` rejects an out-of-range index rather than clamping or
  landing on nothing.** Unlike the old bank/slot model (which had explicit
  empty slots), a rig's `presets` list has no "empty" positions -- every
  index either names a real preset or is invalid -- so an out-of-range
  `rig_index`/`preset_index` is a `validation_error`, not a silent no-op.
* **Tap-tempo taps are not persisted to disk.** Every other mutation is
  persisted immediately (atomic JSON write), but a rapid series of
  footswitch tap-tempo presses deliberately only updates in-memory state
  and broadcasts it -- persisting every single tap would be needless
  storage wear for a transient performance value that nobody needs restored
  after a restart, unlike rigs/presets/footswitch-mapping configuration.
* **Two footswitch-input ingress paths, one convergence point.** Requirement
  6 (GPIO abstraction + software debounce) is implemented as a fully
  standalone, unit-tested unit (`gpio.py` + `debounce.py`,
  `FootswitchInputController`) for a future in-process reader driven by
  real GPIO hardware. Requirement 2's WS `footswitch_press` event is a
  second, independent ingress for an already-logical press (e.g. from a
  footswitch relay process). Both would ultimately call
  `DaemonStateManager.apply_footswitch_press(switch_index)` -- debouncing
  only makes sense on raw voltage-level pin readings, never on discrete WS
  JSON messages, so it is deliberately only wired into the GPIO path.
  `create_app()` does not start a `FootswitchInputController` by default
  (there is no real GPIO hardware in this repo/environment to read from);
  wiring one up on an actual Pi is a few lines in `main.py` using
  `GpioZeroFootswitchBackend`.
* **State mutation methods are synchronous, not async.** `DaemonStateManager`
  has no asyncio/websockets dependency at all -- it is plain, easily
  unit-testable Python. `app.py` bridges a mutation's resulting broadcast
  onto the event loop, either directly (mutation happened inside a WS
  handler coroutine, the common case) or via
  `asyncio.run_coroutine_threadsafe` (mutation happened on a different OS
  thread, e.g. a real GPIO interrupt callback).
