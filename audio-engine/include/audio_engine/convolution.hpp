// Cabinet impulse-response convolution.
//
// ConvolutionEngine uses uniformly partitioned FFT convolution (vendored
// FFTConvolver, MIT, third_party/fftconvolver/ -- the Darkglass fork of
// HiFi-LoFi's library). The IR is cut into kConvolutionPartitionSize-sample
// partitions whose spectra are computed once at load time; each partition of
// input then costs one forward FFT, one inverse FFT and one complex
// multiply-add per IR partition, instead of ir.size() multiply-adds per
// sample. No added latency: output sample n depends on input up to n, exactly
// like the direct form.
//
// History: the first version was a naive time-domain O(n*m) loop. A capped
// 8192-tap IR cost ~8192 MACs/sample (~390M/s at 48kHz) plus two heap
// allocations per block, which overran the 64-sample block budget on the Pi
// with heavier NAMs ("crunchy/jerky" sound, docs/dsp-load-brief.md).
// convolveFull() below keeps that direct form as the test reference;
// tools/conv_bench.cpp compares both.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "audio_engine/effect_block.hpp"

namespace fftconvolver {
class FFTConvolver;
}

namespace audio_engine {

// FFT partition size. Matches the engine's default 64-sample block
// (AudioIoConfig::blockSize) so each callback does one FFT/IFFT pair; other
// block sizes still work (the convolver buffers internally, zero latency).
inline constexpr std::size_t kConvolutionPartitionSize = 64;

// Streaming convolution against a fixed impulse response, keeping its
// input history across process() calls so callers can feed audio in
// arbitrarily-sized blocks (as a real-time engine would) and still get a
// result identical to convolving the whole signal at once.
class ConvolutionEngine : public EffectBlock {
public:
    using EffectBlock::process;  // bring the std::vector<float>& convenience overload back into scope

    ConvolutionEngine() = default;
    explicit ConvolutionEngine(std::vector<float> impulseResponse);
    ~ConvolutionEngine() override;

    // Allocates (FFT of every IR partition): call at load time, never from
    // the audio callback.
    void setImpulseResponse(std::vector<float> impulseResponse);
    const std::vector<float>& impulseResponse() const { return ir_; }

    void prepare(double sampleRate) override;
    void process(float* buffer, std::size_t numSamples) override;
    void reset() override;

private:
    std::vector<float> ir_;
    std::unique_ptr<fftconvolver::FFTConvolver> conv_;
};

// IR length cap: 16384 samples (~341ms @ 48kHz). With the FFT engine the
// cost grows slowly with length (tools/conv_bench, x86 dev box, 64-sample
// blocks: 8192 taps ~0.5% of the block budget, 16384 ~1%, vs 55% / 110%
// for the old direct form), so this is no longer a CPU limit but a sanity
// bound on memory and on room/reverb tails: real cabinet IRs keep 99.9% of
// their energy within ~100ms (scripts/check_assets.py), and the longest
// ones seen (0.2-5.9s room captures) are mostly room tail. Was 8192 with
// the naive engine.
inline constexpr std::size_t kMaxRealtimeIrSamples = 16384;

// Truncates `ir` to at most `maxSamples`, if longer, with a short linear
// fade-out over the last min(maxSamples, 256) samples so the cut doesn't
// produce an audible click (the last sample of a truncated IR is exactly
// silence). A no-op (returns `ir` unchanged) if it's already within
// `maxSamples`.
std::vector<float> truncateIrWithFadeOut(std::vector<float> ir, std::size_t maxSamples);

// Scales `ir` so its L2 (energy) norm -- sqrt(sum(ir[k]^2)) -- is 1.
// Real cabinet IR files are not gain-consistent: some are recorded/
// exported "hot" (a raw room-mic capture rather than a level-matched
// commercial IR), and convolving with an ungained IR can multiply a
// normal playing-level signal well past 0dBFS -- confirmed during manual
// testing against two different real IR packs (one clipped a moderate
// playing level to 3.6x over range, the other -- a "known good" file --
// still clipped at 1.1x). Unit-L2 normalization is the standard approach
// for this in convolution-reverb/cab-sim tools: for a broadband (roughly
// white) input, filter output RMS tracks input RMS almost exactly at this
// normalization (a standard DSP identity, verified empirically here), so
// perceived loudness stays comparable across differently-recorded IR
// files instead of depending on how hot the original capture was. Not a
// hard clipping guarantee by itself (a resonant IR can still produce
// transient peaks above unity on loud input) -- see
// EngineChain::process's defensive output clamp for the second half of
// that story. A no-op if `ir`'s energy is already ~0 (silent/degenerate
// IR -- nothing to normalize against, and dividing by ~0 would blow up).
std::vector<float> normalizeIrEnergy(std::vector<float> ir);

// Loads a mono WAV file as a cabinet IR (thin wrapper over wav_file.hpp
// that returns just the sample data, since that's all ConvolutionEngine
// needs), resampled to `targetSampleRate` if the file's own sample rate
// differs (see resample.hpp -- the engine standardizes on one internal
// rate, and a mismatched IR would otherwise play back time-compressed/
// detuned), truncated to `kMaxRealtimeIrSamples` (see above) if still
// longer than that, then energy-normalized (see normalizeIrEnergy above).
// Throws WavParseError -- see wav_file.hpp -- on a malformed file.
std::vector<float> loadImpulseResponseFile(const std::string& path, double targetSampleRate);

// Pure function form, useful for tests and for one-shot (non-streaming)
// convolution of two known buffers: output length is input.size() +
// ir.size() - 1 (full convolution), matching a hand-computed expectation.
std::vector<float> convolveFull(const std::vector<float>& input, const std::vector<float>& ir);

}  // namespace audio_engine
