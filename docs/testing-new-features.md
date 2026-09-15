# Testing the Step 3a/3b/3c features (VST3, live params, tone stack/volume)

Quick command reference built while manually testing this branch. See
`scripts/README.md` for the original guitar-in walkthrough this extends.

## Build (one build dir with everything on)

Conda's GTest (`~/miniconda3/lib/cmake/GTest`, no rpath) can shadow
Homebrew's if a conda env is active -- pin it explicitly to avoid
`dyld: ... no LC_RPATH's found` when running the tests:

```bash
cmake -S audio-engine -B audio-engine/build-vst3 \
  -DAUDIO_ENGINE_WITH_PORTAUDIO=ON \
  -DAUDIO_ENGINE_WITH_VST3=ON \
  -DAUDIO_ENGINE_WITH_REAL_NAM=ON \
  -DGTest_DIR=/opt/homebrew/lib/cmake/GTest
cmake --build audio-engine/build-vst3 -j
ctest --test-dir audio-engine/build-vst3   # expect 130/130
```

- **NAM**: needs `AUDIO_ENGINE_WITH_REAL_NAM=ON` or every `.nam` file runs
  through `StubNamModel` -- a fixed makeup-gain passthrough, so different
  amps sound identical. `nam_render` (offline render/debug tool) is only
  built when this flag is on.
- **IR/cabinet convolution**: always real, no flag needed (resample ->
  length-cap -> energy-normalize -> time-domain convolve).
- **VST3**: needs `AUDIO_ENGINE_WITH_VST3=ON`, else `type: "vst3"` blocks
  throw a clear "not built with VST3 support" error. Packages the in-repo
  `GainTest.vst3` test plugin at
  `audio-engine/build-vst3/vst3_test_plugins/GainTest.vst3` -- no real
  third-party plugin binary needed to test hosting end to end.

## Run (3 terminals)

```bash
# 1 -- engine
./audio-engine/build-vst3/audio_engine /tmp/audio_engine.sock --audio

# 2 -- daemon
cd control-daemon && CONTROL_DAEMON_AUDIO_ENGINE_SOCKET=/tmp/audio_engine.sock .venv/bin/control-daemon

# 3 -- your existing scripts
bash scripts/init-presets.sh
control-daemon/.venv/bin/python3 scripts/keyboard_footswitch.py
```

## Raw WS commands for the new features

Not wired into `load_test_preset.py` yet, so drive them directly:

```bash
control-daemon/.venv/bin/python3 - <<'EOF'
import asyncio, json, websockets

async def main():
    async with websockets.connect("ws://127.0.0.1:8765/ws") as ws:
        await ws.send(json.dumps({"type": "hello", "role": "app", "client_name": "manual-test"}))
        snap = json.loads(await ws.recv())
        rig = snap["state"]["rigs"][0]
        preset = snap["state"]["presets"][0]

        # schema for every native block type
        await ws.send(json.dumps({"type": "list_block_types"}))
        print("block types:", await ws.recv())

        for b in rig["chain"]:
            print(b["id"], b["type"])

        # live, no-reload tweak (find real ids from the printout above)
        tone_id = next(b["id"] for b in rig["chain"] if b["type"] == "tone_stack")
        await ws.send(json.dumps({
            "type": "set_block_param", "rig_id": rig["id"], "preset_id": preset["id"],
            "block_id": tone_id, "param_key": "mid_db", "value": -6.0,
        }))
        print(await ws.recv())

asyncio.run(main())
EOF
```

**Register + use the in-repo VST3 test plugin:**
```bash
control-daemon/.venv/bin/python3 - <<'EOF'
import asyncio, json, websockets

BUNDLE = "audio-engine/build-vst3/vst3_test_plugins/GainTest.vst3"

async def main():
    async with websockets.connect("ws://127.0.0.1:8765/ws") as ws:
        await ws.send(json.dumps({"type": "hello", "role": "app", "client_name": "vst3-test"}))
        await ws.recv()
        await ws.send(json.dumps({
            "type": "register_asset", "kind": "vst3", "filename": "GainTest.vst3",
            "stored_path": BUNDLE,
        }))
        print(await ws.recv())  # carries the plugin's own parameter schema (its ParamID as "key")

asyncio.run(main())
EOF
```
Then add a `type: "vst3"` block referencing that `asset_id` to a rig's
chain (via `update_rig`, or the web app's Rig chain editor -- the Assets
screen itself has no UI for registering a `.vst3` bundle yet, only
nam/ir upload), and `set_block_param` with `param_key` = the plugin's
stringified `ParamID` from the reply above.

## Web app (Flutter web build of the mobile app)

No Chrome installed, so build + serve statically instead of
`flutter run -d chrome`:

```bash
cd mobile-app
flutter pub get
flutter build web
cd build/web && python3 -m http.server 8080
```
Open `http://localhost:8080` in any browser -- connects to
`ws://127.0.0.1:8765/ws` by default, and the daemon's CORS is wide open
(`allow_origins=["*"]`), so this works with no server changes.

- **Rig chain editor**: see the auto-added `tone_stack`/`volume` blocks;
  add a `vst3` block here once registered above.
- **Preset editor -> "Live controls"**: real sliders per block param,
  built from `list_block_types` (native) or `Asset.parameters` (vst3).
  Dragging sends `set_block_param` live, same path as the raw WS script.

## Resetting state between test runs

```bash
# with the daemon stopped
rm -rf control-daemon/data/state.json control-daemon/data/assets
```
