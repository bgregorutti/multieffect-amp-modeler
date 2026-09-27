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

The **control daemon** is the single source of truth for "which rig/preset
is active" and for all rig/preset/footswitch-mapping configuration. It
notifies every connected client (footswitch relay, mobile app, display) on
every state change, so all interfaces stay consistent — including changes
triggered by the footswitch itself. This is why it's built and tested first:
every other component (audio engine, app, footswitch relay, display) is a
client of its API, so the API contract is the thing worth stabilizing early.

## Components

| Component | Language/Stack | Status |
|---|---|---|
| Control daemon | Python, FastAPI, WebSockets | V1 built and tested (`control-daemon/`) |
| Audio engine | C++20, PortAudio (JUCE deferred, see audio-engine/README.md) | V1 built and tested (`audio-engine/`) |
| Pedal DSP reference | Python (`vst-python/`) | Reference implementation the C++ pedal blocks are ported from and validated against -- not what runs on the Pi |
| Mobile app | Flutter | V1 built and tested (`mobile-app/`) |
| Footswitch relay | Python/C, GPIO (Pi) | Not started -- needs real hardware, see root README "Status" |
| Onboard display | I2C LCD/OLED, driven by daemon broadcasts | Not started -- needs real hardware, see root README "Status" |
| Deployment | Bash + systemd + NetworkManager | Script + guide written (`deploy/`); not yet run on real hardware |

See the root [`README.md`](./README.md) "Status" section for current test
counts and per-component detail; this table is deliberately just a
one-line-per-component snapshot, not the source of truth for numbers.

## Design decisions

- **Presets are scoped to a rig, not standalone.** A rig owns one ordered
  chain whose amp and cab blocks are `pinned` (always on); a preset only
  records which of that rig's other blocks are enabled. This replaced an
  earlier design where each preset carried its own amp/cab reference
  directly, which left chain order undefined, allowed only one IR, and gave
  no way to bypass the cab alone. The split is meant to be a real-time
  property, not just modelling: stepping presets within a rig should only
  flip enable flags (fast, click-free), while stepping rigs reloads the NAM
  model and re-partitions the IR (the expensive, between-songs path) — the
  footswitch mapping follows this exactly (`next_rig`/`prev_rig` vs.
  `next_preset`/`prev_preset`, the latter never crossing a rig boundary).
  The audio engine is kept ignorant of the split: the daemon flattens
  rig + preset into a `ResolvedPreset` and sends the exact chain to play.
  **Not yet realised in the engine:** `ResourceManager::loadPreset`
  rebuilds the whole chain on every `load_preset` — re-reading the `.nam`
  and IR from disk even when only an effect toggled — while holding the
  lock the audio callback needs, so today an in-rig preset switch costs the
  same as a rig switch. The data model and daemon side are done; the engine
  side is tracked in `docs/open-questions.md` #5.
  See "Rigs and presets" in `control-daemon/README.md` and "Shared data
  model" in `audio-engine/README.md`.

- **Pedal effects are hand-written DSP, not NAM captures.** A `.nam` file
  freezes the knob positions it was captured at and spends a neural
  inference to reproduce what is often just a multiply (volume) or a
  one-pole filter (tone); only the nonlinear clipping stage is genuinely
  worth modelling that way, and even then a NAM capture can't move once
  trained, which conflicts with the mobile app's per-block live parameter
  editing. Each modelled pedal (`big_muff`, `tube_screamer`, `noise_gate`)
  is designed and measured first in Python (`vst-python/`), then ported to
  a C++ `EffectBlock` and validated against golden renders from the Python
  reference (`audio-engine/tests/test_python_parity.cpp`). See
  `vst-python/README.md` and "Modelled pedal blocks" in
  `audio-engine/README.md`.

- **VST3 plugins can be hosted as chain blocks, without JUCE.** A rig's
  chain can include any number of `type: "vst3"` blocks alongside the
  native ones, loaded via a hand-written COM-style host against vendored
  `pluginterfaces` only (opt-in, `-DAUDIO_ENGINE_WITH_VST3=ON`). This
  reverses this project's earlier "LV2/VST3 hosting is out of scope"
  stance for VST3 specifically, once it became clear a full host doesn't
  need JUCE or `public.sdk` — see "VST3 plugin hosting" in
  `audio-engine/README.md`.

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
  (`AudioEngineClient`: `NullAudioEngineClient` when no engine socket is
  configured, `UnixSocketAudioEngineClient` to drive a real `audio-engine`).

- **Client roles are distinguished on WebSocket connect** (`app`,
  `footswitch`, `display`) via a `hello` message. Footswitch/display
  connections are read-only/trigger-only per the guiding UX principle —
  configuration-editing commands from those roles are rejected with a typed
  error rather than silently applied.

- **Deployment: systemd services + NetworkManager AP, not a custom init
  system or hand-rolled hostapd/dnsmasq config.** `control-daemon` and
  `audio-engine` run as systemd services under a dedicated unprivileged
  system user, with `ProtectSystem=strict`/`ReadWritePaths`/`ReadOnlyPaths`
  sandboxing scoped to exactly the persistent data directory each needs —
  standard, auditable, and gives `Restart=on-failure` + boot-time
  autostart for free. The Wi-Fi access point (spec section 3
  "Connectivity") uses NetworkManager's built-in `ipv4.method shared` AP
  mode rather than manually configuring `hostapd`/`dnsmasq`/`dhcpcd`,
  since current (Bookworm+) Raspberry Pi OS ships NetworkManager by
  default and `shared` mode already runs the DHCP server + NAT a
  from-scratch AP needs. See `deploy/README.md` for the older-OS fallback
  recipe. Persistent data (`/var/lib/multieffect-amp-modeler/`) lives
  outside the git checkout so a redeploy (`git pull` + re-run) never
  touches rigs/presets/uploaded assets.

## Resource constraints

Target hardware is a Raspberry Pi 4 or 5 with **8GB RAM as the maximum
configuration** — that budget is shared with the real-time audio engine
(which loads NAM models and cabinet IRs and must never be starved or
swapped), so every other process on the box has to stay deliberately small:

- **Control daemon**: pure Python/FastAPI/websockets, JSON-file persistence
  — no databases, no ML/data libraries, no in-memory caching of large
  objects. It tracks *metadata* for uploaded `.nam`/IR assets only (id,
  filename, path, size, checksum); it never holds decoded model/IR bytes in
  memory — that's the audio engine's job, and only for the one active
  preset (running multiple simulations in parallel is explicitly out of
  scope for V1, which keeps that footprint bounded to a single model + a
  single IR at a time).
- **Binary asset uploads are streamed to disk**, never buffered whole in a
  Python object, so a 50MB IR upload doesn't cost 50MB of daemon RSS.
- Every component should be able to state (and, once hardware exists,
  measure) its own steady-state and peak memory footprint. The control
  daemon has an in-repo benchmark proving upload streaming doesn't spike
  RSS with file size — see `control-daemon/README.md` for the measured
  numbers.

## Testing strategy: no hardware required

None of this project's software should require a Raspberry Pi, a real audio
interface, real GPIO, or a real JUCE build to be developed and tested:

- The control daemon's WebSocket API is tested end-to-end with FastAPI/
  Starlette's in-process test client (`websocket_connect`) — no real
  network socket or server process needed.
- Footswitch GPIO is behind a `FootswitchInputBackend` interface; a
  `MockFootswitchBackend` lets tests synthesize raw pin-level (bouncy)
  events to exercise the debounce logic deterministically, with a real
  `gpiozero`/`RPi.GPIO`-backed implementation swapped in only on the actual
  Pi.
- The audio engine sits behind an `AudioEngineClient` interface; the daemon
  defaults to `NullAudioEngineClient` (every call just logged) so
  preset-selection logic is fully testable without any audio hardware, and
  `UnixSocketAudioEngineClient` drives a real `audio-engine` process the
  same way once one is running — see "Audio engine wiring" in
  `control-daemon/README.md`.
- The audio engine itself follows the same pattern one level down:
  `IAudioIoBackend`/`PortAudioBackend` gate the real-time device, and
  `INamModel`/`IHostedPlugin` gate NAM inference and VST3 hosting, each
  off by default behind its own build flag with the control-socket and DSP
  logic fully testable without it. The footswitch relay isn't built yet;
  it should follow the same "narrow interface + software fake" pattern
  once real GPIO hardware exists to build it against.

## Open questions

Tracked in [`docs/open-questions.md`](./docs/open-questions.md) as they get
resolved (Pi 4 vs 5, USB audio interface choice, crossfade/preloading
strategy for glitch-free preset switching, whether the onboard display ships
in V1, power-cut resilience for a pedal that gets switched off like a
stompbox rather than shut down).
