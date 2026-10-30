# Architecture

## Overview

```
┌─────────────────┐        WebSocket         ┌──────────────────────────┐
│   Mobile app      │ ◄──────────────────────► │   Control daemon           │
│  (Flutter)         │                          │  (preset state,            │
└─────────────────┘                          │   source of truth)         │
                                              └──────────┬────────────────┘
┌─────────────────┐         GPIO                         │  Unix socket
│  4 footswitches    │ ─────────────────────────────────────┤
│  (+ V2: 2 pots via │                                       │
│   ADC)             │                                       ▼
└─────────────────┘                              ┌──────────────────────────┐
                                                  │   Audio engine              │
┌─────────────────┐      WebSocket (read-only)   │   (real-time plugin host)  │
│  Onboard TFT       │ ◄──────────────────────── └──────────────────────────┘
│  display client    │         (via daemon)
└─────────────────┘
```

The **control daemon** is the single source of truth for "which rig/preset
is active" and for all rig/preset/footswitch-mapping configuration. It
notifies every connected client (mobile app, onboard display, any
footswitch relay) on every state change, so all interfaces stay consistent
-- including changes triggered by a footswitch. This is why it was built
and tested first: every other component is a client of its API, so the API
contract is the thing worth stabilizing early. The display talks to the
daemon only, never to the engine.

## Components

| Component | Language/Stack | Status |
|---|---|---|
| Control daemon | Python, FastAPI, WebSockets | V1 built and tested (`control-daemon/`) |
| Audio engine | C++20, PortAudio (JUCE deferred, see audio-engine/README.md) | V1 built and tested (`audio-engine/`) |
| Pedal DSP reference | Python (`vst-python/`) | Reference implementation the C++ pedal blocks are ported from and validated against -- not what runs on the Pi |
| Mobile app | Flutter | V1 built and tested (`mobile-app/`) |
| Onboard display client | Python, Pillow, direct framebuffer (no X) | V1 step 1 (text layout) built and tested on a dev machine (`display-client/`); not yet run on the real panel |
| Footswitches (4, GPIO) | Python, in the daemon (`gpio.py`, `debounce.py`) | GPIO backend + software debounce built and tested against a mock; **not yet started by the daemon** on a Pi |
| Potentiometers (V2) | External I2C ADC on software I2C | Not started (V2) |
| Power management / boot | systemd + soft-latch power board | Service readiness, ordering and boot/shutdown screens done; power board and its overlays not yet chosen/wired |
| Deployment | Bash + systemd + NetworkManager | Script + guide written (`deploy/`); not yet run on real hardware |

See the root [`README.md`](./README.md) "Status" section for current test
counts and per-component detail; this table is deliberately just a
one-line-per-component snapshot, not the source of truth for numbers.

## Spec coverage: what's done and what's ongoing

The spec sections not listed here (1-3, 4.1, 4.2, 4.6, 6-8) still match
what is implemented. The rest of this document and the component READMEs
describe them. The spec revision that reworked 4.3, 4.4, 4.5 and 5 stands
as follows.

### 4.3 Footswitches

Done:
- Four logical actions, matching the spec's V1 set: `next_rig`/`prev_rig`
  and `next_preset`/`prev_preset`. Preset stepping never crosses a rig
  boundary. The mapping lives in daemon state and is edited from the app
  only.
- Software debounce on raw pin levels (`debounce.py`), with a GPIO
  abstraction (`gpio.py`): `GpioZeroFootswitchBackend` uses internal
  pull-ups and active-low SPST wiring, exactly as the spec's wiring
  section describes. It is tested against `MockFootswitchBackend`.
- No configuration logic onboard: `footswitch`-role clients may only send
  `footswitch_press`.

Ongoing:
- The daemon does not yet *start* a `FootswitchInputController`. `main.py`
  needs a config entry mapping switch indices to **GPIO 5, 6, 12, 13**
  (physical pins 29, 31, 32, 33) and a default mapping for them. Until
  then `scripts/keyboard_footswitch.py` stands in.
- Optional per-switch status LEDs, bypass and tap-tempo switches: not
  started. `toggle_bypass`/`tap_tempo` already exist as mappable actions.
- GPIO budget: no code claims pins outside 27-40, but nothing enforces it
  either. GPIO 26/16 (power) and 19/20/21 (V2) are reserved by
  documentation only.

### 4.4 Onboard display

Done (`display-client/`, see its README):
- A lightweight read-only client: a `role: "display"` WebSocket connection
  that never sends anything after `hello`. Rendering uses Pillow straight
  into `/dev/fbN`, with no X server, SDL, LVGL build or kiosk browser. This
  settles the spec's LVGL-vs-pygame/Kivy question (open question 6) in
  favour of neither: plain framebuffer writes are lighter than both and
  work identically on the fbtft driver (vendor `waveshare35c` overlay) and
  DRM tiny drivers.
- Content step 1: large preset number and name, rig position and name,
  connection state, bypass, engine error, plus a signal-chain strip
  showing which blocks the active preset enables.
- Content step 2 (first cut): the strip draws component images (rendered
  from `control-daemon/data/img/*.svg`), washed out when a block is off,
  with a text chip for types that have none. Input gain and volume are
  left out of the strip and shown as dB values instead, under the amp and
  cab names.
- Resolution comes from configuration (`--lcd-size`, or the framebuffer's
  own size). The same code renders at 480x320 and 320x240, and both are
  covered by tests.
- CPU isolation: the client is pinned to CPU 0 at `Nice=10`, and the audio
  engine is pinned to CPUs 1-3.
- Driver install for the Waveshare 3.5" (C): `install.sh --lcd waveshare35c`
  installs only the vendor overlay plus `dtparam=spi=on`. It skips the
  vendor's X/LightDM stack.
- Redraws only on state change, and writes only the changed framebuffer
  rows, which keeps SPI traffic and CPU near zero when idle.

Ongoing:
- Not yet run on the physical panel. Two things need confirming there:
  that the overlay works with direct framebuffer rendering on the current
  64-bit kernel (spec open question 7), and the colour order/rotation. The
  README gives the in-tree `piscreen,drm` fallback, untested.
- Content step 2, rest: images per asset rather than per block type,
  served by a daemon component catalog (issue #7).
- Final panel choice (2.8" vs 3.5") is still open in the spec. Nothing in
  the code depends on it.
- Showing V2 pickup state (pot value vs stored value): waits for 4.5.

### 4.5 Potentiometers (V2)

Not started, by design: V2. Nothing implemented so far blocks it. The live
`set_block_param` path a pot reader would drive already exists, and gain
and volume are the `input_trim`/`output_volume` blocks of every rig's
default chain. Still open: pots with pickup mode vs rotary encoders, and
the reader process (software I2C on GPIO 20/21 with an ADS1115) with its
filtering and hysteresis.

### 5. Power management and boot behaviour

Done:
- All three services start at boot under systemd. The display client starts
  first, and is ordered `Before=` the engine and daemon, so it also stops
  last.
- The engine and daemon are `Type=notify`. They report `READY=1` once
  actually usable (engine: control socket listening and audio stream open;
  daemon: WS port bound) and `STOPPING=1` on stop. Both use a few lines of
  `sd_notify` with no libsystemd dependency.
- The boot progress bar follows those real readiness states (spec section
  5.3/5.4). The display switches to a "shutting down" screen as soon as
  the daemon drops while systemd reports `stopping`, and leaves it on the
  panel.
- The daemon persists state atomically on every change (`persistence.py`),
  so a clean stop has nothing left to flush.

Ongoing:
- Soft-latch power board: model not chosen (spec open question 8). Once it
  is, add `dtoverlay=gpio-shutdown,gpio_pin=26` and
  `dtoverlay=gpio-poweroff,gpiopin=16` with the board's polarity. Nothing
  in software changes. The older "button on GPIO3" option in
  `deploy/README.md` no longer applies, because GPIO3 is under the display.
- Engine fade-out/mute before stopping, to avoid a pop at shutdown: not
  implemented. The engine currently stops the stream directly.
- Boot time: not yet measured on hardware (`systemd-analyze`). No
  services disabled yet (Bluetooth, wait-online).
- Behaviour of the power button pressed during boot or shutdown (spec open
  question 9), and an optional read-only root filesystem (open question
  10).

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
  In the engine: when a new chain uses the same `.nam` and IR as the one
  playing, `ResourceManager::buildChain` keeps that running amp and cab
  instead of reading them again, and any loading happens without holding
  the lock the audio callback needs. Effect blocks are still rebuilt on
  each preset switch (cheap, no disk, but their state such as a delay tail
  resets), VST3 blocks still reload, and there's no crossfade yet — see
  `docs/open-questions.md` #5.
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
  `control-daemon/src/control_daemon/debounce.py`. Pins: GPIO 5, 6, 12,
  13 (physical 29, 31, 32, 33) with a shared ground. The display covers
  header pins 1-26, so only 27-40 are usable (spec section 4.3).

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

- **The onboard display is a framebuffer client, not a GUI toolkit or a
  browser.** One SPI TFT (Waveshare 3.5" (C), 480x320, likely) replaces
  the earlier I2C LCD/OLED plan, and also the idea of running the Flutter
  web build in a kiosk browser (`docs/open-questions.md` #4). The spec
  rules that out as too CPU-hungry next to the engine. The client draws
  with Pillow into `/dev/fbN`, has two dependencies, and is entirely
  event-driven after boot. Rendering to the framebuffer rather than through
  a display server means the same code works on the vendor's fbtft overlay
  and on a DRM tiny driver, and on either panel size. See
  `display-client/README.md`.

- **Boot progress comes from systemd readiness, not from timers or log
  scraping.** The engine and daemon are `Type=notify` and send `READY=1`
  only once usable. The display reads their `ActiveState` during boot and
  then stops polling. The display's own unit is ordered `Before=` both, so
  it starts first and stops last, which is what lets it show both the boot
  bar and the shutdown screen. CPU isolation is set in the units too: the
  engine is on CPUs 1-3, and the display and everything else on CPU 0.

- **Deployment: systemd services + NetworkManager AP, not a custom init
  system or hand-rolled hostapd/dnsmasq config.** `control-daemon`,
  `audio-engine` and `display-client` run as systemd services under a dedicated unprivileged
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
- **Display client**: Pillow + websockets only, no GUI toolkit or browser.
  It is idle (no polling, no redraws) unless the daemon broadcasts a
  change, and pinned off the engine's cores.
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
  logic fully testable without it.
- The display client renders to a `PngSink` or an in-memory sink instead
  of `/dev/fbN`. Its framebuffer writer is tested against a fake sysfs and
  device file, and the client end to end against a stand-in daemon
  WebSocket server. systemd is behind a probe that degrades to "unknown"
  when absent.

## Open questions

Tracked in [`docs/open-questions.md`](./docs/open-questions.md) as they get
resolved (Pi 4 vs 5, USB audio interface choice, crossfade/preloading
strategy for glitch-free preset switching, the onboard display, power
management and SD-card resilience). The spec keeps its own list in section
8. The ones the spec revision added and that remain open are the
soft-power board model (8), the button's behaviour during boot/shutdown
(9), a read-only root filesystem (10) and the boot-time target (11).
