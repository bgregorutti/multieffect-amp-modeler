#include "audio_engine/tone_stack_block.hpp"

namespace audio_engine {

namespace {
constexpr double kBassFreqHz = 100.0;
constexpr double kMidFreqHz = 600.0;
constexpr double kMidQ = 0.7;
constexpr double kTrebleFreqHz = 3000.0;

double paramAsDouble(const ParamMap& params, const std::string& key, double fallback) {
    auto it = params.find(key);
    if (it == params.end()) return fallback;
    if (auto* d = std::get_if<double>(&it->second)) return *d;
    return fallback;
}
}  // namespace

ToneStackBlock::ToneStackBlock(double bassDb, double midDb, double trebleDb)
    : bass_(EqFilterType::LowShelf, kBassFreqHz, bassDb),
      mid_(EqFilterType::Peaking, kMidFreqHz, midDb, kMidQ),
      treble_(EqFilterType::HighShelf, kTrebleFreqHz, trebleDb) {}

ToneStackBlock::ToneStackBlock(const ParamMap& params)
    : ToneStackBlock(paramAsDouble(params, "bass_db", 0.0), paramAsDouble(params, "mid_db", 0.0),
                      paramAsDouble(params, "treble_db", 0.0)) {}

void ToneStackBlock::setBassDb(double gainDb) { bass_.setGainDb(gainDb); }
void ToneStackBlock::setMidDb(double gainDb) { mid_.setGainDb(gainDb); }
void ToneStackBlock::setTrebleDb(double gainDb) { treble_.setGainDb(gainDb); }

void ToneStackBlock::prepare(double sampleRate) {
    bass_.prepare(sampleRate);
    mid_.prepare(sampleRate);
    treble_.prepare(sampleRate);
}

void ToneStackBlock::process(float* buffer, std::size_t numSamples) {
    bass_.process(buffer, numSamples);
    mid_.process(buffer, numSamples);
    treble_.process(buffer, numSamples);
}

void ToneStackBlock::reset() {
    bass_.reset();
    mid_.reset();
    treble_.reset();
}

}  // namespace audio_engine
