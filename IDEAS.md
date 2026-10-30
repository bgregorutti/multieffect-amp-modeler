# Ideas from Darkglass Electronics open source

Survey of the public repos at <https://github.com/Darkglass-Electronics>
(October 2026). Darkglass's Anagram pedal runs Linux PREEMPT_RT, JACK2,
`mod-host` (LV2 host), `mod-connector` (state/websocket), `event-bridge`
(hardware input) and an LVGL UI. That is close to our own split
(engine / daemon / footswitches / display), so many of their choices map
directly onto ours.

**Licenses.** These are ideas, not code. `mod-host` is GPL-3.0 and
`mod-connector`, `event-bridge`, `libfios` are AGPL-3.0: read for design
only, do not copy. `FFTConvolver`, `NeuralAudio`, `NeuralAmpModelerCore`
and `nam-binary-loader` are MIT and could be vendored as dependencies
(keep their license notice). Check `COPYING.txt` of the FFTConvolver fork
before vendoring.

Priority: **P1** = clear win for current pain points, **P2** = next
version, **P3** = nice to have.

## P1: audio engine performance

1. **Replace the naive convolution with a partitioned FFT convolver.**
   Our `ConvolutionEngine` is O(n·m) and needs the 8192-sample IR cap
   (`audio-engine/README.md`, "Real-time-safe IR length cap"). Darkglass
   ships `FFTConvolver` (MIT, no dependencies, built-in FFT, optional
   SSE; their branch adds `TwoStageFFTConvolver` with non-uniform
   partitions: small head block for low latency, large tail blocks for
   cheap long IRs). Vendoring it would remove the cap and cut cab CPU use
   sharply at block size 64. Keep our resampling and energy
   normalisation in front of it.
   *Source: Darkglass-Electronics/FFTConvolver.*

2. **Benchmark NeuralAudio against NeuralAmpModelerCore.** `NeuralAudio`
   (MIT, by Mike Oliphant, used by Anagram and the Pi Pedal project)
   claims identical output to NAM Core with lower CPU and memory, and
   also runs RTNeural LSTM/GRU models. Add it behind the existing
   `NamModel` interface as a third backend
   (`-DAUDIO_ENGINE_WITH_NEURALAUDIO=ON`), and compare with our
   `benchmodel`-style numbers on the Pi. Its "quality" scale and
   fixed-frame WaveNet path are worth testing at block size 64.
   *Source: Darkglass-Electronics/NeuralAudio.*

3. **Real-time thread setup.** Darkglass pins all audio threads to the
   big cores with `SCHED_FIFO` (priority 56) and forbids unpinned RT
   threads; background loading runs non-RT on other cores. We already pin
   the engine to CPUs 1-3, but the PortAudio callback thread has no
   explicit RT priority, and we don't `mlockall` or set flush-to-zero
   (denormals in the IR/delay tails cost CPU on ARM). Suggested:
   `SCHED_FIFO` on the callback thread, model/IR loading on a non-RT
   thread on CPU 0 or 1, FTZ/DAZ set once in the callback, `mlockall` at
   startup, and evaluate the Raspberry Pi PREEMPT_RT kernel in `deploy/`.
   *Source: Plugin-Dev-Setup (MULTI-THREADING.md, REQUIREMENTS.md).*

## P1: click-free switching (issue #3)

4. **Per-block smooth bypass instead of rebuilding blocks.** Anagram
   requires every block to expose `enabled` as a smoothed control (not a
   hard switch), and "Scenes" (their presets-within-a-preset) must be
   click-free: a scene only changes enabled flags and cheap parameters.
   This is exactly our rig/preset split. Build every block of a rig once,
   then apply a preset as enable/param changes in place with a short
   (5-20 ms) bypass fade per block. Delay tails and gate state then
   survive preset changes. This closes most of `docs/open-questions.md`
   #5 without needing the full `PresetSwitcher` crossfade, which stays
   for rig changes only.
   *Source: Plugin-Dev-Setup (REQUIREMENTS.md, LV2-FEATURES.md).*

5. **Mark "expensive" parameters.** Darkglass flags parameters such as the
   cab IR file with `pprops:expensive`; they are excluded from scenes.
   Same idea for us: tag block parameters that need a reload (NAM model,
   IR, VST3 state) in the block type registry, so the daemon refuses them
   in a preset override and the app greys them out at preset level. It
   makes the "presets are cheap" rule explicit and enforced.
   *Source: Plugin-Dev-Setup (LV2-FEATURES.md).*

6. **A `reset` trigger per block.** Their `kx:Reset` designation clears a
   block's internal state and skips parameter smoothing (used on
   activation and after a bulk parameter load). We already have
   `snapSmoothers()` in some blocks; generalising it to an `EffectBlock`
   method called on chain build and rig change avoids audible ramps when
   a rig loads.
   *Source: Plugin-Dev-Setup (LV2-FEATURES.md).*

## P2: hardware input (footswitches, V2 pots)

7. **Separate input process with partial-state events.** `event-bridge`
   is a small daemon that turns libinput events (footswitches A/B/C,
   relative encoders, LEDs) into websocket messages, sending only partial
   state updates. Our footswitch reader lives inside the daemon; the
   architecture already allows a `footswitch`-role client, so moving GPIO
   (and later the ADS1115 pot reader) into a tiny client keeps the daemon
   free of hardware libraries and makes a crash in the reader harmless.
   It is also a natural home for footswitch LEDs.
   *Source: Darkglass-Electronics/event-bridge (design only, AGPL).*

8. **Prefer encoders over pots for V2.** Anagram uses relative encoders
   with a "quick pot" concept (`dg:quickPot`: each block designates one
   parameter that the knob under it controls). Encoders avoid the pickup
   problem the spec still lists as open for 4.5, and "quick param per
   block" is a simple mapping for the app to edit.
   *Source: event-bridge, Plugin-Dev-Setup (LV2-FEATURES.md).*

## P2: control and protocol

9. **MIDI control with learn.** `mod-host` exposes `midi_learn`,
   `midi_map`, `midi_unmap` per parameter (pitch bend mapped as CC 131),
   and `anagram-midi-control` maps device functions (preset, scene,
   bypass) to MIDI. For us: a MIDI input in the daemon (ALSA seq, USB MIDI
   footcontrollers) mapping PC to presets/rigs and CC to
   `set_block_param`, plus a "learn" button in the app. Reuses the
   existing footswitch action set.
   *Source: mod-host, anagram-midi-control.*

10. **Expose CPU load and xruns to clients.** `mod-host` has
    `cpu_load`/`max_cpu_load`/`monitor_cpu_load`. We already count
    `xrunCount`/`overBudgetCount` in the engine but only print them to
    stderr. Forward them through the daemon so the app and display can
    show a DSP-load meter, and warn when a rig is too heavy.
    *Source: mod-host.*

11. **Tempo as a single global value pushed to blocks.** Anagram has no
    transport, only a global BPM (40-300) that every time-based block
    receives as a hidden parameter. We already have tap tempo and
    `set_tempo`; the next step is letting the delay block (and future
    tremolo/modulation) declare "follows tempo" with a note division,
    and later MIDI clock or Ableton Link sync (`mod-host` supports both).
    *Source: Plugin-Dev-Setup (TIME.md), mod-host.*

## P3: assets, UI, effects

12. **Compact binary NAM format.** `nam-binary-loader` (MIT) defines
    `.namb`: little-endian float32 weights, CRC32, about 80% smaller than
    `.nam` JSON, loadable from a memory buffer. Converting on upload in
    the daemon (keep the `.nam` as the source) would speed rig changes
    and reduce SD card use.
    *Source: Darkglass-Electronics/nam-binary-loader.*

13. **Block metadata for the UI (issue #7).** Every Anagram block must
    ship a 2-3 letter abbreviation (`dg:abbreviation`), on/off 200x200
    images, units, and parameters grouped by 6 (one page on the device).
    Adding `abbreviation`, `icon_on`/`icon_off`, `unit` and `group` to our
    block type registry would feed both the display's chain strip (step
    2) and the app's component images in one place.
    *Source: Plugin-Dev-Setup (REQUIREMENTS.md, LV2-FEATURES.md).*

14. **Dependent parameters.** Their `control-port-state-update` extension
    lets one parameter disable or hide others (for example "sync" hides
    "time in ms"). A simple `enabled_if` rule in our parameter schema
    would let the app hide irrelevant sliders.
    *Source: Darkglass-Electronics/LV2-Extensions.*

15. **Stereo and mono-to-stereo rule.** Anagram runs two identical mono
    instances after a stereo block and requires mono-to-stereo effects to
    be 2-in/2-out internally. Worth adopting as a rule now if a stereo
    output (for example stereo delay or reverb) is ever planned.
    *Source: Plugin-Dev-Setup (REQUIREMENTS.md).*

16. **Modulation effects to add next.** `dark-plugins`
    (phaser, tremolo, ported from Calf and gen~), `fil4.lv2` (x42 4-band
    parametric EQ, GPL-2.0) and `dm-Vibrato` (Rust, GPL-3.0) show which
    modulation effects they ship. Their licenses are GPL, so use them as
    references for algorithm choice and port our own via `vst-python/`
    like the existing pedals.
    *Source: dark-plugins, fil4.lv2, dm-Vibrato.*

## Not worth adopting

- **Switching to LV2 + JACK + mod-host.** It would bring a large plugin
  ecosystem but replace our tested engine and VST3 host, and add JACK
  latency/complexity on a Pi. Keep our engine; borrow the ideas above.
- **LVGL for the display.** Already decided against in ARCHITECTURE.md
  (framebuffer + Pillow is lighter). Darkglass uses LVGL because its UI is
  far richer and runs on 4 dedicated cores.
- **Radxa kernel / U-Boot trees.** Hardware-specific to their SoC.
