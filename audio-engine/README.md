# audio-engine

The real-time audio engine for the DIY AI guitar multi-effects pedal. It
loads a NAM (Neural Amp Modeler) model + a cabinet impulse response (IR) +
an effects chain per preset and processes the guitar signal. The
(already-built) `control-daemon/` is the single source of truth for which
preset is active and drives this engine over a local control socket (see
"Control socket protocol" below) -- this package only builds the engine
side; wiring `control-daemon`'s `AudioEngineClient` to actually connect
here is a follow-up, not part of this task.

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

Requires: CMake >= 3.16, a C++20 compiler, and two apt packages
(`nlohmann-json3-dev`, `libgtest-dev` + `libgmock-dev`) -- all fetched from
the normal Ubuntu package mirror, not from GitHub. Last run in this
container: **68/68 tests passed** (`ctest --test-dir audio-engine/build`),
covering every module below plus a real-subprocess control-socket
integration test.

Run the engine standalone (mostly useful for manual testing against the
control socket -- see below):

```bash
./audio-engine/build/audio_engine /tmp/audio_engine.sock
```

## Module map

| Module (`include/audio_engine/` + `src/`) | Responsibility |
|---|---|
| `preset_model`   | `Preset`/`Asset`/`EffectBlockSpec` structs + JSON (de)serialization, shaped identically to `control-daemon`'s Pydantic models (see "Shared data model" below) |
| `effect_block`   | `EffectBlock` interface (`prepare(sampleRate)`, `process(buffer)`) implemented by every DSP block |
| `passthrough_block` | Identity `EffectBlock` -- test baseline / safe fallback for an unrecognized block type |
| `gain_block`       | Gain/volume block (`gain_db` param) |
| `eq_block`           | Biquad peaking/shelving EQ (RBJ Audio EQ Cookbook formulas) |
| `delay_block`         | Feedback delay line (`delay_ms`/`feedback`/`mix` params) |
| `wav_file`               | Hand-rolled RIFF/WAVE parser + writer (PCM16/PCM32/float, mono or downmixed) |
| `convolution`              | Naive O(n·m) time-domain cabinet-IR convolution engine, streaming across arbitrary block sizes |
| `nam_model`                  | Real `.nam` JSON metadata parser/validator (`NamModelMetadata`) + `INamModel` interface + `StubNamModel` (identity/gain passthrough -- inference is stubbed, see below) |
| `preset_switcher`              | Glitch-free crossfade between an "old" and "next" already-prepared processing chain |
| `resource_manager`                | Owns the one currently-loaded `EngineChain` (NAM + IR + effects); loading a new preset releases the previous one's resources |
| `engine_state`                      | Dispatches one parsed control-socket command against a `ResourceManager` + bypass/crossfade/active-preset state |
| `control_socket`                      | Unix domain socket server: newline-delimited JSON in, newline-delimited JSON reply out |
| `main.cpp`                              | Process entry point: `audio_engine <control-socket-path>` |

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

### 3. Naive (not partitioned/FFT) convolution for cabinet IRs

`convolution.hpp`'s `ConvolutionEngine` is a textbook O(n·m) time-domain
FIR convolution: correct, simple, and fully tested (impulse IR ->
identity; hand-computed two-tap result; streaming across arbitrary block
sizes matches one-shot convolution -- see `tests/test_convolution.cpp`),
but it does not scale to real cabinet-IR lengths (hundreds to thousands of
taps) at real-time block rates on a Raspberry Pi. A partitioned/FFT-based
(e.g. uniform-partitioned overlap-save) convolution engine is the known
follow-up for real-time performance -- flagged rather than attempted here,
consistent with the product spec's own "latency is the main project risk,
validate early" framing (this is exactly the kind of thing that should be
prototyped and benchmarked once real hardware/audio I/O exists, not
guessed at now).

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

## Explicitly out of scope for this task

- **Real-time audio device I/O** (ALSA/JACK/PortAudio) -- no audio
  hardware exists in this sandbox to develop or test against.
- **Loading actual third-party LV2/VST3 plugin binaries.**
- **Real WaveNet/LSTM NAM inference** -- see deviation #2 above.
- **FFT-based/partitioned convolution performance optimization** -- see
  deviation #3 above; naive convolution is correct but not real-time-fast
  at production IR lengths.
- **JUCE integration** -- see deviation #1 above.
- Wiring control-daemon's `AudioEngineClient` to actually connect to this
  engine's control socket (control-daemon is a sibling component, already
  built and committed, and out of scope to modify for this task) --
  purely a client-side follow-up once both halves exist.
