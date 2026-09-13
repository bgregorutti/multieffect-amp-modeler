// Biquad-based parametric EQ block using Robert Bristow-Johnson's "Audio
// EQ Cookbook" formulas (https://www.w3.org/andrew/2011/ap/cookbook.html --
// a widely-implemented public-domain reference derivation, not a
// GitHub-fetched dependency; the coefficient formulas are transcribed
// directly into eq_block.cpp).
//
// Supports two filter shapes, chosen by params["filter_type"]:
//   "peaking" (default) -- boost/cut a band around freq_hz by gain_db, Q
//   "low_shelf" / "high_shelf" -- shelving boost/cut, using params
//   "freq_hz" as the shelf corner frequency.
//
// Params (all optional, defaults shown):
//   freq_hz    = 1000.0   center/corner frequency
//   gain_db    = 0.0      boost (+) or cut (-)
//   q          = 0.707    peaking Q, or shelf slope proxy
//   filter_type = "peaking"
#pragma once

#include <array>

#include "audio_engine/effect_block.hpp"
#include "audio_engine/preset_model.hpp"

namespace audio_engine {

enum class EqFilterType { Peaking, LowShelf, HighShelf };

class EqBlock : public EffectBlock {
public:
    using EffectBlock::process;  // bring the std::vector<float>& convenience overload back into scope

    EqBlock(EqFilterType type = EqFilterType::Peaking, double freqHz = 1000.0, double gainDb = 0.0,
            double q = 0.70710678);
    explicit EqBlock(const ParamMap& params);

    void prepare(double sampleRate) override;
    void process(float* buffer, std::size_t numSamples) override;
    void reset() override;

    // Exposed for unit testing coefficient correctness / stability directly.
    struct Coefficients {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // a0 normalized to 1
    };
    const Coefficients& coefficients() const { return coeffs_; }

private:
    void recomputeCoefficients();

    EqFilterType type_;
    double freqHz_;
    double gainDb_;
    double q_;
    double sampleRate_ = 48000.0;
    Coefficients coeffs_;

    // Direct Form I history.
    double x1_ = 0, x2_ = 0, y1_ = 0, y2_ = 0;
};

}  // namespace audio_engine
