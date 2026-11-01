# Brief: cut DSP load (FFT convolution + slim NAM)

Background: `IDEAS.md` on branch `claude/darkglass-ideas`, items 1 (FFT
convolver), 2 (NeuralAudio) and 10 (expose CPU load).

## Problem

Some NAM + IR combinations sound crunchy or jerky on the Pi (64-sample blocks
at 48 kHz). Asset scan (`scripts/check_assets.py`):

- **IRs:** 22 of 44 hit the 8192-sample real-time cap
  (`kMaxRealtimeIrSamples`, `audio-engine/include/audio_engine/convolution.hpp`).
  Ampeg SVT/8x10, Orange 4x12, Marshall 1960 IRs are 0.2-5.9 s, while 99.9 %
  of their energy is within 33-96 ms. `ConvolutionEngine::process`
  (`audio-engine/src/convolution.cpp`) is a direct time-domain loop, so a
  capped IR costs about 8192 multiply-adds per sample (~390 M/s), all on the
  audio thread.
- **NAMs:** all 120 are TONE3000 `SlimmableContainer` files with two
  submodels (~1.9k and ~12.1k weights). NeuralAmpModelerCore's
  `ContainerModel` defaults to the last (largest) one, so every amp runs the
  heavy model.

Both share the audio thread; together they can exceed the block budget, which
shows up as underruns.

## Solution for IR: partitioned FFT convolution (IDEAS.md #1)

- Vendor `FFTConvolver` (MIT, no dependencies; Darkglass fork has
  `TwoStageFFTConvolver`) under `audio-engine/`, keeping its license notice.
- Replace the inner loop of `ConvolutionEngine` with it. Use uniform
  partitions of the block size (64), or two-stage (64 head, 1024+ tail):
  this adds no latency.
- Keep the existing pipeline in front: resampling to the engine rate, energy
  normalisation, tail fade.
- `init()` allocates: call it at IR load time (non-RT), never in the
  callback. Pre-size it for the engine's maximum block size.
- Raise or remove `kMaxRealtimeIrSamples` once benchmarked (the expected cost
  at 8192 taps is 20-30x lower). Update the cap section in
  `audio-engine/README.md` and the budget comment in `audio_io_backend.hpp`.
- Tests: compare against the existing naive `convolve()` reference for
  random IRs and block sizes (1, 63, 64, 256, numSamples not a multiple of the
  partition); check impulse in gives the IR out; add a benchmark run on the Pi.

## Solution for NAM

1. **Short term: pick the slim submodel.** `ContainerModel` implements
   `SlimmableModel::SetSlimmableSize(double)` (`NAM/container.h`); the
   breakpoints come from `GetSlimmableSizeBreakpoints()`. After
   `nam::get_dsp()` in `audio-engine/src/resource_manager.cpp`, if the DSP is
   a `SlimmableModel`, apply a size from a new engine option (e.g.
   `--nam-size 0..1`, default 1 = current behaviour), set before `Reset()`.
   Later this can become a per-block "quality" param in the block type
   registry and the app.
2. **Medium term: NeuralAudio backend (IDEAS.md #2).** Add it behind the
   existing `NamModel` interface (`-DAUDIO_ENGINE_WITH_NEURALAUDIO=ON`) and
   benchmark against NAM Core on the Pi at block size 64.

## Verify

- Engine log (`journalctl -u multieffect-audio-engine`): "over-budget
  block(s)" and "device xrun(s)" counters should drop to zero with a capped
  IR + full NAM.
- Optional (IDEAS.md #10): forward those counters through the daemon so the
  app can show DSP load.
