// Common interface implemented by every real-time DSP effect block
// (gain_block.hpp, eq_block.hpp, delay_block.hpp, passthrough_block.hpp).
//
// The engine's signal path is mono (a single guitar input), matching the
// "instrument input, one output channel" hardware note in ARCHITECTURE.md,
// so blocks process a plain in-place float buffer rather than a
// multi-channel structure.
#pragma once

#include <cstddef>
#include <vector>

namespace audio_engine {

class EffectBlock {
public:
    virtual ~EffectBlock() = default;

    // Called once before the first process() call, and again whenever the
    // sample rate changes. Implementations should (re)size any
    // sample-rate-dependent state here, not in process().
    virtual void prepare(double sampleRate) = 0;

    // Processes `buffer` in place, `numSamples` valid samples starting at
    // buffer[0]. Must not allocate or perform I/O (real-time safety).
    virtual void process(float* buffer, std::size_t numSamples) = 0;

    // Convenience overload for tests / non-real-time callers.
    void process(std::vector<float>& buffer) { process(buffer.data(), buffer.size()); }

    // Clears any internal state (delay lines, filter history) without
    // needing a full prepare() call. Default: no-op (stateless blocks like
    // Gain/Passthrough don't need it).
    virtual void reset() {}
};

}  // namespace audio_engine
