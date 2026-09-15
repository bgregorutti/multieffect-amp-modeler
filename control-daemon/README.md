# control-daemon

The control daemon for the DIY AI guitar multi-effects pedal. It is the
single source of truth for "which preset is active" and for all
preset/bank/footswitch configuration:

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
persistence round-trip, preset/bank CRUD + selection transitions,
footswitch mapping -> state change (including next/prev bank wraparound and
tap-tempo BPM estimation), role-based command rejection, a full WebSocket
integration test (app creates + selects a preset, a display-role connection
observes the broadcasts), the GPIO mock backend + debounce integration, the
`gpiozero` "not available on this platform" guard, and the upload memory
benchmark above.

## Architecture / module map

| Module                    | Responsibility |
|----------------------------|----------------|
| `models.py`                 | Pydantic models: `Preset`, `Bank`, `Asset`, footswitch actions, the versioned `DaemonState` |
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
`reason` is one of: `create_preset`, `update_preset`, `delete_preset`,
`select_preset`, `create_bank`, `update_bank`, `reorder_banks`,
`set_bypass`, `set_footswitch_mapping`, `register_asset`, `rename_asset`,
`footswitch_next_bank`, `footswitch_prev_bank`, `footswitch_next_preset`,
`footswitch_prev_preset`, `tap_tempo`.

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
// Create a preset
{"type": "create_preset", "name": "Ambient Swell", "blocks": [{"type": "reverb", "enabled": true, "params": {"decay": 4.2}}], "nam_asset_id": null, "ir_asset_id": null}

// Update a preset (any field omitted is left unchanged)
{"type": "update_preset", "preset_id": "abc123", "name": "Ambient Swell v2"}

// Delete a preset
{"type": "delete_preset", "preset_id": "abc123"}

// Select by preset id, OR by (bank_index, slot) -- not both
{"type": "select_preset", "preset_id": "abc123"}
{"type": "select_preset", "bank_index": 0, "slot": 2}

// Create a bank (num_slots defaults to 4)
{"type": "create_bank", "name": "Live Set 1", "num_slots": 4}

// Update a bank's name and/or its slot -> preset_id assignments
{"type": "update_bank", "bank_id": "bank1", "slots": ["abc123", null, null, null]}

// Reorder banks: every existing bank id, in the new order
{"type": "reorder_banks", "bank_ids": ["bank2", "bank1"]}

// Global bypass
{"type": "set_bypass", "bypass": true}

// Replace the ENTIRE footswitch mapping (not a merge)
{
  "type": "set_footswitch_mapping",
  "mapping": {
    "0": {"type": "select_slot", "slot": 0},
    "1": {"type": "select_slot", "slot": 1},
    "2": {"type": "next_bank"},
    "3": {"type": "toggle_bypass"},
    "4": {"type": "tap_tempo"},
    "5": {"type": "next_preset"},
    "6": {"type": "prev_preset"}
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
applies the mapped action (`select_slot`, `next_bank`, `prev_bank`,
`toggle_bypass`, `next_preset`, `prev_preset`, or `tap_tempo`). A press on
an unmapped switch is a silent no-op. This is intentionally the *only*
thing a footswitch-role connection can send -- all editing happens in the
app.

`next_preset`/`prev_preset` step through every *assigned* slot across all
banks, in bank order then slot order, skipping empty slots and wrapping
around -- unlike `next_bank`/`prev_bank` (which keep the same slot index
and can land on an empty one), this always lands on a real preset if one
exists anywhere. Meant for a minimal footswitch (or `scripts/
keyboard_footswitch.py`, see its docstring) that wants to browse the whole
preset list with just two switches instead of one per slot.

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

## Design notes / deliberate deviations

* **Preset serialization format: plain JSON.** The spec flags this as an
  open question. JSON was chosen for human-inspectability, diffability, and
  because it is trivial for the not-yet-built mobile app to also parse
  directly if useful. The on-disk schema carries a top-level `"version"`
  field (`models.SCHEMA_VERSION`); `persistence.load_state` raises a clear
  error for an unrecognized version rather than silently misinterpreting
  it, as a placeholder for a real migration step once the schema changes.
* **Binary asset upload over HTTP, not WS** -- see above.
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
* **Selecting an empty bank slot clears the active preset** rather than
  erroring -- pressing a footswitch mapped to an empty slot, or the app
  selecting one, is a valid "nothing here" state, not a validation failure.
* **Tap-tempo taps are not persisted to disk.** Every other mutation is
  persisted immediately (atomic JSON write), but a rapid series of
  footswitch tap-tempo presses deliberately only updates in-memory state
  and broadcasts it -- persisting every single tap would be needless
  storage wear for a transient performance value that nobody needs restored
  after a restart, unlike presets/banks/footswitch-mapping configuration.
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
