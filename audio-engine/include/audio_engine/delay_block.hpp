// Simple feedback delay line: delay_ms (echo spacing), feedback (0..~0.99,
// how much of the delayed signal feeds back in), mix (0=dry..1=wet).
#pragma once

#include <vector>

#include "audio_engine/effect_block.hpp"
#include "audio_engine/preset_model.hpp"

namespace audio_engine {

class DelayBlock : public EffectBlock {
public:
    using EffectBlock::process;  // bring the std::vector<float>& convenience overload back into scope

    explicit DelayBlock(double delayMs = 300.0, double feedback = 0.3, double mix = 0.5);
    explicit DelayBlock(const ParamMap& params);

    void prepare(double sampleRate) override;
    void process(float* buffer, std::size_t numSamples) override;
    void reset() override;

    std::size_t delaySamples() const { return delaySamples_; }
    double feedback() const { return feedback_; }
    double mix() const { return mix_; }

private:
    double delayMs_;
    double feedback_;
    double mix_;
    double sampleRate_ = 48000.0;

    std::vector<float> buffer_;  // circular delay line
    std::size_t delaySamples_ = 0;
    std::size_t writeIndex_ = 0;
};

}  // namespace audio_engine
