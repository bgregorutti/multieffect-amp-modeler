#include "audio_engine/tube_screamer_block.hpp"

#include <algorithm>
#include <cmath>

namespace audio_engine {

namespace {
// Mirrors vst-python/src/pedals/tubescreamer.py. Unlike the Big Muff's, most
// of these come from the TS808's actual component values rather than being
// fitted by ear.
constexpr double kInputHpHz = 20.0;
constexpr double kDriveHpHz = 720.0;  // 1 / (2*pi * 4.7k * 0.047uF)
// Stage gain is 1 + (51k + drive_pot) / 4.7k with a 500k pot: 11.9x to 118x.
// Note the minimum is still ~21 dB -- a Tube Screamer is always driving the
// diodes somewhat, which is why its Drive knob has a narrower audible range
// than a Big Muff's Sustain.
constexpr double kDriveMinDb = 21.5;
constexpr double kDriveMaxDb = 41.4;
constexpr double kDiodeClamp = 0.6;
constexpr double kClipBias = 0.05;  // near-symmetric: two matched silicon diodes
constexpr double kToneFixedLpHz = 5500.0;
constexpr double kToneDarkLpHz = 1200.0;
constexpr double kDcBlockerHz = 10.0;

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

double paramAsDouble(const ParamMap& params, const std::string& key, double fallback) {
    auto it = params.find(key);
    if (it == params.end()) return fallback;
    if (auto* d = std::get_if<double>(&it->second)) return *d;
    if (auto* b = std::get_if<bool>(&it->second)) return *b ? 1.0 : 0.0;
    return fallback;
}
}  // namespace

TubeScreamerBlock::TubeScreamerBlock(double drive, double tone, double level, bool bypass)
    : drive_(std::clamp(drive, 0.0, 1.0)),
      tone_(std::clamp(tone, 0.0, 1.0)),
      level_(std::clamp(level, 0.0, 1.0)),
      bypass_(bypass) {
    prepare(sampleRate_);
}

TubeScreamerBlock::TubeScreamerBlock(const ParamMap& params)
    : TubeScreamerBlock(paramAsDouble(params, "drive", 0.5), paramAsDouble(params, "tone", 0.5),
                        paramAsDouble(params, "level", 0.5),
                        paramAsDouble(params, "bypass", 0.0) != 0.0) {}

void TubeScreamerBlock::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double osRate = sampleRate_ * oversample_;

    oversampler_.prepare(oversample_);
    inputHp_.prepare(sampleRate_, kInputHpHz, OnePole::Mode::Highpass);
    // Runs at the oversampled rate, alongside the clipper it feeds.
    driveHp_.prepare(osRate, kDriveHpHz, OnePole::Mode::Highpass);
    dcBlocker_.prepare(sampleRate_, kDcBlockerHz, OnePole::Mode::Highpass);
    fixedLp_.prepare(sampleRate_, kToneFixedLpHz, OnePole::Mode::Lowpass);
    toneDarkLp_.prepare(sampleRate_, kToneDarkLpHz, OnePole::Mode::Lowpass);

    driveGain_.prepare(sampleRate_);
    toneBlend_.prepare(sampleRate_);
    levelGain_.prepare(sampleRate_);
    snapSmoothers();

    base_.assign(kPedalMaxChunk, 0.0);
    oversampled_.assign(kPedalMaxChunk * static_cast<std::size_t>(oversample_), 0.0);
    boost_.assign(kPedalMaxChunk * static_cast<std::size_t>(oversample_), 0.0);
}

void TubeScreamerBlock::snapSmoothers() {
    driveGain_.snap(dbToGain(kDriveMinDb + drive_ * (kDriveMaxDb - kDriveMinDb)));
    toneBlend_.snap(tone_);
    levelGain_.snap(level_ * level_);
}

void TubeScreamerBlock::reset() {
    oversampler_.reset();
    inputHp_.reset();
    driveHp_.reset();
    dcBlocker_.reset();
    fixedLp_.reset();
    toneDarkLp_.reset();
    snapSmoothers();
}

void TubeScreamerBlock::setDrive(double value) {
    drive_ = std::clamp(value, 0.0, 1.0);
    driveGain_.setTarget(dbToGain(kDriveMinDb + drive_ * (kDriveMaxDb - kDriveMinDb)));
}

void TubeScreamerBlock::setTone(double value) {
    tone_ = std::clamp(value, 0.0, 1.0);
    toneBlend_.setTarget(tone_);
}

void TubeScreamerBlock::setLevel(double value) {
    level_ = std::clamp(value, 0.0, 1.0);
    levelGain_.setTarget(level_ * level_);
}

bool TubeScreamerBlock::setLiveParam(const std::string& key, double value) {
    if (key == "drive") {
        setDrive(value);
        return true;
    }
    if (key == "tone") {
        setTone(value);
        return true;
    }
    if (key == "level") {
        setLevel(value);
        return true;
    }
    if (key == "bypass") {
        setBypass(value != 0.0);
        return true;
    }
    return false;
}

void TubeScreamerBlock::process(float* buffer, std::size_t numSamples) {
    if (bypass_ || numSamples == 0) return;
    for (std::size_t offset = 0; offset < numSamples; offset += kPedalMaxChunk) {
        processChunk(buffer + offset, std::min(kPedalMaxChunk, numSamples - offset));
    }
}

void TubeScreamerBlock::processChunk(float* buffer, std::size_t n) noexcept {
    const std::size_t expanded = n * static_cast<std::size_t>(oversample_);
    const int factor = oversample_;

    for (std::size_t i = 0; i < n; ++i) {
        base_[i] = inputHp_.processSample(static_cast<double>(buffer[i]));
    }

    oversampler_.upsample(base_.data(), oversampled_.data(), n);
    driveHp_.process(oversampled_.data(), boost_.data(), expanded);

    // Drive is advanced once per *base-rate* sample and held across that
    // sample's oversampled group: the smoother's time constant is defined at
    // the base rate, and a gain is a scalar, so scaling before or after the
    // linear filtering above is equivalent.
    for (std::size_t i = 0, k = 0; i < n; ++i) {
        const double gain = driveGain_.next();
        for (int j = 0; j < factor; ++j, ++k) boost_[k] *= gain;
    }

    // The non-inverting stage: the clipped, frequency-shaped boost is added
    // *to* the dry signal rather than replacing it.
    for (std::size_t k = 0; k < expanded; ++k) {
        oversampled_[k] += softClip(boost_[k] / kDiodeClamp, kClipBias) * kDiodeClamp;
    }
    oversampler_.downsample(oversampled_.data(), base_.data(), n);

    dcBlocker_.process(base_.data(), base_.data(), n);
    fixedLp_.process(base_.data(), base_.data(), n);

    for (std::size_t i = 0; i < n; ++i) {
        const double x = base_[i];
        const double blend = toneBlend_.next();
        const double dark = toneDarkLp_.processSample(x);
        buffer[i] = static_cast<float>(((1.0 - blend) * dark + blend * x) * levelGain_.next());
    }
}

}  // namespace audio_engine
