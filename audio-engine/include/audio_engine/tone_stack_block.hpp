// Guitar-amp-style 3-band tone stack: three cascaded EqBlocks (low-shelf
// "bass", peaking "mid", high-shelf "treble"), exposed behind a
// {bass_db, mid_db, treble_db} surface rather than the app/daemon having
// to juggle three raw generic "eq" blocks. See the product spec's
// "adjustable parameters around the static NAM capture" -- placed after
// the amp+cabinet stage in EngineChain, since tone shaping isn't part of
// the captured NAM model itself.
#pragma once

#include "audio_engine/eq_block.hpp"
#include "audio_engine/effect_block.hpp"
#include "audio_engine/preset_model.hpp"

namespace audio_engine {

class ToneStackBlock : public EffectBlock {
public:
    using EffectBlock::process;  // bring the std::vector<float>& convenience overload back into scope

    // Default corner frequencies are a generic "guitar amp tone stack"
    // shape, not modeled on any specific real amp: a bass shelf low
    // enough to move low-end weight without touching mids, a mid peak in
    // the "boxy/honky" range most amp tone stacks target, and a treble
    // shelf high enough to affect "air"/pick attack.
    explicit ToneStackBlock(double bassDb = 0.0, double midDb = 0.0, double trebleDb = 0.0);
    explicit ToneStackBlock(const ParamMap& params);

    // Live updates (recompute coefficients immediately) -- distinct from
    // reconstructing the block, same as GainBlock::setGainDb/
    // EqBlock::setGainDb this composes.
    void setBassDb(double gainDb);
    void setMidDb(double gainDb);
    void setTrebleDb(double gainDb);
    double bassDb() const { return bass_.gainDb(); }
    double midDb() const { return mid_.gainDb(); }
    double trebleDb() const { return treble_.gainDb(); }

    void prepare(double sampleRate) override;
    void process(float* buffer, std::size_t numSamples) override;
    void reset() override;
    // Recognizes "bass_db"/"mid_db"/"treble_db".
    bool setLiveParam(const std::string& key, double value) override;

private:
    EqBlock bass_;
    EqBlock mid_;
    EqBlock treble_;
};

}  // namespace audio_engine
