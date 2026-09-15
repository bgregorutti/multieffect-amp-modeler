// Common interface implemented by every real-time DSP effect block
// (gain_block.hpp, eq_block.hpp, delay_block.hpp, passthrough_block.hpp).
//
// The engine's signal path is mono (a single guitar input), matching the
// "instrument input, one output channel" hardware note in ARCHITECTURE.md,
// so blocks process a plain in-place float buffer rather than a
// multi-channel structure.
#pragma once

#include <cstddef>
#include <string>
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

    // Live, no-reload parameter tweak (e.g. a slider drag): mutates one
    // named parameter of an *already-loaded* block in place, guarded by
    // whatever mutex the caller already holds around processAudioBlock --
    // same contract as GainBlock::setGainDb, just reachable generically by
    // key so a single engine command (set_block_param) can drive any block
    // type, native or VST3, without a per-type special case. `key` is the
    // block's own parameter name for native blocks (e.g. "gain_db") or a
    // stringified VST3 ParamID for a Vst3EffectBlock; `value` is in the
    // parameter's own plain units (dB, ms, ...), not a normalized [0,1].
    // Returns false if `key` isn't a parameter this block recognizes --
    // fail safe, same spirit as an unrecognized block `type` falling back
    // to PassthroughBlock in createEffectBlock(). Default: no live params.
    virtual bool setLiveParam(const std::string& /*key*/, double /*value*/) { return false; }
};

}  // namespace audio_engine
