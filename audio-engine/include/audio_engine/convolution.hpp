// Cabinet impulse-response convolution.
//
// This is a NAIVE time-domain (O(n*m)) convolution engine: for each output
// sample it sums `ir.size()` multiply-adds against a history buffer. This
// is correct and simple to reason about/test, but does not scale to the
// (~hundreds to thousands of taps) IR lengths a real cabinet IR can have
// at real-time block rates on a Raspberry Pi -- confirmed, not just
// theorized: a real 34623-sample cabinet IR (resampled to 48kHz) measured
// at ~8.3ms to convolve one 256-sample/48kHz block on a real dev machine,
// 156% of the 5.33ms budget -- i.e. a guaranteed dropout on every single
// block (see "real-time-safe IR length cap" below, and
// audio-engine/README.md "Sample rate policy" for the full numbers this
// was derived from).
//
// Deliberately deferred follow-up (see audio-engine/README.md): a
// partitioned/FFT-based (e.g. uniform-partitioned overlap-save) convolution
// engine is the known next step for using a long IR's full length in real
// time without a cap. Per the product spec's own framing ("latency is the
// main project risk, validate early"), that optimization is flagged rather
// than attempted here.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "audio_engine/effect_block.hpp"

namespace audio_engine {

// Streaming convolution against a fixed impulse response, maintaining a
// history ring buffer across process() calls so callers can feed audio in
// arbitrarily-sized blocks (as a real-time engine would) and still get a
// result identical to convolving the whole signal at once.
class ConvolutionEngine : public EffectBlock {
public:
    using EffectBlock::process;  // bring the std::vector<float>& convenience overload back into scope

    ConvolutionEngine() = default;
    explicit ConvolutionEngine(std::vector<float> impulseResponse);

    void setImpulseResponse(std::vector<float> impulseResponse);
    const std::vector<float>& impulseResponse() const { return ir_; }

    void prepare(double sampleRate) override;
    void process(float* buffer, std::size_t numSamples) override;
    void reset() override;

private:
    std::vector<float> ir_;
    std::vector<float> history_;  // most recent ir_.size()-1 input samples, oldest first
};

// Real-time-safe IR length cap: 8192 samples (~171ms @ 48kHz). Chosen from
// the benchmark above's numbers -- at this length, naive convolution uses
// well under half the 5.33ms/block budget (measured ~37% for a comparable
// length), leaving headroom for the rest of the effect chain plus OS
// scheduling jitter. Real amp-sim cabinet IRs are typically much shorter
// (10-50ms); this cap only bites for unusually long "room capture" style
// IRs (some commercial packs bake in room ambience/reverb tail, which is
// exactly what triggered writing this in the first place).
inline constexpr std::size_t kMaxRealtimeIrSamples = 8192;

// Truncates `ir` to at most `maxSamples`, if longer, with a short linear
// fade-out over the last min(maxSamples, 256) samples so the cut doesn't
// produce an audible click (the last sample of a truncated IR is exactly
// silence). A no-op (returns `ir` unchanged) if it's already within
// `maxSamples`.
std::vector<float> truncateIrWithFadeOut(std::vector<float> ir, std::size_t maxSamples);

// Loads a mono WAV file as a cabinet IR (thin wrapper over wav_file.hpp
// that returns just the sample data, since that's all ConvolutionEngine
// needs), resampled to `targetSampleRate` if the file's own sample rate
// differs (see resample.hpp -- the engine standardizes on one internal
// rate, and a mismatched IR would otherwise play back time-compressed/
// detuned), then truncated to `kMaxRealtimeIrSamples` (see above) if still
// longer than that. Throws WavParseError -- see wav_file.hpp -- on a
// malformed file.
std::vector<float> loadImpulseResponseFile(const std::string& path, double targetSampleRate);

// Pure function form, useful for tests and for one-shot (non-streaming)
// convolution of two known buffers: output length is input.size() +
// ir.size() - 1 (full convolution), matching a hand-computed expectation.
std::vector<float> convolveFull(const std::vector<float>& input, const std::vector<float>& ir);

}  // namespace audio_engine
