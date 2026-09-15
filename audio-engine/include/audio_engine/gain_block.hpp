// Simple gain (volume) block: params["gain_db"] (default 0 dB = unity).
#pragma once

#include "audio_engine/effect_block.hpp"
#include "audio_engine/preset_model.hpp"

namespace audio_engine {

class GainBlock : public EffectBlock {
public:
    using EffectBlock::process;  // bring the std::vector<float>& convenience overload back into scope

    explicit GainBlock(double gainDb = 0.0);

    // Reads params["gain_db"] if present (defaults to 0 dB otherwise).
    explicit GainBlock(const ParamMap& params);

    void setGainDb(double gainDb);
    double gainDb() const { return gainDb_; }
    float linearGain() const { return linearGain_; }

    void prepare(double sampleRate) override;
    void process(float* buffer, std::size_t numSamples) override;
    // Recognizes "gain_db" (used by both the "gain" and "volume" block types).
    bool setLiveParam(const std::string& key, double value) override;

private:
    double gainDb_;
    float linearGain_;
};

}  // namespace audio_engine
