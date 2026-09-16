#include "audio_engine/noise_gate_block.hpp"

#include <algorithm>
#include <cmath>

namespace audio_engine {

namespace {
// Peak-follower decay, mirroring vst-python/src/pedals/noisegate.py. Set by
// the lowest note the pedal has to survive, not by taste: the follower tracks
// abs(x), so peaks arrive at twice the note's frequency, and a low E at 82 Hz
// refreshes every 6.1 ms. The envelope then sags exp(-6.1/25) between peaks --
// about 2.1 dB, comfortably inside the 6 dB default hysteresis. At a 10 ms
// decay the same note sags 5.3 dB, close enough to the window to chatter.
constexpr double kEnvelopeDecayMs = 25.0;

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

double paramAsDouble(const ParamMap& params, const std::string& key, double fallback) {
    auto it = params.find(key);
    if (it == params.end()) return fallback;
    if (auto* d = std::get_if<double>(&it->second)) return *d;
    if (auto* b = std::get_if<bool>(&it->second)) return *b ? 1.0 : 0.0;
    return fallback;
}

// exp(-1/samples), guarding against a zero or negative time producing a
// coefficient of 1 (a smoother that never moves) or a NaN.
double timeToCoef(double milliseconds, double sampleRate) {
    const double samples = std::max(milliseconds * 1.0e-3 * sampleRate, 1.0e-9);
    return std::exp(-1.0 / samples);
}
}  // namespace

NoiseGateBlock::NoiseGateBlock() : NoiseGateBlock(Settings{}) {}

NoiseGateBlock::NoiseGateBlock(const Settings& settings) : settings_(settings) {
    prepare(sampleRate_);
}

NoiseGateBlock::NoiseGateBlock(const ParamMap& params) {
    Settings s;
    s.thresholdDb = paramAsDouble(params, "threshold_db", s.thresholdDb);
    s.rangeDb = paramAsDouble(params, "range_db", s.rangeDb);
    s.attackMs = paramAsDouble(params, "attack_ms", s.attackMs);
    s.holdMs = paramAsDouble(params, "hold_ms", s.holdMs);
    s.releaseMs = paramAsDouble(params, "release_ms", s.releaseMs);
    s.hysteresisDb = paramAsDouble(params, "hysteresis_db", s.hysteresisDb);
    s.bypass = paramAsDouble(params, "bypass", 0.0) != 0.0;
    settings_ = s;
    prepare(sampleRate_);
}

void NoiseGateBlock::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    recomputeCoefficients();
    reset();
}

void NoiseGateBlock::recomputeCoefficients() {
    envelopeDecay_ = timeToCoef(kEnvelopeDecayMs, sampleRate_);
    attackCoef_ = timeToCoef(settings_.attackMs, sampleRate_);
    releaseCoef_ = timeToCoef(settings_.releaseMs, sampleRate_);
    openThreshold_ = dbToGain(settings_.thresholdDb);
    closeThreshold_ = dbToGain(settings_.thresholdDb - settings_.hysteresisDb);
    floorGain_ = dbToGain(settings_.rangeDb);
    holdSamples_ = static_cast<long>(settings_.holdMs * 1.0e-3 * sampleRate_);
}

void NoiseGateBlock::reset() {
    envelope_ = 0.0;
    isOpen_ = false;
    holdCounter_ = 0;
    gain_ = floorGain_;
}

void NoiseGateBlock::setSettings(const Settings& settings) {
    settings_ = settings;
    recomputeCoefficients();
}

bool NoiseGateBlock::setLiveParam(const std::string& key, double value) {
    if (key == "threshold_db") {
        settings_.thresholdDb = value;
    } else if (key == "range_db") {
        settings_.rangeDb = value;
    } else if (key == "attack_ms") {
        settings_.attackMs = value;
    } else if (key == "hold_ms") {
        settings_.holdMs = value;
    } else if (key == "release_ms") {
        settings_.releaseMs = value;
    } else if (key == "hysteresis_db") {
        settings_.hysteresisDb = value;
    } else if (key == "bypass") {
        settings_.bypass = value != 0.0;
        return true;  // no coefficients depend on it
    } else {
        return false;
    }
    recomputeCoefficients();
    return true;
}

void NoiseGateBlock::process(float* buffer, std::size_t numSamples) {
    if (settings_.bypass || numSamples == 0) return;

    // Per-sample: the envelope's coefficient depends on the signal and the
    // state machine on its own history, so neither can be expressed as a
    // fixed filter. No allocation, no branching on block size.
    for (std::size_t i = 0; i < numSamples; ++i) {
        const double x = static_cast<double>(buffer[i]);
        const double level = std::fabs(x);

        // Peak follower: instant attack, exponential decay.
        envelope_ = level > envelope_ ? level : envelope_ * envelopeDecay_;

        if (envelope_ > openThreshold_) {
            isOpen_ = true;
            holdCounter_ = holdSamples_;
        } else if (isOpen_) {
            if (envelope_ < closeThreshold_) {
                if (holdCounter_ > 0) {
                    --holdCounter_;
                } else {
                    isOpen_ = false;
                }
            } else {
                // Inside the hysteresis band: still committed to staying open,
                // so keep the hold budget topped up.
                holdCounter_ = holdSamples_;
            }
        }

        const double target = isOpen_ ? 1.0 : floorGain_;
        const double coef = target > gain_ ? attackCoef_ : releaseCoef_;
        gain_ = target + (gain_ - target) * coef;
        buffer[i] = static_cast<float>(x * gain_);
    }
}

}  // namespace audio_engine
