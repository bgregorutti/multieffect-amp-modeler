// Cabinet impulse-response convolution.
//
// This is a NAIVE time-domain (O(n*m)) convolution engine: for each output
// sample it sums `ir.size()` multiply-adds against a history buffer. This
// is correct and simple to reason about/test, but does not scale to the
// (~hundreds to thousands of taps) IR lengths a real cabinet IR can have
// at real-time block rates on a Raspberry Pi.
//
// Deliberately deferred follow-up (see audio-engine/README.md): a
// partitioned/FFT-based (e.g. uniform-partitioned overlap-save) convolution
// engine is the known next step for real-time performance. Per the product
// spec's own framing ("latency is the main project risk, validate early"),
// that optimization is flagged rather than attempted here -- correctness
// first, on short test IRs, with the performance work called out as
// explicitly out of scope for this task.
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

// Loads a mono WAV file as a cabinet IR (thin wrapper over wav_file.hpp
// that returns just the sample data, since that's all ConvolutionEngine
// needs), resampled to `targetSampleRate` if the file's own sample rate
// differs (see resample.hpp -- the engine standardizes on one internal
// rate, and a mismatched IR would otherwise play back time-compressed/
// detuned). Throws WavParseError -- see wav_file.hpp -- on a malformed
// file.
std::vector<float> loadImpulseResponseFile(const std::string& path, double targetSampleRate);

// Pure function form, useful for tests and for one-shot (non-streaming)
// convolution of two known buffers: output length is input.size() +
// ir.size() - 1 (full convolution), matching a hand-computed expectation.
std::vector<float> convolveFull(const std::vector<float>& input, const std::vector<float>& ir);

}  // namespace audio_engine
