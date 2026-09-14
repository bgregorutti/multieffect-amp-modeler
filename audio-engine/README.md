# audio-engine

The real-time audio engine for the DIY AI guitar multi-effects pedal. It
loads a NAM (Neural Amp Modeler) model + a cabinet impulse response (IR) +
an effects chain per preset and processes the guitar signal. The
(already-built) `control-daemon/` is the single source of truth for which
preset is active and drives this engine over a local control socket (see
"Control socket protocol" below) -- `control-daemon`'s
`UnixSocketAudioEngineClient` (see `control-daemon/README.md` "Audio engine
wiring") is the client side of that connection.

```
Mobile app / footswitch <--WebSocket/GPIO--> control-daemon <--Unix socket--> audio-engine (this package)
```

This package is self-contained and buildable/testable on a normal Linux
dev machine -- no Raspberry Pi, no real audio interface, no JUCE, and no
GitHub-fetched C++ dependency required. See "Deviations from the plan"
below for why, and what that costs.

## Building and testing

```bash
cmake -S audio-engine -B audio-engine/build
cmake --build audio-engine/build -j
ctest --test-dir audio-engine/build            # or: ./audio-engine/build/audio_engine_tests
```

Requires: CMake >= 3.16, a C++20 compiler, and two packages
(`nlohmann-json`, `gtest`+`gmock`) -- from the normal Ubuntu package mirror
(`nlohmann-json3-dev`, `libgtest-dev`+`libgmock-dev`) or Homebrew
(`nlohmann-json`, `googletest`) on macOS, not from GitHub. Last run: **72/72
tests passed** (`ctest --test-dir audio-engine/build`), covering every
module below plus a real-subprocess control-socket integration test.

Run the engine standalone (mostly useful for manual testing against the
control socket -- see below):

```bash
./audio-engine/build/audio_engine /tmp/audio_engine.sock
```

## Real-time audio I/O (dev-machine testing, e.g. before a Pi exists)

By default `audio_engine` only runs the control socket -- no audio device
is ever opened, which is deliberate (see `IAudioIoBackend` below) and is
what the gtest suite uses. To actually hear audio through a real interface
while developing/testing on a normal machine (no Raspberry Pi, no GPIO
footswitch yet -- see the root README and `scripts/keyboard_footswitch.py`
for the rest of that story), build with the PortAudio backend and pass
`--audio`:

```bash
brew install portaudio pkg-config          # macOS; apt: portaudio19-dev pkg-config
cmake -S audio-engine -B audio-engine/build-audio -DAUDIO_ENGINE_WITH_PORTAUDIO=ON
cmake --build audio-engine/build-audio -j
./audio-engine/build-audio/audio_engine /tmp/audio_engine.sock --audio
```

Set your external interface as the system default input/output device
first (macOS: Audio MIDI Setup) -- device selection is deliberately just
"the OS default" for now, not a name/index flag (see
`portaudio_backend.hpp`).

**Seam, not a hard dependency.** `IAudioIoBackend` (`audio_io_backend.hpp`)
is the interface; `PortAudioBackend` is one implementation of it, built
only when `-DAUDIO_ENGINE_WITH_PORTAUDIO=ON` is passed (default `OFF`).
This is the same "narrow interface + swappable backend" pattern as
control-daemon's `FootswitchInputBackend`/`AudioEngineClient` and this
package's own `INamModel`/`IAssetLoader`: a production/Pi build doesn't
need to link PortAudio at all if it ends up using a different backend, and
nothing in `EngineChain`/`EngineState`/the control socket cares which
backend (if any) is driving it. `EngineState::processAudioBlock` is the
one new entry point the real-time callback calls once per block; it takes
the same mutex `handleCommand` does, so a `load_preset` command can never
race a concurrent audio block -- see `tests/test_engine_state.cpp`.

**Known gap: no crossfade in the real-time path yet.** `PresetSwitcher`
(equal-power crossfade, see "Deviation #4" below) is fully built and
tested standalone, but `EngineState::handleLoadPreset` still swaps
`ResourceManager`'s chain synchronously rather than handing old+new to a
`PresetSwitcher` -- so a preset switch while `--audio` is running is
thread-safe (same mutex) but not glitch-free; expect an audible click.
Wiring `PresetSwitcher` into the real-time path is the natural next step
once you're validating audio through real hardware, not attempted here to
keep this change scoped to "get real signal flowing."

**macOS microphone permission.** The very first time you run `--audio`,
opening the input device can trigger (or silently block on, if run
non-interactively/headless) a one-time TCC microphone-permission prompt
for whatever app launched the process (Terminal, iTerm, etc). Run it from
an actual interactive terminal window at least once and approve the
prompt; a headless/CI invocation of `--audio` will hang at that point
until permission has already been granted.

**Direct hardware monitoring will fight you.** If your interface has a
"direct monitor" knob/switch (common on Focusrite/PreSonus/etc -- routes
input straight to output in hardware, independent of the computer), it
sums your dry signal into the output *underneath* whatever the engine
sends back. Two copies of the same signal at a slight delay offset comb-
filter each other, which sounds like a generic "not quite flat" coloration
present no matter what the software is doing (confirmed this exact
symptom during manual testing -- it looked like a bypass bug at first, and
wasn't one). Turn direct monitoring off and monitor 100% through the
software path when testing this engine.

## Latency

`--audio`'s block size directly trades off round-trip latency against
xrun safety margin. Confirmed by benchmarking (not guessed): with the
convolution engine's real-time IR cap (`kMaxRealtimeIrSamples`, see
below), processing uses roughly the **same ~38% of the per-block budget
regardless of block size** (block-size samples of work and block-size
samples of budget both scale together), so shrinking the block size
trades latency for margin much less than intuition suggests:

| block size | budget/block | measured use | latency contribution |
|---|---|---|---|
| 256 | 5.33ms | 37.5% | ~5.33ms |
| 128 | 2.67ms | 37.4% | ~2.67ms |
| 64 (**default**) | 1.33ms | 37.6% | ~1.33ms |
| 32 | 0.67ms | 44.1% | ~0.67ms |

Default was lowered from 256 to **64** after real hardware testing
surfaced round-trip latency as an audible, real problem, not just a
theoretical one. Override with `--block-size N` -- go higher if you hear
crackling/dropouts (a general-purpose OS scheduler, not an RTOS, means
smaller blocks leave less slack for scheduling jitter than the CPU-time
numbers above alone suggest), lower if your machine has room and you want
less still.

On startup, `--audio` prints the **actual negotiated** input/output
latency PortAudio settled on for your device
(`PortAudioBackend::inputLatencySeconds`/`outputLatencySeconds`, via
`Pa_GetStreamInfo`) -- a measured number, not the block-size math above,
since the driver/OS can add its own buffering on top. True round-trip
latency is roughly that input+output sum, plus USB/driver overhead
PortAudio itself can't see.

## Sample rate policy

The engine standardizes on a single fixed internal operating rate --
**48kHz** (`ResourceManager`'s/`EngineState`'s default, and what
`--audio` asks the device for) -- rather than tracking whatever rate each
loaded asset happens to be at. Chosen over 44.1kHz because it's the
existing default everywhere in this tree, matches typical class-compliant
USB audio interfaces and Raspberry Pi audio HATs equally well, and headers
(control-daemon, mobile app) never asked for a specific rate, so there was
no reason to prefer the "CD audio" rate over it.

Any asset loaded at a different native rate is converted to 48kHz at load
time rather than played back as-is:

* **Cabinet IRs**: `loadImpulseResponseFile` (`convolution.cpp`) resamples
  via `resampleLinear` (`resample.hpp`/`.cpp`) whenever the WAV file's own
  rate differs from `ResourceManager`'s `sampleRate_`. This was a real,
  discovered bug, not a hypothetical -- an IR pack captured at 44.1kHz
  (common) played back audibly time-compressed/detuned against the
  engine's 48kHz processing before this existed. `resampleLinear` is naive
  linear interpolation (no anti-aliasing lowpass before downsampling) --
  the same "correct and simple over maximally faithful" tradeoff this
  codebase already makes for convolution itself (see deviation #3 below);
  a proper windowed-sinc resampler is a real follow-up, not attempted
  here. See `tests/test_resample.cpp` and
  `Convolution.LoadsImpulseResponseFromWavFileResamplingToTargetRate`.
* **NAM models**: metadata parsing records `sample_rate`, but nothing
  reads it yet -- inference itself is stubbed (see deviation #2 below), so
  there is no real forward pass to feed at the wrong rate today. A real
  `INamModel` implementation will need to resample its input the same way
  once inference exists.
* **The live device** (`PortAudioBackend`, via `--audio`): opened at
  `EngineState::sampleRate()` (48kHz) directly -- if your audio interface
  can't run at 48kHz at all, `Pa_OpenStream` fails outright with a clear
  error rather than silently running at a different rate than the rest of
  the engine assumes.

## Real-time-safe IR length cap

Also discovered during real hardware testing, alongside the sample-rate
mismatch above: `ConvolutionEngine`'s naive O(n·m) convolution (deviation
#3 below) genuinely cannot keep up with a long real-world cabinet IR at
real-time block rates -- this isn't theoretical. Measured on a real dev
machine, `--audio`'s default 256-sample/48kHz block gives a 5.33ms
per-block budget; a real 34623-sample IR (an "8x10 cabinet" capture that
bakes in room ambience, resampled from 44.1kHz to 48kHz per the policy
above) took **~8.3ms to convolve one block -- 156% of budget, a guaranteed
dropout on every single block**, which is exactly the "horrible, jerky"
audio reported when first testing through real hardware.

`loadImpulseResponseFile` now truncates any IR longer than
`kMaxRealtimeIrSamples` (8192 samples, ~171ms @ 48kHz) with a short linear
fade-out (`truncateIrWithFadeOut`) so the cut is inaudible rather than a
click. 8192 was chosen directly from measurement, not guessed: comparable
lengths used ~37-43% of the block budget in benchmarking, leaving solid
headroom for the rest of the effect chain and OS scheduling jitter. Real
amp-sim cabinet IRs are typically much shorter (10-50ms) -- this cap only
bites for unusually long "room capture" style IRs, and re-verified against
the actual triggering IR file: it now loads at exactly 8192 samples and
uses **2.0ms (38%) of budget**, not 8.3ms.

This is a stopgap, not the fix the naive-convolution deviation below
already calls for: a partitioned/FFT-based convolution engine would use a
long IR's *full* length in real time instead of cutting it short. That
remains the real follow-up; capping IR length is what makes real hardware
testing usable today without waiting on it.

## IR gain normalization

A separate real bug, also only surfaced by testing against real IR files
(not the same one as the length cap above -- fixing that first just
exposed this one): cabinet IR files have no standardized gain convention.
Convolving with an ungained one can multiply a normal playing-level signal
well past 0dBFS -- measured against two different real IR packs: one (a
raw "room capture" style file) clipped a moderate playing level to **3.6x
over range**; a second, unrelated pack that had seemed to "just work"
clipped to **1.1x over range** at the same level once actually measured.
This is a property of convolving with *any* ungained real-world IR, not a
defect specific to one file.

Fix, in `convolution.cpp`: `loadImpulseResponseFile` now also calls
`normalizeIrEnergy`, which scales the loaded IR to unit L2 (energy) norm
(`sqrt(sum(ir[k]^2)) == 1`). This is the standard normalization used in
convolution-reverb/cab-sim tools for exactly this problem: for a broadband
input (real guitar signal, not a pure tone), output RMS tracks input RMS
almost exactly at this normalization (a standard DSP identity, confirmed
empirically against both real files during manual testing) -- so
perceived loudness becomes comparable across differently-recorded IR
files instead of depending on how hot the original capture happened to
be, without changing an IR's tonal *shape* (a uniform scale preserves
relative proportions between taps).

Energy normalization controls average level, not worst-case peaks -- a
resonant IR can still produce a transient above unity on a loud input.
`EngineChain::process` (`resource_manager.cpp`) adds a defensive final
clamp to `[-1, 1]` as the second half of this fix: scoped to the whole
chain's combined output only (not into individual `EffectBlock`s, which
stay free to produce whatever their own isolated unit tests expect), so
it also catches any preset that simply stacks enough gain/EQ boost on its
own to do the same thing an ungained IR did. See
`tests/test_convolution.cpp` (`NormalizeIrEnergy*`) and
`tests/test_resource_manager.cpp`
(`ChainProcessClampsFinalOutputToUnitRange`).

**Known remaining gap: unusually peaky (high crest-factor) IRs can still
hit the clamp on transients.** Energy normalization matches *average*
loudness across IRs; it does nothing to bound *peak* loudness, which
depends on how peaky both the IR and the input signal are. Measured
against two real files: the Ampeg IR referenced above has a crest factor
(peak/RMS) of **37** raw, vs. **9** for a Shift Line Orange cab IR that
works cleanly -- a hard-pick-attack test signal pushed 86 of 48000 samples
into the Ampeg IR's clamp ceiling, and 0 for the Orange one. A
geometric-mean-of-L1-and-L2 normalization eliminates this (verified), but
was deliberately not adopted: it would also quiet down every
*already-correct* IR by roughly -10dB (including the Orange one), trading
a rare, bounded artifact on one unusually peaky file for a guaranteed
level change on files that don't need it. Left as-is: rare clamping on
hard attacks with outlier IRs is preferable to universally reduced
headroom. A per-IR adaptive threshold is a plausible middle ground but
wasn't pursued with only two real files to calibrate against.

## Module map

| Module (`include/audio_engine/` + `src/`) | Responsibility |
|---|---|
| `preset_model`   | `Preset`/`Asset`/`EffectBlockSpec` structs + JSON (de)serialization, shaped identically to `control-daemon`'s Pydantic models (see "Shared data model" below) |
| `effect_block`   | `EffectBlock` interface (`prepare(sampleRate)`, `process(buffer)`) implemented by every DSP block |
| `passthrough_block` | Identity `EffectBlock` -- test baseline / safe fallback for an unrecognized block type |
| `gain_block`       | Gain/volume block (`gain_db` param) |
| `eq_block`           | Biquad peaking/shelving EQ (RBJ Audio EQ Cookbook formulas) |
| `delay_block`         | Feedback delay line (`delay_ms`/`feedback`/`mix` params) |
| `wav_file`               | Hand-rolled RIFF/WAVE parser (reads PCM16/PCM24/PCM32/float32, mono or downmixed) + writer (PCM16/float32) |
| `resample`                 | `resampleLinear`: naive linear-interpolation sample-rate conversion (see "Sample rate policy" below) |
| `convolution`              | Naive O(n·m) time-domain cabinet-IR convolution engine, streaming across arbitrary block sizes; `loadImpulseResponseFile` resamples to the engine's target rate and truncates to a real-time-safe length (see "Real-time-safe IR length cap") |
| `nam_model`                  | Real `.nam` JSON metadata parser/validator (`NamModelMetadata`) + `INamModel` interface + `StubNamModel` (identity/gain passthrough -- inference is stubbed, see below) |
| `preset_switcher`              | Glitch-free crossfade between an "old" and "next" already-prepared processing chain |
| `resource_manager`                | Owns the one currently-loaded `EngineChain` (NAM + IR + effects); loading a new preset releases the previous one's resources |
| `engine_state`                      | Dispatches one parsed control-socket command against a `ResourceManager` + bypass/crossfade/active-preset state; `processAudioBlock` runs the current chain over one real-time audio block |
| `control_socket`                      | Unix domain socket server: newline-delimited JSON in, newline-delimited JSON reply out |
| `audio_io_backend`                      | `IAudioIoBackend` interface + `AudioCallback`/`AudioIoConfig` -- the real-time-audio-device seam (see "Real-time audio I/O" below) |
| `portaudio_backend`                        | `PortAudioBackend`: `IAudioIoBackend` over PortAudio, built only when `AUDIO_ENGINE_WITH_PORTAUDIO` is on |
| `main.cpp`                              | Process entry point: `audio_engine <control-socket-path> [--audio]` |

### Shared data model

`preset_model.hpp`'s `Preset`/`Asset`/`EffectBlockSpec` structs mirror
`control-daemon/src/control_daemon/models.py`'s `Preset`/`Asset`/
`EffectBlock` Pydantic models field-for-field (`id`, `name`, `blocks:
[{type, enabled, params}]`, `nam_asset_id`, `ir_asset_id`, `created_at`,
`updated_at` for `Preset`; `id`, `kind` ("nam"|"ir"), `filename`,
`stored_path`, `size_bytes`, `sha256`, `uploaded_at` for `Asset`), so a
preset JSON blob produced by the control daemon deserializes here with no
translation layer. `tests/test_preset_model.cpp` round-trips a fixture
shaped exactly like a real control-daemon preset export.

## Control socket protocol

Transport: a **Unix domain socket** (`SOCK_STREAM`) at a filesystem path
given as `argv[1]` to the `audio_engine` executable, carrying
**newline-delimited JSON**: one command object per line in, one reply
object per line out.

**Why a Unix socket instead of reusing control-daemon's WebSocket
protocol:** both processes always run on the same Raspberry Pi, so a
filesystem-path-addressed local socket needs no port allocation, gets
free OS-enforced access control via filesystem permissions on the socket
path, and needs no HTTP/WS framing. The engine also has exactly one
controller (the daemon) and no broadcast/multi-role concept the way the
daemon's WS protocol does (`app`/`footswitch`/`display` roles, broadcast
`state_changed`) -- reusing that protocol's message shapes here would drag
in machinery this engine doesn't need. This is a new, narrower protocol,
not a subset of the daemon's.

**Error shape mirrors control-daemon's WS `error` message style**
(`code` + `message`) for consistency across the system, even though the
transport differs:

```json
{"ok": false, "code": "validation_error", "message": "load_preset requires an object field 'preset'"}
```

`code` is one of: `validation_error`, `not_found`, `internal_error`,
`unknown_command`.

A successful command replies `{"ok": true, "cmd": "<name>", ...}` with
command-specific extra fields.

### Commands

**`load_preset`** -- loads and applies a full preset (see "Shared data
model" above for the exact `preset` shape; `nam_asset_id`/`ir_asset_id`
must already have been registered via `register_asset`, below, or be
`null`):
```json
{"cmd": "load_preset", "preset": {"id": "p1", "name": "Ambient Swell", "blocks": [{"type": "gain", "enabled": true, "params": {"gain_db": 3.0}}], "nam_asset_id": null, "ir_asset_id": null}}
```
```json
{"ok": true, "cmd": "load_preset", "preset_id": "p1"}
```

**`set_bypass`** -- global bypass toggle:
```json
{"cmd": "set_bypass", "bypass": true}
```
```json
{"ok": true, "cmd": "set_bypass", "bypass": true}
```

**`crossfade_ms`** -- sets the crossfade window duration (milliseconds)
used for the next preset switch:
```json
{"cmd": "crossfade_ms", "value": 50}
```
```json
{"ok": true, "cmd": "crossfade_ms", "value": 50}
```

**`register_asset`** -- registers metadata for a `.nam`/IR file already
written to disk (mirrors control-daemon's own `register_asset` WS
command/`Asset` shape -- see control-daemon/README.md):
```json
{"cmd": "register_asset", "asset": {"id": "nam1", "kind": "nam", "filename": "my_amp.nam", "stored_path": "/data/assets/nam1.nam", "size_bytes": 20971520, "sha256": "..."}}
```
```json
{"ok": true, "cmd": "register_asset", "asset_id": "nam1"}
```

**`get_state`** -- debug/query command, reads back current engine state
(used heavily by the test suite):
```json
{"cmd": "get_state"}
```
```json
{"ok": true, "cmd": "get_state", "state": {"bypass": false, "crossfade_ms": 50, "current_preset_id": "p1", "sample_rate": 48000.0, "registered_assets": 1}}
```

`tests/test_control_socket_integration.cpp` builds the real `audio_engine`
executable, launches it as a subprocess bound to a temp socket path,
connects a plain POSIX socket client, and asserts on acks/errors for all
of the above (including the unknown-command and malformed-preset error
paths).

## Deviations from the plan (and why)

### 1. No JUCE -- network sandbox constraint

**What's missing:** JUCE (and any real-time-audio-device I/O built on
it -- ALSA/PortAudio/etc.) is not present anywhere in this tree.

**Why:** this environment's network proxy allows plain `https://` GETs to
many hosts (`raw.githubusercontent.com`, PyPI, the Ubuntu apt mirror) but
returns **403 for `codeload.github.com`**, which is what both `git clone`
of a GitHub repo and a GitHub release/archive tarball download resolve
through. JUCE is normally vendored via one of exactly those two paths --
there is no apt package for it. Confirmed by direct test in this
container before writing any code; retrying would not change the outcome.

**What's built instead:** every module here is architected exactly like
control-daemon's hardware seams (`FootswitchInputBackend` +
`MockFootswitchBackend`, `AudioEngineClient` + `NullAudioEngineClient`):
a narrow interface with a real, fully-functional implementation behind it
wherever the logic doesn't actually require JUCE/real audio hardware
(`EffectBlock`, `IAssetLoader`, `INamModel`, the control socket), so a
real JUCE (or bare ALSA/PortAudio) backend can be dropped in later to
supply the actual live audio callback without changing any of the DSP,
preset-loading, crossfade, or IPC code. Concretely, the missing piece is
just: a real-time audio callback thread that pulls samples from a
hardware input, hands them to `EngineChain::process`/`PresetSwitcher`,
and pushes the result to a hardware output. Nothing in this tree
currently drives `EngineChain`/`PresetSwitcher` from such a callback --
that wiring, plus the JUCE (or ALSA) dependency it needs, is the
documented next step once this sandbox (or a real dev machine) has full
GitHub access or a vendored JUCE copy.

### 2. NAM inference stubbed -- same network constraint, different dependency

**What's real:** `.nam` file **metadata parsing** (`nam_model.hpp`'s
`parseNamModelMetadata`/`parseNamModelFile`). A `.nam` file is plain JSON
(the format used by the reference implementation,
github.com/sdatkinson/NeuralAmpModelerCore): a top-level `architecture`
name, an architecture-specific `config` object, a flat `weights` array,
and usually `sample_rate`/`metadata`. The parser validates all of this for
real and rejects malformed/incomplete files with a clear error
(`NamParseError`) -- see `tests/test_nam_model.cpp` for the full set of
rejected-input cases (missing architecture, non-object config, empty/
non-numeric weights, etc.).

**What's stubbed:** actually *running* the WaveNet/LSTM forward pass
described by `weights` (`StubNamModel` is a fixed identity/gain
passthrough, not real inference). Real inference requires vendoring
`NeuralAmpModelerCore` itself (MIT-licensed,
github.com/sdatkinson/NeuralAmpModelerCore) -- again only obtainable via
`git clone`/GitHub archive download, both blocked here (see deviation #1).

**The seam:** `INamModel` is the interface (`prepare`/`process`, plus
`metadata()`); `StubNamModel` is the only implementation today.
Swapping in a real implementation backed by `NeuralAmpModelerCore` (e.g.
as a git submodule, added from a machine with full GitHub access) is a
drop-in replacement behind this interface -- nothing else in the engine
(resource manager, preset switcher, control socket) needs to change.

**Known parser gap: only the flat single-model export shape is
understood.** `parseNamModelMetadata` requires a non-empty top-level
`weights` array. Verified against a real commercial `.nam` export
(Darkglass B7K Ultra) that this rejects: some plugins export a
`"SlimmableContainer"` architecture whose top-level `weights` is `[]`,
with the actual per-submodel weight data nested under
`config.submodels[...]` instead -- a materially different shape this
parser was never written against (it targets the reference
NeuralAmpModelerCore single-model format). Such a file fails
`load_preset` with `missing or invalid required field 'weights'`; since
inference is stubbed regardless (see above), this only blocks the
upload/register/load *plumbing* today, not any audible capability.
Supporting nested-submodel exports is unstarted follow-up work, not
attempted here.

### 3. Naive (not partitioned/FFT) convolution for cabinet IRs

`convolution.hpp`'s `ConvolutionEngine` is a textbook O(n·m) time-domain
FIR convolution: correct, simple, and fully tested (impulse IR ->
identity; hand-computed two-tap result; streaming across arbitrary block
sizes matches one-shot convolution -- see `tests/test_convolution.cpp`),
but it does not scale to real cabinet-IR lengths (hundreds to thousands of
taps) at real-time block rates -- confirmed by real benchmarking once real
hardware/audio I/O existed to test against (not guessed at), see
"Real-time-safe IR length cap" above for the measured numbers and the
truncation stopgap now in place. A partitioned/FFT-based (e.g.
uniform-partitioned overlap-save) convolution engine, which would use a
long IR's full length instead of capping it, remains the known follow-up
for real-time performance -- flagged rather than attempted here, consistent
with the product spec's own "latency is the main project risk, validate
early" framing.

### 4. Preset-switching crossfade curve: equal-power, not linear

`PresetSwitcher` blends old-chain and new-chain output with
`gainOld = cos(t*pi/2)`, `gainNew = sin(t*pi/2)` (so `gainOld^2 +
gainNew^2 == 1` throughout), rather than a linear `1-t`/`t` blend. A
linear crossfade measurably dips in perceived loudness for two
uncorrelated signals -- which is the realistic case here: switching
presets generally switches to a different amp model/IR/effects chain
entirely, not a phase-aligned copy of the same signal. Equal-power is the
standard audio-engineering choice for exactly this "switch between two
unrelated sources" scenario, and is still perfectly smooth/monotonic for
the simpler correlated-signal case (see
`tests/test_preset_switcher.cpp`).

"Preloading" (the other half of the glitch-free-switching requirement) is
enforced structurally, not just by convention: `PresetSwitcher` never
loads anything -- both chains handed to `beginCrossfade` must already be
fully prepared -- so `process()` never performs blocking file I/O. This is
asserted directly with a call-counting mock loader
(`tests/test_preset_switcher.cpp:
NoLoaderIoOnceCrossfadeProcessingHasStarted`): 2 loader calls happen
before `beginCrossfade`, and stay at 2 across every subsequent
`process()` call.

### 5. IPC transport: Unix domain socket + newline-delimited JSON, not the daemon's WS protocol

See "Control socket protocol" above for the full rationale.

## Memory budget

Per ARCHITECTURE.md's "Resource constraints": the audio engine is the one
process on the Pi that's actually expected to use real RAM (NAM model +
cabinet IR + effects), and the product spec puts running multiple
simulations in parallel explicitly out of scope for V1 -- so exactly one
model + one IR should ever be resident. `ResourceManager::loadPreset`
replaces the current `EngineChain` (a single `std::unique_ptr`) wholesale;
the previous chain's `shared_ptr<INamModel>`/`shared_ptr<IrHandle>` are
dropped as part of that replacement.

This isn't just asserted by inspection: `tests/test_resource_manager.cpp`
(`LoadingSequentialPresetsReleasesPreviousResources`) uses an
instance-counting `IAssetLoader` test double whose loaded NAM
model/IR objects increment a live-instance counter in their constructor
and decrement it in their destructor. Loading preset A, then B, then C in
sequence (three distinct loader calls, confirmed via a separate
total-loaded counter so the release assertions aren't vacuous) keeps the
live-instance count at exactly 1 for both NAM models and IRs after every
single load -- i.e. each preset's resources are actually released,
synchronously, as part of loading the next one, not merely dereferenced
and left for a GC that doesn't exist in C++. **Result: 1 live NAM model, 1
live IR, at every point in that test, confirmed by
`ResourceManager.LoadingSequentialPresetsReleasesPreviousResources`
passing.**

## Explicitly out of scope

- **A real-time audio backend for the Raspberry Pi specifically.**
  `--audio`/`PortAudioBackend` (see "Real-time audio I/O" above) covers
  dev-machine testing; what actually ships on the Pi (PortAudio-over-ALSA
  again, bare ALSA, or JUCE) is still an open call -- `IAudioIoBackend` is
  built so that's a new implementation behind the same interface, not a
  rewrite, whichever way it goes.
- **Crossfading a preset switch in the real-time path** -- see "Known gap"
  under "Real-time audio I/O" above; `PresetSwitcher` itself is built and
  tested, just not wired into `EngineState` yet.
- **Loading actual third-party LV2/VST3 plugin binaries.**
- **Real WaveNet/LSTM NAM inference** -- see deviation #2 above.
- **FFT-based/partitioned convolution performance optimization** -- see
  deviation #3 above; naive convolution is correct but not real-time-fast
  at production IR lengths.
- **JUCE integration** -- see deviation #1 above; PortAudio covers the
  "get real audio flowing on a dev machine" need without it.
