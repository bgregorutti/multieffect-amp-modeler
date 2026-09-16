#include "audio_engine/big_muff_block.hpp"

#include <algorithm>
#include <cmath>

namespace audio_engine {

namespace {
// Circuit constants. These mirror vst-python/src/pedals/bigmuff.py exactly --
// they are the A/B tuning surface, and changing one here without changing it
// there (and regenerating the golden fixture) breaks the parity test.
constexpr double kPadDb = -15.0;
constexpr double kInputHpHz = 30.0;
constexpr double kStageHpHz = 80.0;   // interstage coupling caps
constexpr double kStageLpHz = 6000.0;  // cap across each stage's feedback diodes
constexpr double kStageGainDb = 14.0;
constexpr double kStageBias = 0.15;  // clipping asymmetry -> even harmonics
constexpr double kSustainMinDb = -18.0;
constexpr double kSustainMaxDb = 30.0;
constexpr double kToneLpHz = 400.0;    // bass branch
constexpr double kToneHpHz = 2000.0;   // treble branch; above the bass corner,
                                        // which is what creates the mid scoop
constexpr double kDcBlockerHz = 10.0;

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

double paramAsDouble(const ParamMap& params, const std::string& key, double fallback) {
    auto it = params.find(key);
    if (it == params.end()) return fallback;
    if (auto* d = std::get_if<double>(&it->second)) return *d;
    if (auto* b = std::get_if<bool>(&it->second)) return *b ? 1.0 : 0.0;
    return fallback;
}

bool paramAsBool(const ParamMap& params, const std::string& key, bool fallback) {
    return paramAsDouble(params, key, fallback ? 1.0 : 0.0) != 0.0;
}
}  // namespace

BigMuffBlock::BigMuffBlock(double sustain, double tone, double volume, bool pad15dB, bool bypass)
    : sustain_(std::clamp(sustain, 0.0, 1.0)),
      tone_(std::clamp(tone, 0.0, 1.0)),
      volume_(std::clamp(volume, 0.0, 1.0)),
      pad15dB_(pad15dB),
      bypass_(bypass) {
    prepare(sampleRate_);
}

BigMuffBlock::BigMuffBlock(const ParamMap& params)
    : BigMuffBlock(paramAsDouble(params, "sustain", 0.7), paramAsDouble(params, "tone", 0.5),
                   paramAsDouble(params, "volume", 0.5), paramAsBool(params, "pad_15db", false),
                   paramAsBool(params, "bypass", false)) {}

void BigMuffBlock::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double osRate = sampleRate_ * oversample_;

    oversampler_.prepare(oversample_);
    inputHp_.prepare(sampleRate_, kInputHpHz, OnePole::Mode::Highpass);
    // The clipping stages run oversampled, so their filters must be built at
    // that rate -- at the base rate their corners would land a factor of
    // `oversample` too high.
    for (auto& hp : stageHp_) hp.prepare(osRate, kStageHpHz, OnePole::Mode::Highpass);
    for (auto& lp : stageLp_) lp.prepare(osRate, kStageLpHz, OnePole::Mode::Lowpass);
    dcBlocker_.prepare(sampleRate_, kDcBlockerHz, OnePole::Mode::Highpass);
    toneLp_.prepare(sampleRate_, kToneLpHz, OnePole::Mode::Lowpass);
    toneHp_.prepare(sampleRate_, kToneHpHz, OnePole::Mode::Highpass);

    padGain_.prepare(sampleRate_);
    sustainGain_.prepare(sampleRate_);
    toneBlend_.prepare(sampleRate_);
    volumeGain_.prepare(sampleRate_);
    snapSmoothers();

    base_.assign(kPedalMaxChunk, 0.0);
    oversampled_.assign(kPedalMaxChunk * static_cast<std::size_t>(oversample_), 0.0);
}

void BigMuffBlock::snapSmoothers() {
    padGain_.snap(pad15dB_ ? dbToGain(kPadDb) : 1.0);
    sustainGain_.snap(dbToGain(kSustainMinDb + sustain_ * (kSustainMaxDb - kSustainMinDb)));
    toneBlend_.snap(tone_);
    // A squared taper: the knob's useful range sits in its upper half, as on
    // the real pedal.
    volumeGain_.snap(volume_ * volume_);
}

void BigMuffBlock::reset() {
    oversampler_.reset();
    inputHp_.reset();
    for (auto& hp : stageHp_) hp.reset();
    for (auto& lp : stageLp_) lp.reset();
    dcBlocker_.reset();
    toneLp_.reset();
    toneHp_.reset();
    snapSmoothers();
}

void BigMuffBlock::setSustain(double value) {
    sustain_ = std::clamp(value, 0.0, 1.0);
    sustainGain_.setTarget(dbToGain(kSustainMinDb + sustain_ * (kSustainMaxDb - kSustainMinDb)));
}

void BigMuffBlock::setTone(double value) {
    tone_ = std::clamp(value, 0.0, 1.0);
    toneBlend_.setTarget(tone_);
}

void BigMuffBlock::setVolume(double value) {
    volume_ = std::clamp(value, 0.0, 1.0);
    volumeGain_.setTarget(volume_ * volume_);
}

void BigMuffBlock::setPad15dB(bool enabled) {
    pad15dB_ = enabled;
    padGain_.setTarget(enabled ? dbToGain(kPadDb) : 1.0);
}

bool BigMuffBlock::setLiveParam(const std::string& key, double value) {
    if (key == "sustain") {
        setSustain(value);
        return true;
    }
    if (key == "tone") {
        setTone(value);
        return true;
    }
    if (key == "volume") {
        setVolume(value);
        return true;
    }
    if (key == "pad_15db") {
        setPad15dB(value != 0.0);
        return true;
    }
    if (key == "bypass") {
        setBypass(value != 0.0);
        return true;
    }
    return false;
}

void BigMuffBlock::process(float* buffer, std::size_t numSamples) {
    // Bypass leaves the signal and the filter state alone, matching the
    // reference implementation's behaviour exactly.
    if (bypass_ || numSamples == 0) return;
    for (std::size_t offset = 0; offset < numSamples; offset += kPedalMaxChunk) {
        processChunk(buffer + offset, std::min(kPedalMaxChunk, numSamples - offset));
    }
}

void BigMuffBlock::processChunk(float* buffer, std::size_t n) noexcept {
    const std::size_t expanded = n * static_cast<std::size_t>(oversample_);
    const double stageGain = dbToGain(kStageGainDb);

    for (std::size_t i = 0; i < n; ++i) {
        const double padded = static_cast<double>(buffer[i]) * padGain_.next();
        base_[i] = inputHp_.processSample(padded) * sustainGain_.next();
    }

    oversampler_.upsample(base_.data(), oversampled_.data(), n);
    for (int stage = 0; stage < 2; ++stage) {
        stageHp_[stage].process(oversampled_.data(), oversampled_.data(), expanded);
        for (std::size_t k = 0; k < expanded; ++k) {
            oversampled_[k] = softClip(oversampled_[k] * stageGain, kStageBias);
        }
        stageLp_[stage].process(oversampled_.data(), oversampled_.data(), expanded);
    }
    oversampler_.downsample(oversampled_.data(), base_.data(), n);

    // The asymmetric clipper leaves a DC offset the tone stack would
    // otherwise pass straight to the output.
    dcBlocker_.process(base_.data(), base_.data(), n);

    for (std::size_t i = 0; i < n; ++i) {
        const double x = base_[i];
        const double blend = toneBlend_.next();
        const double low = toneLp_.processSample(x);
        const double high = toneHp_.processSample(x);
        buffer[i] = static_cast<float>(((1.0 - blend) * low + blend * high) * volumeGain_.next());
    }
}

}  // namespace audio_engine
