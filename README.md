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

audio-engine/     (planned) C++/JUCE real-time plugin host: loads NAM models,
                  cabinet IRs, and the effects chain; talks to the control
                  daemon.

mobile-app/       (planned) Flutter app: connects to the pedal over local
                  Wi-Fi, builds/edits presets, uploads NAM/IR assets.

footswitch/       (planned) GPIO event relay running on the Pi, talking to
                  the control daemon's WebSocket API.

display/          (planned) Minimal read-only display client (I2C
                  LCD/OLED), driven by control-daemon broadcasts.

docs/             Cross-cutting design notes and open questions.
```

## Status

- `control-daemon`: in progress — see its own README for how to run it and
  its test suite.
- All other components: not yet started.
