# scripts

Dev-machine testing tools that don't belong to any one component -- for
proving the software works *before* a Raspberry Pi, real footswitch, or
finished mobile-app file-picker exists. See the root `README.md`'s
"Status" section and each component's own README for what's real vs.
stubbed.

- `load_test_preset.py` -- builds a preset (optionally referencing an
  uploaded `.nam`/IR asset -- real WaveNet/LSTM amp modeling if the engine
  was built with `AUDIO_ENGINE_WITH_REAL_NAM`, see below) against a
  running control-daemon, without the mobile app. Stands in for the app's
  "create preset" + "upload asset" screens, whose editing logic is real
  but whose OS file-picker is currently stubbed.
- `keyboard_footswitch.py` -- stands in for a physical footswitch relay,
  mirroring the planned four-switch pedal layout: up/down arrow = next/prev
  **rig** (swaps the whole backline -- amp + cab -- the expensive,
  between-songs path), right/left arrow = next/prev **preset** (flips
  effects under an unchanged amp/cab, staying within the current rig), `b`
  = toggle bypass.
- `init-presets.sh` -- a personal convenience script (hardcoded local
  `.nam`/IR file paths) that calls `load_test_preset.py` repeatedly to
  build out a couple of real rigs (amp + cab as separate `--rig`-scoped
  calls, so they accumulate into one backline each rather than becoming
  unrelated presets). Not portable as-is -- copy and edit the paths for
  your own asset library rather than running it directly.
- `upload_assets.py` -- bulk-uploads/registers `.nam`/IR `.wav`/`.vst3`
  files (or a whole folder of them) against a running control-daemon, then
  stops -- no preset created, just makes the assets available to the app's
  picker. Also the documented way to load a pack onto a real Pi over SSH
  once the mobile app's file-picker is stubbed: see
  `deploy/README.md`'s "Loading NAM/IR/VST3 asset packs".

## Full walkthrough: guitar in, hear the effects chain, before any hardware

Three processes, three terminals, all on this Mac, using your external
audio interface set as the system default in Audio MIDI Setup:

**Terminal 1 -- the audio engine, built with real device I/O:**
```bash
brew install portaudio pkg-config    # one-time
cmake -S audio-engine -B audio-engine/build-audio -DAUDIO_ENGINE_WITH_PORTAUDIO=ON
cmake --build audio-engine/build-audio -j
./audio-engine/build-audio/audio_engine /tmp/audio_engine.sock --audio
```
First run only: this opens your input device, which can trigger (or, if
run non-interactively, silently hang on) a one-time macOS microphone
permission prompt -- run this from an actual terminal window and approve
it. See audio-engine/README.md "Real-time audio I/O".

Add `-DAUDIO_ENGINE_WITH_REAL_NAM=ON` to the `cmake -S` line above if you
want real amp modeling (not just cab-IR convolution) for `--nam` -- see
"Using your own .nam/IR files" below and audio-engine/README.md "Real NAM
inference". Skip it for now if you just want to hear the built-in
gain/delay/EQ blocks.

Watch terminal 1's startup output for the negotiated round-trip latency
(`audio-engine: negotiated latency: input ... ms, output ... ms`) -- pass
`--block-size N` (default 64, ~1.33ms) to trade off latency against xrun
safety margin; see audio-engine/README.md "Latency" for the numbers behind
that default and when to raise/lower it.

If your interface has a **direct monitor** knob/switch, turn it off (or
all the way to "playback") before testing -- otherwise you're hearing your
dry signal summed straight from the hardware *underneath* whatever the
engine outputs, which comb-filters into a "not quite flat" coloration no
matter what the software does (this looks exactly like a bypass bug the
first time you hit it; it isn't one -- see audio-engine/README.md "Real-time
audio I/O").

**Terminal 2 -- the control daemon, wired to that engine:**
```bash
cd control-daemon
CONTROL_DAEMON_AUDIO_ENGINE_SOCKET=/tmp/audio_engine.sock .venv/bin/control-daemon
```

**Terminal 3 -- load a test preset, then become the footswitch:**
```bash
control-daemon/.venv/bin/python3 scripts/load_test_preset.py --name "Slap Delay"
# optionally build a second one to switch between:
control-daemon/.venv/bin/python3 scripts/load_test_preset.py --name "Clean" --gain-db 0 --no-delay --no-select

control-daemon/.venv/bin/python3 scripts/keyboard_footswitch.py
```

At this point: plug your guitar into the interface's input, monitor its
output, and you should hear +6dB gain and a slapback delay on your dry
signal (both real DSP -- see audio-engine/README.md's module map). Press
the right/left arrow keys in terminal 3 to switch to the other preset and
back; watch terminal 3's own output and/or terminal 2's log for the
`footswitch_next_preset`/`footswitch_prev_preset` state changes.

### Using your own `.nam`/IR files (e.g. a NAM/IR pack you already own)

```bash
control-daemon/.venv/bin/python3 scripts/load_test_preset.py \
  --name "Ampeg cab" \
  --ir "$HOME/Documents/Musique/VST-NAM/Ampeg SVT - DI - 4x10 - 8x10 - the definitive speaker capture collection/Ampeg 8x10 57 A107.wav"
```
Quote each path (these packs routinely have spaces/dashes in folder and
file names) -- `$HOME/...` rather than `~/...` inside the quotes, since
`~` isn't expanded inside double quotes by the shell.

**`--nam` now does real WaveNet/LSTM amp modeling** -- but only if the
`audio_engine` process you're talking to was built with
`-DAUDIO_ENGINE_WITH_REAL_NAM=ON` (see audio-engine/README.md "Real NAM
inference"):

```bash
cmake -S audio-engine -B audio-engine/build-nam \
  -DAUDIO_ENGINE_WITH_PORTAUDIO=ON -DAUDIO_ENGINE_WITH_REAL_NAM=ON
cmake --build audio-engine/build-nam -j
./audio-engine/build-nam/audio_engine /tmp/audio_engine.sock --audio

control-daemon/.venv/bin/python3 scripts/load_test_preset.py \
  --name "Real amp" --gain-db 0 --no-delay \
  --nam "$HOME/Documents/Musique/VST-NAM/AMPEG SVT CL (GAIN STAGES)/AmpegSVT - B7K.nam"
```

Without that flag, `--nam` still uploads/registers/loads successfully
(this doesn't require the flag), but the engine falls back to
`StubNamModel` -- a fixed identity pass-through, no real amp tone.
`load_test_preset.py` can't tell which build it's talking to, so it can't
warn you if you forgot the flag; if a `.nam` file "loads fine" but sounds
completely dry, check terminal 1 was built with `AUDIO_ENGINE_WITH_REAL_NAM`.

Multi-gain-stage exports (`"SlimmableContainer"` architecture -- both real
files used during development, from two different commercial packs, used
this) are fully supported; you do not need to hunt for a "simpler" `.nam`
file first.

A cabinet IR is still separately real convolution and will audibly change
the tone regardless of the `--nam` flag situation -- the Ampeg IR above is
44.1kHz, and the engine resamples it to its own 48kHz internal rate at
load time (see audio-engine/README.md "Sample rate policy"), so it plays
back at the correct pitch/timing rather than time-compressed.

**Jerky/glitchy/choppy audio through `--audio`?** This was a real bug,
not a hypothetical: a long cabinet IR (this Ampeg one included -- it's an
unusually long "room capture" style IR, ~720ms natively) can't be
convolved by the naive engine within one real-time block's budget, so
every block underruns. `loadImpulseResponseFile` now caps IR length to a
measured-safe 8192 samples (~171ms, with a short fade-out so the cut is
inaudible) -- see audio-engine/README.md "Real-time-safe IR length cap"
for the exact before/after numbers. If you still hear glitching after
pulling the latest code, rebuild `build-audio` (a stale binary won't have
the fix) and check you're not also loading a second long asset.

**Sound is loud/distorted/"horrible" (not jerky -- that's the bug
above)?** A separate real bug: cabinet IR files aren't gain-consistent,
and convolving with an ungained one can multiply a normal playing-level
signal well past 0dBFS -- measured 3.6x over range on this Ampeg IR at a
moderate playing level, before the fix. `loadImpulseResponseFile` now
energy-normalizes every loaded IR, and the full chain's output is also
defensively clamped to `[-1, 1]` -- see audio-engine/README.md "IR gain
normalization". Rebuild `build-audio` to pick this up too.

### Resetting state between test runs

control-daemon persists everything (presets, banks, footswitch mapping,
asset metadata) to a JSON file and reloads it on every startup -- that's
the whole point of persistence, so restarting the daemon process alone
will *not* clear anything you've built up across test sessions with
`load_test_preset.py`.

There's also no WS command to wipe state wholesale (only `delete_preset`
exists for individual presets; there's no `delete_bank` at all yet) --
deliberately: a "wipe everything" remote command is a real production
feature decision for the actual pedal, not something to bolt on as a side
effect of a test-tooling convenience. For a clean slate while testing,
stop the daemon and delete its store instead:

```bash
# with the daemon stopped
rm -rf control-daemon/data/state.json control-daemon/data/assets
```
(or whatever path `CONTROL_DAEMON_STORE_PATH` pointed at, if you set one).
The next startup begins from empty state. Alternatively, point
`CONTROL_DAEMON_STORE_PATH` at a fresh path per test session (this is what
the diagnostic sessions in this repo's own development used) to keep
old runs around instead of deleting them.

**No sound at all?** Check, in order:
- the interface is the OS default input *and* output;
- terminal 1 (the engine) printed "streaming default audio device" (not
  just "listening on ..." -- see the mic-permission note above);
- terminal 2 (the daemon) has no `audio-engine socket error ... No such
  file or directory` in its log -- that means terminal 1 either isn't
  running yet or is using a different socket path; start terminal 1
  *before* terminal 3's commands, or just rerun terminal 3's last command
  once terminal 1 is up (the daemon doesn't retry a failed engine call on
  its own);
- `load_test_preset.py`/`keyboard_footswitch.py` fail outright with
  something like `Connect call failed ('127.0.0.1', 8765)` -- that's
  terminal 2 (the daemon) not running at all, not an engine problem; start
  it first;
- a preset is actually selected (`load_test_preset.py`'s last line said
  "selected ... as the active preset", or press an arrow key in terminal 3);
- bypass is off (a fresh daemon starts with `bypass: false`, but check
  terminal 2's log if you've been experimenting).
