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

**Status: open.** Belongs to the audio engine. Candidates per the spec:
software crossfade between old/new chains, or background preloading of the
next preset while the current one plays. The control daemon's
`AudioEngineClient` interface (`load_preset`, etc.) is written so either
strategy can be implemented behind it without changing the daemon.

Where it stands: `PresetSwitcher` (equal-power crossfade between two
already-prepared chains) is built and tested but not wired into the
real-time path. More fundamentally, the rig/preset split (see
`ARCHITECTURE.md`) was introduced precisely so that switching presets
*within* a rig needs no loading at all -- the amp, cab and every effect are
the same blocks, only their enable flags and params differ -- but the engine
doesn't exploit it yet: `ResourceManager::loadPreset` rebuilds the whole
chain on every `load_preset`, re-reading the `.nam` and IR from disk, while
holding the mutex the audio callback needs. So the likely shape of the answer
is two-tier: in-rig preset changes applied in place to a chain built once per
rig (no load, nothing to crossfade beyond per-param smoothing), and rig changes
built off the audio thread then crossfaded with `PresetSwitcher`.

## 6. Power-cut resilience (the Pi will be switched on/off like a stompbox, not gracefully shut down)

**Status: open.** Confirmed design constraint: the pedal's power switch cuts
power to the Pi directly (no GPIO/soft-shutdown signal beforehand), the
same way unplugging a wall-wart or a real stompbox's footswitch does.
Autostart itself is already solved — both systemd units are `enable`d with
`Restart=on-failure` (see `deploy/install.sh`, confirmed by the "Reboot
test" step in `deploy/README.md`) — but this raises two problems autostart
doesn't touch:

- **Boot time.** A full Linux boot (kernel, systemd, NetworkManager AP
  bring-up, then the daemon/engine) is realistically 15-30+ seconds, not a
  stompbox's instant-on. Needs `systemd-analyze blame`/`critical-chain` on
  real hardware to find what's slow, and a "booting, not ready yet" state
  on whatever the onboard display/LED ends up being (see open question #4)
  rather than silence during that window.
- **SD card / OS corruption from repeated abrupt power loss.** `state.json`
  itself is already protected — `persistence.py` writes to a temp file,
  `fsync`s it, then does an atomic `os.replace`, so a cut mid-save loses at
  most the last unsaved edit, never a corrupt file. The OS partition as a
  whole is not protected the same way: enough abrupt cuts during a
  systemd/journald/apt write can leave Raspberry Pi OS's root filesystem
  unbootable.

  The standard fix is Raspberry Pi OS's **read-only-root overlay**
  (`raspi-config` → Overlay Filesystem / `raspi-config nonint do_overlayfs`)
  so the root filesystem can't be corrupted by a power cut at all. The
  catch: that overlay's writable layer is tmpfs and discarded on reboot, so
  `/var/lib/multieffect-amp-modeler` (`DATA_DIR` — rigs/presets/assets)
  would need to live on its own real, separately-mounted partition
  *excluded* from the overlay, or every saved preset would vanish on the
  next boot. Not yet automated in `install.sh` or verified on real
  hardware -- doing so needs a concrete SD card partition layout decision
  first, and belongs alongside this repo's existing "not tested on real
  Pi hardware" caveat for `deploy/`.
