# Multi-Effect Amp Modeler

A DIY AI-powered guitar multi-effects pedal, inspired by the Quad Cortex / MOD
Duo: amp simulation via neural networks ([NAM — Neural Amp
Modeler](https://github.com/sdatkinson/neural-amp-modeler)), cabinet impulse
response loading, a general effects chain, live preset switching via a
physical footswitch, and full preset editing from a mobile app.

Guiding UX principle: **all editing complexity lives in the mobile app.** The
footswitch and the onboard display only show state and trigger transitions —
no configuration logic should ever be required without the phone.

See [`ARCHITECTURE.md`](./ARCHITECTURE.md) for the full system design and
[`docs/open-questions.md`](./docs/open-questions.md) for open decisions being
resolved as the project progresses.

## Repository layout

```
control-daemon/   Python service: source of truth for rig/preset state,
                  WebSocket API for the app/footswitch/display, JSON
                  persistence. See control-daemon/README.md.

audio-engine/     C++20 real-time plugin host skeleton: preset/asset model,
                  real DSP blocks, WAV/IR loading, .nam metadata parsing,
                  crossfade preset switching, a local control socket. JUCE
                  integration deferred (see audio-engine/README.md). See
                  audio-engine/README.md.

mobile-app/       Flutter app: connects to the daemon's WebSocket API over
                  local Wi-Fi, builds/edits presets and banks, configures
                  the footswitch mapping, uploads NAM/IR assets. See
                  mobile-app/README.md.

footswitch/       (planned) GPIO event relay running on the Pi, talking to
                  the control daemon's WebSocket API.

display/          (planned) Minimal read-only display client (I2C
                  LCD/OLED), driven by control-daemon broadcasts.

vst-python/       Python DSP pedal models (Big Muff, Tube Screamer, noise
                  gate): the reference implementation the audio-engine's
                  C++ pedal blocks are ported from and validated against.
                  See vst-python/README.md.

deploy/           Raspberry Pi provisioning script + step-by-step deployment
                  guide: systemd services, Wi-Fi access point setup. See
                  deploy/README.md.

docs/             Cross-cutting design notes and open questions.

scripts/          Dev-machine testing tools that don't belong to any one
                  component: load a test preset without the mobile app,
                  and a keyboard stand-in for a footswitch relay
                  (right/left arrow = next/previous preset). See
                  scripts/README.md for the full guitar-in-hear-it-out
                  walkthrough.
```

## Status

- `control-daemon`: **V1 built and tested** (111 passing tests as of this
  writing; re-run `.venv/bin/pytest -q` for the current count) — see its own
  README for how to run it and the WebSocket protocol reference.
  `AudioEngineClient` now has a real implementation
  (`UnixSocketAudioEngineClient`) wired to audio-engine's control socket
  (`CONTROL_DAEMON_AUDIO_ENGINE_SOCKET`) — see "Audio engine wiring" in
  control-daemon/README.md, including asset re-registration on an engine
  reconnect and `engine_error` surfacing when the engine rejects a load.
- `audio-engine`: **V1 built and tested** (151 passing tests by default;
  re-run `ctest --test-dir audio-engine/build` for the current count) —
  real DSP, WAV/IR loading, and preset-switching logic, now including three
  modelled stompbox pedals (`big_muff`/`tube_screamer`/`noise_gate`, ported
  from and validated against the Python reference in `vst-python/`) and
  real VST3 plugin hosting (opt-in via `-DAUDIO_ENGINE_WITH_VST3=ON`, no
  JUCE) alongside the native block types. The engine only ever receives an
  already-flattened, rig-agnostic chain from the daemon — see "Shared data
  model" in audio-engine/README.md. Real NAM (WaveNet/LSTM) inference now
  works, opt-in via
  `-DAUDIO_ENGINE_WITH_REAL_NAM=ON` (vendors NeuralAmpModelerCore via
  CMake FetchContent, off by default) — see "Real NAM inference" in
  audio-engine/README.md, including measured real-time cost (4-6% of
  budget) against two real commercial `.nam` files. Real-time audio device
  I/O exists for dev-machine testing via PortAudio (`audio_engine --audio`,
  opt-in build flag, off by default) — see "Real-time audio I/O" in
  audio-engine/README.md; what the Raspberry Pi build itself uses
  (PortAudio again, ALSA, or JUCE) is still open, but `IAudioIoBackend`
  means that's a new backend behind an existing interface, not a rewrite.
  Crossfade-on-switch is not yet wired into the real-time path (known gap,
  documented in audio-engine/README.md).
- `mobile-app`: **V1 built and tested** (passing via `flutter test`,
  `flutter analyze` clean as of this writing — this environment has no
  Flutter SDK to re-run the count against, so treat any specific number as
  unverified rather than trusting a hardcoded one) — full rig/preset/
  footswitch-mapping editing and asset upload against control-daemon's
  protocol, including the rig/preset split (`RigListScreen`,
  `RigChainEditorScreen`, `PresetListScreen`, `PresetEditorScreen`).
  Adding/removing/reordering chain blocks lives only in the rig editor now
  (a rig's chain is shared by every preset in it); the preset editor only
  toggles and tweaks what the rig already has — see "Rigs are boards,
  presets stomp them" in mobile-app/README.md. OS file-picker integration
  is stubbed pending on-device testing (needs a real phone/emulator).
- `footswitch`, `display`: not yet started — real GPIO/I2C hardware is
  needed to build and validate these properly. `next_rig`/`prev_rig`
  (swap the whole backline) and `next_preset`/`prev_preset` (step within
  the current rig, never crossing into another one) footswitch actions are
  already supported by control-daemon, and exercised today by
  `scripts/keyboard_footswitch.py`'s four-switch (plus bypass) keyboard
  mapping, for whenever a real pedal is the first one wired up.
- `deploy`: **script + guide written**, shellchecked and validated piece by
  piece in this dev environment (unit-file verification, production build
  paths) — but **not yet run end to end on real Raspberry Pi hardware**. See
  deploy/README.md's "Honesty check" before running it.
