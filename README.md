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
control-daemon/   Python service: source of truth for preset/bank state,
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

docs/             Cross-cutting design notes and open questions.
```

## Status

- `control-daemon`: **V1 built and tested** (40 passing tests) — see its own
  README for how to run it and the WebSocket protocol reference.
- `audio-engine`: **V1 built and tested** (68 passing tests) — real DSP,
  WAV/IR loading, and preset-switching logic; JUCE audio I/O and real NAM
  inference are deferred (both require GitHub-hosted dependencies this
  sandbox's network can't fetch — see audio-engine/README.md). Not yet wired
  to control-daemon's `AudioEngineClient`.
- `mobile-app`: **V1 built and tested** (77 passing tests via `flutter test`,
  `flutter analyze` clean) — full preset/bank/footswitch-mapping editing and
  asset upload against control-daemon's protocol. OS file-picker integration
  is stubbed pending on-device testing (needs a real phone/emulator).
- `footswitch`, `display`: not yet started — real GPIO/I2C hardware is
  needed to build and validate these properly.
