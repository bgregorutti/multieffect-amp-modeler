# mobile-app

The Flutter mobile app for the DIY AI guitar multi-effects pedal. This is
where all editing complexity lives: creating/editing presets, building
effects chains, organizing banks, configuring the footswitch mapping, and
uploading `.nam`/IR files. The physical footswitch and any onboard display
are read-only/trigger-only -- the control daemon (`control-daemon/` in this
repo) is the single source of truth, and this app is simply its editing and
monitoring surface, reflecting live state in real time (including changes
triggered by someone stomping the footswitch while the app is open).

See `control-daemon/README.md` for the authoritative protocol reference.
Everything in this app's `lib/models/` and `lib/services/daemon_client.dart`
is a direct translation of that document.

## Running it

```bash
export PATH="/opt/flutter-sdk/flutter/bin:$PATH"   # or wherever your Flutter SDK lives
cd mobile-app
flutter pub get
flutter analyze
flutter test
```

There is no `flutter run` target exercised in CI/sandboxes without a device,
emulator, or desktop toolchain -- see "What's stubbed" below. Everything
else (all business logic, all screens) is fully covered by `flutter test`,
which uses Flutter's headless "flutter tester" engine and needs none of
that.

## Architecture

```
lib/
  models/     Plain Dart data classes + manual fromJson/toJson (no code-gen)
  services/   DaemonClient (WebSocket) and AssetUploadService (HTTP upload)
  state/      DaemonStateController: a ChangeNotifier wrapping DaemonClient
  screens/    One StatelessWidget/StatefulWidget per top-level screen
  main.dart   Wires it together behind a bottom-navigation shell
```

**Data flow.** `DaemonClient` owns the single WebSocket connection to the
daemon. On connect it sends `hello` with `role: "app"`. Every
`state_snapshot`/`state_changed` message it receives is parsed into a fresh
`DaemonState` and republished via `client.state` + `client.stateStream` --
this is the *only* thing that updates the exposed state. There is
deliberately no separate optimistic local state anywhere in this app: a
screen calls a command method (e.g. `createPreset`), that resolves once the
matching `command_ok` arrives, and the UI change the user actually sees
comes from the `state_changed` broadcast the daemon sends right after (which
arrives on the same connection, whether the mutation was caused by this
app, another app instance, or a footswitch press). This is why the protocol
docs' "broadcasts reach the sender too" design note matters: this app relies
on it instead of updating its own view of the world speculatively.

`DaemonStateController` (`lib/state/daemon_state_controller.dart`) is a thin
`ChangeNotifier` over a `DaemonClientBase` so screens can rebuild via
`ListenableBuilder` without pulling in a state-management package -- this
app intentionally has no riverpod/bloc/provider dependency.

**Testability.** Every screen takes a `DaemonStateController` (itself
wrapping anything implementing `DaemonClientBase`), so widget tests inject
`test/screens/fake_daemon_client.dart` -- an in-memory fake that records
every command sent -- instead of a real socket. `DaemonClient` and
`AssetUploadService` themselves are tested against **real local servers**
(`dart:io` `HttpServer`/`WebSocketTransformer`), not in-process fakes -- see
"Protocol-compatibility notes" below for why.

## Live parameter controls (dynamic per-block schema)

`PresetEditorScreen`'s "Live controls" section renders one real `Slider`
per adjustable parameter of every block in the active preset's rig --
gain, drive, blend, whatever the block actually has -- from a schema
fetched from the daemon, not a hardcoded "always a gain slider" UI (the
same way a DAW's generic plugin view works):

- **Native block types** (gain, volume, eq, tone_stack, delay): schema
  comes from `list_block_types`, fetched once when the screen opens
  (`BlockTypeDescriptor`/`BlockParamDescriptor`, `lib/models/
  block_param_descriptor.dart`) -- static per engine build, not per-rig
  state.
- **`vst3` blocks**: schema is per-*asset*, not per-type (different
  `.vst3` files have different parameters) -- it rides along on
  `Asset.parameters`, already populated by `register_asset`'s reply, so
  no extra round-trip is needed once a rig references the plugin.
- A block with neither (an experimental/unrecognized type) simply gets no
  live-controls card -- fail safe, not a broken or empty one.

Dragging a slider updates the local value immediately (so the UI never
waits on a round-trip to feel responsive) and sends `set_block_param`
(`rig_id`/`preset_id`/`block_id`/`param_key`/`value`) on every tick --
live, not gated on the screen's own Save button, same pattern the
now-removed Step-2 "Amp" panel established for gain/tone knobs. The value
persists into *this preset's* block-state override, so the same plugin or
block can sit at different settings per preset within one rig.

This is a live per-preset override, distinct from editing a block's own
*default* parameters (what a freshly-created preset starts from), which
stays `RigChainEditorScreen`'s generic key/value `_ParamsEditor` -- an
edit-time, save-gated concern on the rig itself.

## Protocol-compatibility notes

- `test/models/protocol_fixtures.dart` copies the example JSON payloads from
  `control-daemon/README.md`'s "WebSocket protocol" section **verbatim**
  (plus one full `DaemonState` example built field-by-field from
  `control_daemon/src/control_daemon/models.py`, since the README itself
  doesn't spell out a complete state object). Every model's round-trip test
  parses/serializes against these fixtures, so a change to the daemon's
  documented wire format that isn't mirrored here will fail
  `flutter test`, not just silently drift. If the daemon's README examples
  change, update this file to match.
- `EffectBlock.type`/`.params` and the whole footswitch mapping are treated
  as opaque/free-form here, exactly as the daemon treats them -- there is
  deliberately no hardcoded catalog of "known" effect types anywhere in this
  app (see `PresetEditorScreen`'s generic block/param editor).
- The daemon's `footswitch_mapping` and `DaemonState.footswitch_mapping` use
  `Dict[int, FootswitchAction]` in Python, which pydantic serializes with
  **string** keys on the wire (`"0"`, `"1"`, ...) because JSON object keys
  are always strings. `DaemonState.footswitchMapping` in this app exposes
  `Map<int, FootswitchAction>` for convenient Dart use, converting to/from
  the wire's string keys at the JSON boundary
  (`daemon_state.dart`/`ws_messages.dart`).
- `DaemonClient` (`lib/services/daemon_client.dart`) is tested in
  `test/services/daemon_client_test.dart` against a genuine local WebSocket
  server (`test/services/fake_control_daemon.dart`, a small `dart:io`
  `HttpServer` + `WebSocketTransformer`), not an in-process fake transport.
  This mirrors the same principle behind
  `control-daemon/tests/test_upload_memory.py` (see that file's docstring):
  an in-process fake can hide real protocol/framing bugs that only surface
  over an actual socket.
- `AssetUploadService`'s HTTP step (`lib/services/asset_upload_service.dart`)
  is likewise tested in `test/services/asset_upload_service_test.dart`
  against a real local `HttpServer`, asserting the request body arrives as
  raw bytes (not multipart) byte-for-byte, and that `kind`/`filename` land
  correctly as query params -- exactly the contract in
  `control-daemon/README.md`'s "HTTP: uploading a .nam/IR binary" and the
  real streaming endpoint in `control_daemon/app.py: upload_asset`.
  Registering the asset afterwards (`register_asset` over the WS
  connection) is exercised against a fake `DaemonClientBase` in the same
  test file, and against the real fake-server `DaemonClient` implicitly via
  the model round-trip and daemon-client tests.
- Command JSON is built by hand (`lib/models/ws_messages.dart`) to match the
  daemon's pydantic models field-for-field, including **omitting** unset
  optional fields on `update_preset`/`update_bank` (the daemon treats a
  present-but-null field differently from an absent one only in the sense
  that "any field omitted is left unchanged" -- see the README) versus
  **always sending** `nam_asset_id`/`ir_asset_id` (possibly `null`) on
  `create_preset`, matching the README's example byte-for-byte.

## Why manual JSON instead of `json_serializable`/`build_runner`

Per the task brief, this deliberately avoids a code-gen build step (no
`build_runner`) to keep the build simple and dependency-light in this
environment. Every model has hand-written `fromJson`/`toJson`, verified by
the round-trip tests described above. This is more boilerplate than
annotations + code-gen, but it's boilerplate that's easy to read, easy to
`flutter test` without an extra build phase, and (arguably) an easier
"catches a wire-format mismatch instantly" property.

## What's stubbed (needs real-device follow-up)

**The OS file picker.** `AssetsScreen` (`lib/screens/assets_screen.dart`)
takes an injected `FilePickerCallback` (`Future<PickedFile?> Function(String
kind)`) rather than calling a concrete file-picker package directly. A real
build should wire this to something like the `file_picker` pub.dev package,
which needs Android/iOS/desktop platform channels -- unavailable in this
sandbox (no Android SDK, no iOS toolchain, no Chrome/GTK desktop libs, and
`flutter run`/an emulator/`integration_test` on a real device aren't
exercisable here). `main.dart`'s `HomeShell._stubFilePicker` is a placeholder
that shows a snackbar and returns `null` -- **replace this before shipping**.
Everything downstream of "a file was picked" (the two-step HTTP-upload +
`register_asset` flow in `AssetUploadService`) is fully implemented and
tested against real local servers, so wiring in a real picker is the only
remaining step for on-device asset uploads.

Nothing else is stubbed: every screen, every command, and the full
WebSocket connection lifecycle (connecting/connected/disconnected/error) are
implemented and covered by `flutter test`.

## Known limitations / follow-ups

- The daemon host/port is currently a compile-time constant
  (`kDefaultDaemonHost`/`kDefaultDaemonPort` in `main.dart`, defaulting to
  `127.0.0.1:8765`). A real deployment will want a settings screen (backed
  by e.g. `shared_preferences`) to point the app at the pedal's actual
  address -- out of scope for this first pass.
- `DaemonClient` does not currently auto-reconnect after a dropped
  connection; `status` correctly reflects `disconnected`/`error`, and
  `ConnectionScreen` offers a manual "Connect" button, but automatic
  backoff-retry would be a natural next step for real hardware use (Wi-Fi
  drops, pedal reboots, etc).
