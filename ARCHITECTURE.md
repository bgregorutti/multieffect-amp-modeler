# Architecture

## Overview

```
┌─────────────────┐        WebSocket         ┌──────────────────────────┐
│   Mobile app      │ ◄──────────────────────► │   Control daemon           │
│  (Flutter)         │                          │  (preset state,            │
└─────────────────┘                          │   source of truth)         │
                                              └──────────┬────────────────┘
┌─────────────────┐         GPIO                         │
│  Footswitch        │ ─────────────────────────────────────┤
│  (Raspberry Pi)    │                                       │
└─────────────────┘                                       ▼
                                              ┌──────────────────────────┐
┌─────────────────┐        (optional)         │   Audio engine              │
│  Onboard display    │ ◄────────────────────── │   (real-time plugin host)  │
│  (touch or simple)  │                          └──────────────────────────┘
└─────────────────┘
```

The **control daemon** is the single source of truth for "which preset is
active" and for all preset/bank/footswitch-mapping configuration. It
notifies every connected client (footswitch relay, mobile app, display) on
every state change, so all interfaces stay consistent — including changes
triggered by the footswitch itself. This is why it's built and tested first:
every other component (audio engine, app, footswitch relay, display) is a
client of its API, so the API contract is the thing worth stabilizing early.

## Components

| Component | Language/Stack | Status |
|---|---|---|
| Control daemon | Python, FastAPI, WebSockets | In progress (`control-daemon/`) |
| Audio engine | C++, JUCE (planned) | Not started |
| Mobile app | Flutter | Not started |
| Footswitch relay | Python/C, GPIO (Pi) | Not started |
| Onboard display | I2C LCD/OLED, driven by daemon broadcasts | Not started |

## Design decisions

- **Preset serialization format: JSON.** Versioned schema (top-level
  `"version"` field) so the on-disk format can evolve. Atomic writes
  (write-temp + rename) to survive a crash mid-write. See
  `control-daemon/src/control_daemon/persistence.py` and
  `control-daemon/src/control_daemon/models.py`.

- **Binary asset upload (`.nam` models, cabinet IRs) goes over plain HTTP
  POST, not the WebSocket JSON protocol.** The spec's WS API description
  mentions "file uploads" as one of the app's commands, but raw binary
  doesn't belong in a JSON message protocol. The daemon exposes an HTTP
  upload endpoint; the WebSocket protocol carries only the resulting asset
  metadata (id/name/path) so presets can reference uploaded assets. This is
  a deliberate deviation from a literal reading of section 4.2 — documented
  here and in `control-daemon/README.md`.

- **Footswitch wiring has no signal path through it.** Audio never passes
  through the footswitch — everything happens in software — so a simple
  momentary SPST switch (one leg to a pull-up GPIO pin, other to ground) is
  sufficient; no 3PDT true-bypass switching hardware is needed. Debounce (a
  few ms) is handled entirely in the control daemon in software, not in
  hardware and not on the footswitch relay. See
  `control-daemon/src/control_daemon/debounce.py`.

- **GPIO and audio-engine integration are abstracted behind interfaces** so
  the control daemon runs and is fully unit/integration-tested on a normal
  Linux dev machine with no Raspberry Pi, no GPIO library, and no real audio
  engine present. See `control-daemon/src/control_daemon/gpio.py`
  (`FootswitchInputBackend`, with a `MockFootswitchBackend` for dev/tests)
  and `control-daemon/src/control_daemon/audio_engine_client.py`
  (`AudioEngineClient`, with a `NullAudioEngineClient` placeholder until the
  real JUCE-based engine exists).

- **Client roles are distinguished on WebSocket connect** (`app`,
  `footswitch`, `display`) via a `hello` message. Footswitch/display
  connections are read-only/trigger-only per the guiding UX principle —
  configuration-editing commands from those roles are rejected with a typed
  error rather than silently applied.

## Open questions

Tracked in [`docs/open-questions.md`](./docs/open-questions.md) as they get
resolved (Pi 4 vs 5, USB audio interface choice, crossfade/preloading
strategy for glitch-free preset switching, whether the onboard display ships
in V1, etc.).
