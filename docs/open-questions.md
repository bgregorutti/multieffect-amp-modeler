# Open questions

Tracked from the original product spec, section 7. Updated as decisions are
made during development.

## 1. Pi 4 vs Pi 5

**Status: open.** Depends on latency benchmarks with the target effects
chain, once the audio engine exists. Needs a real NAM model + IR + a
representative effects chain running under a PREEMPT_RT kernel (or Elk Audio
OS) with buffer sizes swept down until xruns appear, on both boards.

## 2. Final choice of USB audio interface

**Status: open.** Requirement: class-compliant USB Audio 2.0, no proprietary
driver, mounted inside the enclosure with only the instrument input and one
output channel routed to the panel.

## 3. Preset serialization format

**Status: resolved — JSON.** Versioned schema, atomic writes. See
`ARCHITECTURE.md` and `control-daemon/src/control_daemon/models.py` /
`persistence.py`.

## 4. Whether the onboard display is needed in V1

**Status: open.** The control daemon already broadcasts state to any
connected client, so a display client (I2C LCD/OLED) is a thin consumer of
the existing WebSocket API whenever it's built — deferring it costs nothing
architecturally.

## 5. Exact crossfade/preloading strategy for glitch-free preset switching

**Status: open.** Belongs to the audio engine (not yet started). Candidates
per the spec: software crossfade between old/new chains, or background
preloading of the next preset while the current one plays. The control
daemon's `AudioEngineClient` interface (`load_preset`, etc.) is written so
either strategy can be implemented behind it without changing the daemon.
