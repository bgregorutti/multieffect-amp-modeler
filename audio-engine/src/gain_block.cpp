#include "audio_engine/gain_block.hpp"

#include <cmath>

namespace audio_engine {

namespace {
double paramAsDouble(const ParamMap& params, const std::string& key, double fallback) {
    auto it = params.find(key);
    if (it == params.end()) return fallback;
    if (auto* d = std::get_if<double>(&it->second)) return *d;
    return fallback;
}
}  // namespace

GainBlock::GainBlock(double gainDb) : gainDb_(gainDb), linearGain_(static_cast<float>(std::pow(10.0, gainDb / 20.0))) {}

GainBlock::GainBlock(const ParamMap& params) : GainBlock(paramAsDouble(params, "gain_db", 0.0)) {}

void GainBlock::setGainDb(double gainDb) {
    gainDb_ = gainDb;
    linearGain_ = static_cast<float>(std::pow(10.0, gainDb / 20.0));
}

void GainBlock::prepare(double /*sampleRate*/) {
    // Stateless: nothing sample-rate dependent to (re)compute.
}

void GainBlock::process(float* buffer, std::size_t numSamples) {
    for (std::size_t i = 0; i < numSamples; ++i) {
        buffer[i] *= linearGain_;
    }
}

}  // namespace audio_engine
