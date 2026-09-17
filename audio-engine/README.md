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
(`nlohmann-json`, `googletest`) on macOS, not from GitHub. Last run: **151/151
tests passed** (`ctest --test-dir audio-engine/build`), covering every
module below (the three modelled pedal blocks and their Python-parity golden
renders included) plus a real-subprocess control-socket integration test.
This default build needs no network access and no GitHub-hosted dependency
-- see "Real-time audio I/O", "Real NAM inference" and "VST3 plugin hosting"
below for the three optional, network-fetching build flags
(`-DAUDIO_ENGINE_WITH_VST3=ON` alone: **159/159**); re-run `ctest` for the
current per-flag (and flag-combination) counts rather than trusting a
hardcoded one here, as they drift with every test added.

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

## Real-time callback health monitoring

`PortAudioBackend::paCallback` previously discarded PortAudio's own
`PaStreamCallbackFlags` parameter entirely -- a real gap, found while
chasing a live-only "jerky/saturated" audio report that every offline
reproduction (including `nam_render`, and a direct `EngineChain::process`
run against the reporter's own real recorded signal) came back completely
clean on: zero clamp engagement, comfortable timing margins, no NAM
computation issue. That pointed squarely at the real-time device path
itself, which nothing had ever actually instrumented.

Now tracked, via atomics updated inside the callback (no locks, no I/O
there -- real-time safe) and polled every ~2s from the main thread:
* **`xrunCount`** -- PortAudio/the driver's own `paInputUnderflow` /
  `paInputOverflow` / `paOutputUnderflow` / `paOutputOverflow` flags,
  ground truth for "did the device itself glitch," previously invisible.
* **`overBudgetCount`** -- callbacks where wall-clock time around just the
  `EngineState::processAudioBlock` call (not the memcpy/bookkeeping
  around it) exceeded that block's real-time budget -- our own code being
  the bottleneck, distinct from a device-level xrun.
* **`maxCallbackMicros`** -- worst single callback observed, for the life
  of the stream.

`--audio` prints a delta line to stderr whenever either counter moves,
plus a final summary on shutdown. Distinguishes two previously-conflated
possibilities: the device/driver actually underrunning (`xrunCount`) vs.
our own processing occasionally running long (`overBudgetCount`) -- prior
to this, "jerky" audio gave no signal on which of those (or neither) was
actually happening.

**This instrumentation is what found the actual root cause of a real
"jerky and saturated" report, and it's worth spelling out because it's an
easy mistake to repeat: `audio_engine` was being configured with
`-DCMAKE_BUILD_TYPE=Debug`.** CMakeLists.txt's own default (when
`CMAKE_BUILD_TYPE` isn't passed at all) is `RelWithDebInfo` -- optimized
-- but an explicit `Debug` override during development silently replaced
that. Nothing about real NAM inference is slow; **unoptimized Eigen is.**
Eigen's matrix operations are deeply-templated expression trees that rely
on the compiler inlining and vectorizing them -- without `-O2`/`-O3`,
every one of those expressions materializes as literal, unfused,
unvectorized function calls. Measured on the exact same code, same model,
same real recorded signal: **Debug build averaged 3.6ms/block (100% of
blocks over the 1.33ms budget); `RelWithDebInfo` averaged 0.06ms/block
(0% over budget) -- a 59x difference from the compiler flag alone.** Every
offline reproduction attempt during that investigation (`nam_render`,
`EngineChain::process` run directly) had been built with `-O2` by hand and
came back clean, while the actual `--audio` binary -- built via CMake with
`Debug` -- was the only thing actually failing, which is exactly why it
took real-time callback instrumentation (`xrunCount`/`overBudgetCount`
above) to catch: **`overBudgetCount` climbing steadily while `xrunCount`
stayed at 0** was the signature that pointed at "our own code is
consistently slow," not a device-level problem. **Always build
`audio_engine` as `RelWithDebInfo` or `Release` for anything real-time
(`--audio`, or any latency-sensitive testing) -- reserve `Debug` for
control-socket-only work with no audio device involved, where raw DSP
throughput doesn't matter.**

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

## Modelled pedal blocks

Three pedals implemented as hand-written DSP rather than NAM captures:
`big_muff`, `tube_screamer` and `noise_gate` (`pedal_dsp.hpp` plus one header
per pedal). A `.nam` file bakes in the knob positions it was captured at, and
most of what a pedal's knobs do is not worth an inference: Volume is a
multiply, Tone is a linear filter network. Only the gain control reshapes the
nonlinearity. NAM stays the right tool for the **amp**; for a gate it cannot
work at all, since gating is time-varying gain rather than a static
nonlinearity.

Measured on this dev machine, `-O2`, per 256-sample block (5.33 ms budget):

| Block | Cost | Latency |
|---|---|---|
| `big_muff` | 0.095 ms — **1.8%** of budget | 16 samples (0.33 ms) |
| `tube_screamer` | 0.071 ms — **1.3%** | 16 samples (0.33 ms) |
| `noise_gate` | 0.0012 ms — **0.02%** | 0 — causal, no lookahead |

All three together cost about a quarter of what one NAM inference does (13%
worst case, above). The latency on the distortion blocks is the anti-aliasing
oversampler: they run their clipping stages 4x oversampled, without which a
hard-driven clipper folds its harmonics back as inharmonic aliasing.

### The Python reference, and parity

These are a **port of `vst-python/`**, which is the reference implementation --
where each pedal's behaviour was designed, measured and argued about, and where
the circuit constants live in a form that is quick to iterate on against a real
plugin. That is not a claim anyone has to take on trust:
`vst-python/tools/export_golden.py` freezes a render from each Python model
into `tests/golden/pedal_parity.json`, and `tests/test_python_parity.cpp`
replays the identical input through the C++ block and compares.

Parity is asserted to **1e-6**, not bit-exactness. The Python computes in
float64 throughout; `EffectBlock`'s buffers are `float`. These blocks keep
every intermediate in `double` and round once on the way out, so what remains
is a single float quantization. The oversampler's FIR is checked separately
against the exported `scipy.signal.firwin` taps, so a divergence in the filter
*design* reports itself rather than surfacing as a mysteriously wrong waveform.

After any deliberate change to a Python model, regenerate the fixture:

```bash
cd vst-python && .venv/bin/python tools/export_golden.py \
    ../audio-engine/tests/golden/pedal_parity.json
```

### Parameter smoothing, and why it does not break parity

The Python models deliberately have **no** parameter smoothing: they apply
parameters per block, which is what keeps their renders block-size invariant
and exactly reproducible. A real-time engine cannot do that -- a gain stepping
discontinuously mid-buffer clicks audibly.

`ParamSmoother` resolves the two. `snap()` sets current and target to the same
value and `next()` then returns it unchanged forever, with no epsilon and no
drift; blocks snap in their constructor, `prepare()` and `reset()`, and only
`setLiveParam()` ramps. So a preset load renders identically to the reference
while a slider drag stays click-free. Both halves are tested:
`PedalParameterSmoothing.LiveChangeConvergesToTheConstructedValue` (smoothing
changes *when* a value arrives, never *where* it lands),
`...LiveChangeDoesNotClick`, and `...ResetSnapsInsteadOfRamping`.

The gate needs no smoother: its output gain is already ramped by its own
attack/release coefficients, and its other parameters feed a state machine
rather than multiplying the signal.

### Running these under sanitizers

The pedal DSP is the one part of this tree doing raw pointer arithmetic over
pre-allocated scratch buffers (the oversampler), so it is worth checking:

```bash
cmake -S audio-engine -B audio-engine/build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build audio-engine/build-asan -j
ASAN_OPTIONS=detect_container_overflow=0 ./audio-engine/build-asan/audio_engine_tests
```

`detect_container_overflow=0` is required and is **not** hiding a bug of ours:
Homebrew's `libgtest` is not ASan-instrumented, and ASan's container
annotations only agree when the whole program is. Without the flag the build
aborts during gtest's own test *discovery*, inside `GTestIsInitialized()`
copying a `vector<string>` -- no project code on the stack at all.
Heap-buffer-overflow and every UBSan check stay on, and those are what catch a
real overrun. Last run: **151/151 clean**.

### A memory bug the sanitizer confirmed

`Oversampler`'s scratch buffers are sized once in `prepare()` for
`kPedalMaxChunk` samples, which is what keeps `process()` allocation-free in
the real-time path. The pedal blocks chunk their own buffers to match -- but
`Oversampler` is public, and a *direct* caller passing a longer buffer ran
straight off the end of `stuffed_`.

Being a vector overrun this was undefined behaviour rather than a clean crash,
and it behaved like one: the full suite passed, and the symptom only appeared
when an unrelated new test shifted the heap layout. It then looked like
innocuous filter error -- output drifting further from the input the longer the
buffer ran -- rather than like memory corruption. `upsample`/`downsample` now
split long calls internally, carrying filter state across the splits so the
result is identical either way, and
`OversamplerFilter.HandlesBuffersLongerThanItsInternalChunk` pins it.

The lesson worth keeping: a green suite over UB is not evidence. The
sanitizer run above is.

### A bug the parity test caught

Worth recording, because it is exactly what this test is for. The first parity
run failed on the gate alone, by 2.2e-6 at sample 9. The cause was in the
*Python*, not the port: `NoiseGate.reset()` ran from `__init__` before
`set_params`, so the gate started at the **default** floor rather than the
configured one and spent a release-time ramping between them at the start of
every render. The C++ was right. Fixed in the reference (the closed gain is now
resolved lazily on the first sample), fixture regenerated, and both sides now
agree.

## Real NAM inference

Real WaveNet/LSTM amp-model inference, not the identity/gain-passthrough
stub described in deviation #2 below. Opt-in, same pattern as
`AUDIO_ENGINE_WITH_PORTAUDIO`:

```bash
cmake -S audio-engine -B audio-engine/build-nam -DAUDIO_ENGINE_WITH_REAL_NAM=ON
cmake --build audio-engine/build-nam -j
```

(Combine with `-DAUDIO_ENGINE_WITH_PORTAUDIO=ON` in the same build to get
both real inference and real audio I/O -- both flags are independent.)

**What this vendors, and why FetchContent, not a submodule.** CMake
`FetchContent` pulls two repos at configure time, each pinned to a fixed
commit/tag for reproducibility:
* `NeuralAmpModelerCore` (MIT, github.com/sdatkinson/NeuralAmpModelerCore)
  -- pinned to a specific commit, not `main`, so a rebuild months from now
  vendors the exact same code.
* `Eigen` (MPL2, gitlab.com/libeigen/eigen) -- pinned to `3.4.0`, the
  matrix-math library NeuralAmpModelerCore's WaveNet/LSTM implementations
  are built on.

Both are fetched with `SOURCE_SUBDIR` pointed at a directory that doesn't
exist, which populates the source tree without running either repo's own
CMakeLists.txt -- we only want NeuralAmpModelerCore's `NAM/` sources and
Eigen's headers, not NeuralAmpModelerCore's own `tools/` build (which
needs a second submodule, `AudioDSPTools`, that we have no use for). A
tiny generated `json.hpp` shim (`#include <nlohmann/json.hpp>`) satisfies
NeuralAmpModelerCore's `#include "json.hpp"` using our own
already-required nlohmann_json, rather than fetching a second copy of the
same library.

**Real gotcha, not a hypothetical one, worth documenting:**
`nam::get_dsp()` dispatches to a per-architecture parser (WaveNet, LSTM,
container/multi-submodel, ...), each self-registered via a static
initializer in its own translation unit. A plain
`target_link_libraries(... nam_core)` against the static library only
pulls in `.o` members that resolve an outstanding undefined symbol --
since nothing calls a symbol from e.g. `wavenet/model.cpp` directly, the
linker silently drops that whole file, registration side-effect included,
and `get_dsp()` then throws `No config parser registered for
architecture: WaveNet` at runtime for every real file. Fixed with CMake's
`$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>` generator expression (forces
every object file in the archive to link, portable across ld64/GNU ld) --
see `CMakeLists.txt`.

**Architecture support is NeuralAmpModelerCore's, not reimplemented
here.** `RealNamModel` (`real_nam_model.hpp`/`.cpp`) is a thin adapter:
converts our mono in-place `float*` buffer to/from the vendored library's
`NAM_SAMPLE**` channel-indexed convention and calls straight through to
`nam::DSP::process`. Whatever architecture `nam::get_dsp()` understands,
this engine understands -- including `"SlimmableContainer"` (multi-
submodel, gain-stage-switching exports, e.g. from TONE3000's newer
trainer), which is exactly the format that turned out to matter: **every
real `.nam` file tried against this** (two different commercial packs)
used that container format, not the simpler flat single-model shape this
project's own metadata parser originally assumed. That parser
(`nam_model.hpp`) was relaxed accordingly -- `weights` is no longer
required to be non-empty at the top level, only to be an array of numbers
*if present* -- since container architectures legitimately nest their real
weights under `config.submodels[...]` instead. The parser deliberately
does not validate architecture-specific structure beyond that; real
structural validation is `nam::get_dsp()`'s job now, not a second,
competing implementation of an evolving format here.

**Measured, not assumed, real-time cost.** Benchmarked against all 14 real
`.nam` files available during development (two commercial packs, all
`"SlimmableContainer"` WaveNet exports, 48kHz native), including
worst-case (not just average) per-block time -- a real concern for a
level-dependent multi-submodel container architecture, since a heavier
submodel could in principle get selected right when the player is
digging in hardest: **worst case across all 14 files was 0.17ms per
64-sample/48kHz block, 13% of the 1.33ms budget** -- comfortable margin,
zero over-budget blocks in a 2-second/1500-block stress test per file.
Real neural inference is not the bottleneck on this hardware (cabinet-IR
convolution uses far more of the budget -- see "Real-time-safe IR length
cap" above). `prepare()` (which calls `nam::DSP::Reset`, including
prewarm -- settling dilated-conv/recurrent history so the first real
block isn't a "cold" transient) took ~10-15ms in the same test -- fine,
since it runs once per preset *load* (`ResourceManager::loadPreset`),
never in the real-time audio callback.

**Testing.** `tests/test_real_nam_model.cpp` (built only with the flag on)
reuses NeuralAmpModelerCore's own bundled `example_models/*.nam` fixtures
(already fetched, no extra cost) rather than hand-rolled fixtures that
might not match real export shapes -- including
`example_models/slimmable_container.nam` for the container-format case
specifically. Verifies: loads without throwing (both flat and container
architectures), output is bounded/finite and demonstrably not a no-op,
`prepare()` is safe to call repeatedly (preset reload / sample-rate
change). It does not re-verify the neural network's own numerical
correctness -- that's NeuralAmpModelerCore's own test suite's job, not
duplicated here.

**Diagnostic tool.** `tools/nam_render.cpp` (built only with the flag on,
as the `nam_render` target) runs a WAV file through a real `.nam` model
using the exact production code path (`parseNamModelFile` + `get_dsp` +
`RealNamModel`, same resample-to-48kHz, same default 64-sample block
size) and writes out both the resampled-but-unprocessed ("dry") and
NAM-processed ("wet") signal as separate 48kHz float32 WAV files, plus
peak/RMS stats for each -- for directly listening to or diffing what the
model actually did, rather than inferring it from aggregate numbers
alone:
```bash
./audio-engine/build-nam/nam_render <input.wav> <model.nam> dry.wav wet.wav [block_size]
```

### NAM output loudness normalization

A real bug, found the same way the IR gain problem was (see "IR gain
normalization" above): `.nam` files are not gain-consistent with each
other any more than cabinet IRs are. Surveyed all 14 real `.nam` files
available during development (two commercial packs, one with 9 "gain
stage" variants of the same amp) -- one measured **~6dB hotter** than its
siblings (`GetLoudness()` -17.18dB vs. a -19 to -23dB range for the rest)
and clipped audibly (measured peak 1.29, i.e. 1.29x over range) on an
otherwise-unremarkable test signal with zero compensation. Every other
file stayed safely under 0.5 peak on the same signal -- this wasn't a
timing/xrun problem (worst-case block time measured at 13% of budget
across all 14 files, comfortable margin) or an architecture problem, just
uncorrected gain.

Fix: `RealNamModel` now reads the model's own `nam::DSP::GetLoudness()` (a
standard field NAM models carry specifically for this purpose -- "how loud
is this model's output for a typical input") and applies a linear gain
scaling every model to a common target, `kNamTargetLoudnessDb = -22.0` dB
(`real_nam_model.hpp`). That number wasn't guessed: chosen empirically by
testing candidate targets (-20 to -26dB) against all 14 real files under
both moderate-level and aggressive-pick-attack synthetic signals, picking
the highest (loudest, least unnecessarily-quiet) target that still kept
every file's worst-case measured peak comfortably under 1.0 (worst case at
-22dB: 0.54 peak under an aggressive-attack stress test, ~5.4dB of
headroom). A model with no loudness metadata at all gets no correction
(gain 1.0, not a guess) -- see `tests/test_real_nam_model.cpp`'s
`NoGainAppliedWhenModelReportsNoLoudness`. This is the same "correct the
input, keep the defensive clamp as backup, don't just clamp harder"
layered approach as the IR fix, and the existing chain-level clamp in
`EngineChain::process` remains the final safety net regardless.

**Known gaps, not attempted here:**
* No resampling if a model's own `expected_sample_rate` differs from the
  engine's 48kHz (see "Sample rate policy" above) -- all real files tried
  were natively 48kHz, so this hasn't bitten yet, but a model trained at
  e.g. 44.1kHz would currently run at the wrong rate.
* `nam::NamFileValidationError` (thrown by `get_dsp()` on a file its
  fuller validation rejects) isn't specifically caught in
  `engine_state.cpp`'s command dispatch -- it falls through to the
  generic `std::exception` handler and reports as `internal_error` rather
  than `validation_error`. Not wrong, just less precise than it could be.

## VST3 plugin hosting

Hosts real VST3 effect plugins as ordinary chain blocks (`type: "vst3"`),
alongside the native gain/eq/delay/tone-stack blocks -- a rig chain can mix
both freely, in any order, any number of `"vst3"` blocks included (unlike
`"nam"`/`"ir"`, which each own exactly one dedicated `EngineChain` slot).
Opt-in, same pattern as `AUDIO_ENGINE_WITH_REAL_NAM`:

```bash
cmake -S audio-engine -B audio-engine/build-vst3 -DAUDIO_ENGINE_WITH_VST3=ON
cmake --build audio-engine/build-vst3 -j
```

Off builds keep `type: "vst3"` resolvable (`vst3_plugin_host_stub.cpp`
throws one clear "not built with VST3 support" error) rather than a link
error or a silently-wrong passthrough standing in for a plugin the preset
actually expects.

**No JUCE, no public.sdk -- a hand-written COM-style host.** Only
`pluginterfaces` is vendored (via CMake `FetchContent`, pinned to
`steinbergmedia/vst3_pluginterfaces` tag `v3.7.14_build_55` -- the
`vst3sdk` repo's own `pluginterfaces/` directory is itself a submodule
pointing at this separate repo, so it's fetched directly rather than
pulling all of `vst3sdk` and its other submodules for one subdirectory).
`vst3_host.hpp`'s `IHostedPlugin`/`loadVst3Plugin()` seam is the same
"narrow interface + swappable backend" pattern as `INamModel`/
`IAssetLoader`: `Vst3PluginHost` (`vst3_plugin_host.cpp`, real
implementation) `dlopen`s a `.vst3` bundle's
`Contents/<arch>-linux/*.so`, resolves its exported `GetPluginFactory`,
walks `IPluginFactory` for the first `"Audio Module Class"`, and drives
`IComponent`/`IAudioProcessor`/`IEditController` directly -- no
third-party framework, since only the interface headers (plus the four
`.cpp` files `pluginterfaces` itself needs compiled: `funknown.cpp`,
`coreiids.cpp`, `conststringtable.cpp`, `ustring.cpp`) are vendored.

**A real gotcha worth documenting:** none of the `Vst`-namespace
interfaces (`IComponent`, `IAudioProcessor`, `IEditController`, ...) get a
`DEF_CLASS_IID` definition anywhere in `pluginterfaces` itself -- only the
base ones (`FUnknown`, `IPluginBase`, `IPluginFactory*`) do, in
`coreiids.cpp`. Referencing e.g. `IComponent::iid` (the `FUID` static
member `DECLARE_CLASS_IID` declares but doesn't define without a
`DEF_CLASS_IID` line somewhere) would link-fail with an undefined
reference. Fixed by using the header-declared `IComponent_iid` (etc.)
`TUID` constants instead everywhere an interface id is needed --
namespace-scope `static`, so every translation unit that includes the
header gets its own private copy with no linker symbol at all, sidestepping
the whole `INIT_CLASS_IID`-in-exactly-one-translation-unit dance real VST3
SDK builds normally need.

**Mono or stereo only.** `Vst3PluginHost` negotiates the plugin's main
bus arrangement at load time: a mono-in/mono-out plugin runs directly; a
stereo-in/stereo-out plugin gets the engine's mono signal duplicated to
L/R on the way in and averaged back to mono on the way out. Any other bus
shape (a plugin with sidechain/aux busses, a synth with no audio input,
...) throws a descriptive `std::runtime_error` at load time -- fail loud,
not a guess about what to do with an unsupported layout.

**Parameter automation goes through a real `IParameterChanges` queue, not
a shortcut.** `IEditController::setParamNormalized` only updates the
controller's own UI-facing state per the VST3 spec ("must never pass this
value-change back to the host... update the GUI element(s) only") -- it
does not by itself affect what a separate-controller plugin's
`IAudioProcessor::process()` actually does. `Vst3PluginHost::setParam()`
therefore does both: calls `setParamNormalized` (state sync) *and* queues
a point on a host-owned `IParameterChanges`/`IParamValueQueue`
implementation delivered via `ProcessData::inputParameterChanges` on the
next `process()` call -- the same mechanism a real DAW's automation lane
uses. A parameter's plain-units min/max/default (not just its normalized
`[0,1]` range) come from asking the plugin's own
`normalizedParamToPlain(id, 0.0)` / `(id, 1.0)` / `(id,
defaultNormalizedValue)` -- the standard trick a host UI uses to build a
real-world-unit slider, since `ParameterInfo` itself carries no plain
min/max fields.

**Real-time-safety-sized blocks.** `IAudioProcessor::setupProcessing`
requires declaring a `maxSamplesPerBlock` upfront; `Vst3PluginHost` uses a
generous fixed ceiling (4096 samples) and chunks any larger `process()`
call into pieces of at most that size, rather than assuming a specific
engine block size or violating the plugin's declared contract.

**No plugin GUI, no state persistence (yet).** `IEditController::
createView` is never called (headless host -- see Step 3c's planned mobile
generic-parameter-UI instead of a native plugin editor) and `setState`/
`getState` are never called either (a freshly loaded plugin always starts
at its own defaults; whatever a preset previously tweaked is expected to
come back via Step 3b's `set_block_param`, not save-state serialization
-- see the top-level plan).

**Discovery is a filesystem scan, not a phone upload** (planned, Step 3b):
a `.vst3` bundle on Linux is a *directory*
(`MyPlugin.vst3/Contents/<arch>-linux/*.so`), which doesn't fit the
existing single-file `POST /assets/upload` endpoint at all -- getting a
plugin onto the pedal is an out-of-band install step, not a mobile-app
upload flow.

**Testing without any third-party plugin binary.** One trivial in-repo
VST3 plugin (`tests/vst3_test_plugin/gain_plugin.cpp`: a single "Gain"
parameter, plain range `[0, 2]`x, mono in/out) is built from the same
vendored `pluginterfaces` and packaged into a real bundle directory
structure by a CMake post-build step, so `tests/test_vst3_host.cpp`
exercises `Vst3PluginHost` end to end -- real `dlopen`, real
`IPluginFactory`/`IComponent`/`IAudioProcessor`/`IEditController` calls --
with zero external dependency, same ethos as
`MockFootswitchBackend`/`NullAudioEngineClient` elsewhere in this project.
Its `GainProcessor` class implements `IComponent` + `IAudioProcessor` +
`IEditController` via plain multiple inheritance (no virtual inheritance)
-- the standard VST3 "single component" idiom (public.sdk's own
`AudioEffect` combines `IComponent`+`IAudioProcessor` the same way) --
with `queryInterface`/`addRef`/`release` written by hand rather than via
the single-interface `IMPLEMENT_FUNKNOWN_METHODS` macro, since that only
branches on one interface id and this class answers for three. Real
third-party plugins (DISTRHO-family first, since it builds VST3 from the
same source as its LV2 builds, so an ARM build is more likely to exist or
be buildable) are for later manual/hardware validation, not this test.

## Module map

| Module (`include/audio_engine/` + `src/`) | Responsibility |
|---|---|
| `preset_model`   | `Preset`/`Asset`/`EffectBlockSpec` structs + JSON (de)serialization, shaped identically to `control-daemon`'s Pydantic models (see "Shared data model" below) |
| `effect_block`   | `EffectBlock` interface (`prepare(sampleRate)`, `process(buffer)`, optional `setLiveParam(key, value)` for a no-reload tweak) implemented by every DSP block |
| `passthrough_block` | Identity `EffectBlock` -- test baseline / safe fallback for an unrecognized block type |
| `gain_block`       | Gain block (`gain_db` param) -- built for block types `"gain"` (input trim) and `"volume"` (output level): same DSP, two type names so a rig's chain can tell them apart |
| `eq_block`           | Biquad peaking/shelving EQ (RBJ Audio EQ Cookbook formulas); `setGainDb`/`gainDb()` support live boost/cut updates without reconstructing the block |
| `tone_stack_block`     | 3-band tone stack (block type `"tone_stack"`, `{bass_db, mid_db, treble_db}` params) composing three `EqBlock`s (low-shelf/peaking/high-shelf) -- tone shaping around a NAM capture isn't part of the captured model itself, see "Rig chain: the gain stages around a NAM capture" below |
| `delay_block`         | Feedback delay line (`delay_ms`/`feedback`/`mix` params, all three live-settable via `setLiveParam`) |
| `pedal_dsp`             | Shared DSP primitives (diode clipping, oversampling, `ParamSmoother`) behind the three modelled pedal blocks, ported from `vst-python/src/pedals/dsp.py` -- see "Modelled pedal blocks" below |
| `big_muff_block`         | `BigMuffBlock` (block type `"big_muff"`) -- fuzz/distortion, ported from `vst-python/src/pedals/bigmuff.py` |
| `tube_screamer_block`      | `TubeScreamerBlock` (block type `"tube_screamer"`) -- overdrive, ported from `vst-python/src/pedals/tubescreamer.py` |
| `noise_gate_block`           | `NoiseGateBlock` (block type `"noise_gate"`) -- ported from `vst-python/src/pedals/noisegate.py` |
| `block_type_registry`     | Static per-type parameter schema (label/unit/min/max/default) for every native block type -- backs the `list_block_types` command, see "Commands" below |
| `wav_file`               | Hand-rolled RIFF/WAVE parser (reads PCM16/PCM24/PCM32/float32, mono or downmixed) + writer (PCM16/float32) |
| `resample`                 | `resampleLinear`: naive linear-interpolation sample-rate conversion (see "Sample rate policy" below) |
| `convolution`              | Naive O(n·m) time-domain cabinet-IR convolution engine, streaming across arbitrary block sizes; `loadImpulseResponseFile` resamples to the engine's target rate and truncates to a real-time-safe length (see "Real-time-safe IR length cap") |
| `nam_model`                  | Real `.nam` JSON metadata parser (`NamModelMetadata`) + `INamModel` interface + `StubNamModel` (identity/gain passthrough -- default when `AUDIO_ENGINE_WITH_REAL_NAM` is off) |
| `real_nam_model`               | `RealNamModel`: real WaveNet/LSTM inference via vendored NeuralAmpModelerCore, built only when `AUDIO_ENGINE_WITH_REAL_NAM` is on -- see "Real NAM inference" above |
| `vst3_host`                       | `IHostedPlugin`/`Vst3EffectBlock` (block type `"vst3"`) + `loadVst3Plugin()`; `Vst3PluginHost` (real hosting, built only when `AUDIO_ENGINE_WITH_VST3` is on) or a stub that throws a clear error otherwise -- see "VST3 plugin hosting" above |
| `preset_switcher`              | Glitch-free crossfade between an "old" and "next" already-prepared processing chain |
| `resource_manager`                | Owns the one currently-loaded `EngineChain` (NAM + IR + effects); loading a new preset releases the previous one's resources |
| `engine_state`                      | Dispatches one parsed control-socket command against a `ResourceManager` + bypass/crossfade/active-preset state; `processAudioBlock` runs the current chain over one real-time audio block |
| `control_socket`                      | Unix domain socket server: newline-delimited JSON in, newline-delimited JSON reply out |
| `audio_io_backend`                      | `IAudioIoBackend` interface + `AudioCallback`/`AudioIoConfig` -- the real-time-audio-device seam (see "Real-time audio I/O" below) |
| `portaudio_backend`                        | `PortAudioBackend`: `IAudioIoBackend` over PortAudio, built only when `AUDIO_ENGINE_WITH_PORTAUDIO` is on |
| `main.cpp`                              | Process entry point: `audio_engine <control-socket-path> [--audio]` |

### Shared data model

`preset_model.hpp`'s `Preset`/`Asset`/`EffectBlockSpec` structs mirror
`control-daemon/src/control_daemon/models.py`'s `ResolvedPreset`/`Asset`/
`ResolvedBlock` field-for-field (`id`, `name`, `rig_id`, `rig_name`,
`blocks: [{id, type, asset_id, enabled, params}]` for `Preset`; `id`,
`kind` ("nam"|"ir"|"vst3"), `filename`, `stored_path`, `size_bytes`,
`sha256`, `uploaded_at` for `Asset`), so a resolved-preset JSON blob produced by the
control daemon deserializes here with no translation layer. There is no
preset-level `nam_asset_id`/`ir_asset_id` -- the daemon models a rig (an
ordered chain whose amp/cab blocks are pinned) containing presets that
only toggle the rest of that chain, and flattens rig+preset into this
`ResolvedPreset` shape before sending it; the engine never sees a rig or
a preset override, only the exact chain to play, `blocks` order included.
`tests/test_preset_model.cpp` round-trips a fixture shaped exactly like a
real control-daemon resolved-preset export.

### Rig chain: the gain stages around a NAM capture

A captured `.nam` model has no gain/tone controls of its own -- it's a
frozen snapshot of one amp setting. The daemon's default new-rig template
(`control_daemon.models.default_rig_chain`) surrounds the amp/cab with
four extra pinned blocks that add that feel back in software, in this
fixed order:

```
gain (input trim) -> nam (amp) -> ...switchable effects... -> ir (cab) -> tone_stack -> volume (output level)
```

* **`gain`** before the amp -- the "push the amp harder/softer" feel a
  physical gain knob would give, since the model itself can't be pushed.
* **`tone_stack`** after the cab -- generic 3-band tone shaping, since
  that's not part of the captured model or the cabinet IR either.
* **`volume`** after the tone stack -- a plain output-level trim. Built by
  `createEffectBlock` from the exact same `GainBlock` as `"gain"`; the two
  type names exist only so a chain (and its UI) can tell an input-trim
  knob from an output-level one apart, not because the DSP differs.

None of this is special-cased in the engine beyond `createEffectBlock`
recognizing the `"gain"`/`"tone_stack"`/`"volume"` type strings -- same
"just another block" treatment as `"eq"`/`"delay"`. The daemon is what
gives new rigs this shape by default; `resource_manager.cpp` only needs
to know how to build each block type.

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

**`load_preset`** -- loads and applies a full (already rig-resolved)
preset (see "Shared data model" above for the exact `preset` shape; each
block's `asset_id`, if any, must already have been registered via
`register_asset`, below, or be `null`):
```json
{"cmd": "load_preset", "preset": {"id": "p1", "name": "Ambient Swell", "rig_id": "rig1", "rig_name": "Ampeg SVT", "blocks": [{"id": "gain1", "type": "gain", "asset_id": null, "enabled": true, "params": {"gain_db": 3.0}}]}}
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

**`register_asset`** -- registers metadata for a `.nam`/IR/`.vst3` asset
already on disk (mirrors control-daemon's own `register_asset` WS
command/`Asset` shape -- see control-daemon/README.md):
```json
{"cmd": "register_asset", "asset": {"id": "nam1", "kind": "nam", "filename": "my_amp.nam", "stored_path": "/data/assets/nam1.nam", "size_bytes": 20971520, "sha256": "..."}}
```
```json
{"ok": true, "cmd": "register_asset", "asset_id": "nam1"}
```
For `"kind": "vst3"`, the reply also carries the plugin's own parameter
schema (a throwaway instantiation purely to introspect it -- see "VST3
plugin hosting" above) so the daemon never needs a second round-trip to
learn what knobs a plugin has:
```json
{"ok": true, "cmd": "register_asset", "asset_id": "plug1", "parameters": [{"key": "0", "label": "Gain", "unit": "x", "min": 0.0, "max": 2.0, "default": 1.0, "step_count": 0}]}
```

**`set_block_param`** -- live, no-reload parameter tweak on one block of
the *currently loaded* chain, addressed by the block's own id (native
block's own param key, e.g. `"gain_db"`; a `"vst3"` block's stringified
VST3 `ParamID`, from `register_asset`'s or `list_block_types`'
`"parameters"`). Mutates the block in place, guarded by the same mutex
`processAudioBlock` uses -- never calls `load_preset`:
```json
{"cmd": "set_block_param", "block_id": "boost", "param_key": "gain_db", "value": 6.0}
```
```json
{"ok": true, "cmd": "set_block_param", "block_id": "boost", "param_key": "gain_db", "value": 6.0}
```
`not_found` if no preset is loaded or no *enabled* block with that id
exists in the current chain; `validation_error` if the block doesn't
recognize `param_key`.

**`list_block_types`** -- static parameter schema for every native
(non-`"vst3"`) block type this engine build recognizes -- see
`block_type_registry.hpp`. Static per engine build, not per-rig state, so
a client fetches this once (e.g. on connect) rather than per preset:
```json
{"cmd": "list_block_types"}
```
```json
{"ok": true, "cmd": "list_block_types", "block_types": [{"type": "gain", "parameters": [{"key": "gain_db", "label": "Gain", "unit": "dB", "min": -60.0, "max": 24.0, "default": 0.0, "step_count": 0}]}, ...]}
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

### 2. NAM inference: was stubbed, now real (see "Real NAM inference" below)

This deviation is historical -- kept for the record of *why* it was
stubbed for most of this project's development. The network constraint
that caused it (this sandbox's proxy blocking `codeload.github.com`, the
host both `git clone` of a GitHub repo and a release/archive tarball
download resolve through) doesn't apply to every environment this code
runs in; once it was built on a machine with full GitHub access, real
inference became a same-day drop-in behind the `INamModel` interface this
section always said it would be. See "Real NAM inference" below for what
actually shipped, including real measured performance numbers.

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
- **Loading LV2 plugin binaries.** (VST3 hosting shipped -- see "VST3
  plugin hosting" above -- but only VST3; an LV2 host would be a separate,
  unimplemented effort.)
- **FFT-based/partitioned convolution performance optimization** -- see
  deviation #3 above; naive convolution is correct but not real-time-fast
  at production IR lengths.
- **JUCE integration** -- see deviation #1 above; PortAudio covers the
  "get real audio flowing on a dev machine" need without it.
