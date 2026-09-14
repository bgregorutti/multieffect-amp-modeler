# scripts

Dev-machine testing tools that don't belong to any one component -- for
proving the software works *before* a Raspberry Pi, real footswitch, or
finished mobile-app file-picker exists. See the root `README.md`'s
"Status" section and each component's own README for what's real vs.
stubbed.

- `load_test_preset.py` -- builds a preset (optionally referencing an
  uploaded `.nam`/IR asset) against a running control-daemon, without the
  mobile app. Stands in for the app's "create preset" + "upload asset"
  screens, whose editing logic is real but whose OS file-picker is
  currently stubbed.
- `keyboard_footswitch.py` -- stands in for a physical footswitch relay.
  Right arrow = next preset, left arrow = previous preset.

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

**`--nam` compatibility is narrower than `--ir`.** The metadata parser
(`nam_model.hpp`) expects the single-model NeuralAmpModelerCore export
shape: a flat, non-empty top-level `weights` array. Files exported by some
commercial plugins (verified against a real Darkglass B7K Ultra `.nam`
export) instead use a `"SlimmableContainer"` architecture whose top-level
`weights` is `[]` -- the real weight data lives nested under
`config.submodels[...]`, a different shape the parser doesn't understand
yet. That file gets rejected with `missing or invalid required field
'weights'`, and the whole `load_preset` fails as a result (so the preset
stays created in the daemon, just not loaded into the engine) -- test
`--nam` against a single-model `.nam` export if you have one; otherwise
leave `--nam` off for now (NAM inference is stubbed to identity
pass-through regardless, so a rejected upload costs you nothing audible
today) and use `--ir` alone, which is real convolution and does not have
this limitation.

**What you will *not* hear yet, even with a `.nam` file that does parse:**
actual neural amp modeling -- inference is stubbed to a fixed identity
pass-through no matter the architecture (see audio-engine/README.md "NAM
inference stubbed"). A cabinet IR *is* real convolution and will audibly
change the tone -- the Ampeg IR above is 44.1kHz, and the engine now
resamples it to its own 48kHz internal rate at load time (see
audio-engine/README.md "Sample rate policy"), so it plays back at the
correct pitch/timing rather than time-compressed.

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
