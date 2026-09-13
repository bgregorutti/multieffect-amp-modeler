// Identity effect block: leaves the buffer untouched. Used as a test
// baseline for the crossfade/chain machinery and as a safe default block.
#pragma once

#include "audio_engine/effect_block.hpp"

namespace audio_engine {

class PassthroughBlock : public EffectBlock {
public:
    using EffectBlock::process;  // bring the std::vector<float>& convenience overload back into scope

    void prepare(double /*sampleRate*/) override {}
    void process(float* /*buffer*/, std::size_t /*numSamples*/) override {}
};

}  // namespace audio_engine
