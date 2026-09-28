# Open questions

Tracked from the original product spec, section 7. Updated as decisions are
made during development.

## 1. Pi 4 vs Pi 5

**Status: open.** Depends on latency benchmarks with the target effects
chain, once the audio engine exists. Needs a real NAM model + IR + a
representative effects chain running under a PREEMPT_RT kernel (or Elk Audio
OS) with buffer sizes swept down until xruns appear, on both boards.

Power/enclosure input to this decision: the Pi 5 has an onboard power
button that already does clean shutdown and wake, and `POWER_OFF_ON_HALT=1`
makes it cut its own rails on halt. Bringing that button out to the
enclosure is much less work than the equivalent on a Pi 4 (a
`gpio-poweroff` GPIO plus an external latching circuit), and it leaves GPIO3
free for an I2C display -- see "Wiring an on/off switch" in
`deploy/README.md` and question #4 below.

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

One hardware decision here does *not* defer cheaply, though: **I2C vs SPI**.
An I2C display needs GPIO2/GPIO3 (pins 3 and 5), and GPIO3 is also the only
pin that wakes a halted Pi 4 — the pin `dtoverlay=gpio-shutdown` defaults to
for a press-to-shutdown/press-to-boot power button (question #6, and
"Wiring an on/off switch" in `deploy/README.md`). Picking an I2C display
means either giving up press-to-boot or moving to a Pi 5 whose own power
button leaves GPIO3 free. An SPI display avoids the clash entirely. Worth
settling before buying the panel, since the cheap fix is gone afterwards.

## 5. Exact crossfade/preloading strategy for glitch-free preset switching

**Status: open.** Belongs to the audio engine. Candidates per the spec:
software crossfade between old/new chains, or background preloading of the
next preset while the current one plays. The control daemon's
`AudioEngineClient` interface (`load_preset`, etc.) is written so either
strategy can be implemented behind it without changing the daemon.

Where it stands:

- **Done:** the amp and cab are loaded on a rig change and kept running
  across preset changes within the rig (`ResourceManager::buildChain` reuses
  them when the asset is unchanged -- no disk read, no re-prepare), and
  loading no longer holds the mutex the audio callback needs, so the audio
  keeps playing the previous chain until the new one is swapped in.
- **Open:** effect blocks are still rebuilt on every preset switch, so their
  state resets (a delay tail is cut, a gate restarts) and toggling one is
  an instant on/off rather than a short fade; `"vst3"` blocks reload.
  Building every block of a rig once and applying a preset as enable/param
  changes in place (with a short per-block bypass fade) would close that.
- **Open:** `PresetSwitcher` (equal-power crossfade between two
  already-prepared chains) is built and tested but not wired in, so a rig
  change still switches hard at the swap.

## 6. Power-cut resilience (the Pi will be switched on/off like a stompbox, not gracefully shut down)

**Status: open, but the constraint has softened.** This was originally
recorded as a confirmed design constraint -- the pedal's power switch cuts
power to the Pi directly, no GPIO/soft-shutdown signal, the same way
unplugging a wall-wart does. That is no longer settled: a momentary button
on GPIO3 plus `dtoverlay=gpio-shutdown` gives a clean shutdown *and*
press-to-boot from one button, which removes most of the corruption risk
below for one line of `config.txt`. See "Wiring an on/off switch" in
`deploy/README.md` for the three options, their standby-draw trade-offs,
and the GPIO3-vs-I2C conflict that interacts with open question #4.

Interim working practice: `sudo poweroff` before pulling the wire.
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

  Note this risk is largely *avoided*, not merely mitigated, if the switch
  ends up being a soft-shutdown button rather than a bare power cut -- the
  overlay then guards against accidents (yanked cable, blown supply) rather
  than against routine use.

  The standard fix is a **read-only-root overlay**. On Raspberry Pi OS
  that's built in (`raspi-config` → Overlay Filesystem /
  `raspi-config nonint do_overlayfs`); on the plain Debian at least one of
  these Pis actually runs there is no `raspi-config`, so it's the
  `overlayroot` package or a hand-written initramfs hook
  so the root filesystem can't be corrupted by a power cut at all. The
  catch: that overlay's writable layer is tmpfs and discarded on reboot, so
  `/var/lib/multieffect-amp-modeler` (`DATA_DIR` — rigs/presets/assets)
  would need to live on its own real, separately-mounted partition
  *excluded* from the overlay, or every saved preset would vanish on the
  next boot. Not yet automated in `install.sh` or verified on real
  hardware -- doing so needs a concrete SD card partition layout decision
  first, and belongs alongside this repo's existing "not tested on real
  Pi hardware" caveat for `deploy/`.
