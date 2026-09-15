#include "audio_engine/delay_block.hpp"

#include <algorithm>
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

DelayBlock::DelayBlock(double delayMs, double feedback, double mix)
    : delayMs_(delayMs), feedback_(feedback), mix_(mix) {
    prepare(sampleRate_);
}

DelayBlock::DelayBlock(const ParamMap& params)
    : DelayBlock(paramAsDouble(params, "delay_ms", 300.0), paramAsDouble(params, "feedback", 0.3),
                 paramAsDouble(params, "mix", 0.5)) {}

void DelayBlock::prepare(double sampleRate) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000.0;
    std::size_t newDelaySamples =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::llround(delayMs_ / 1000.0 * sampleRate_)));
    delaySamples_ = newDelaySamples;
    buffer_.assign(delaySamples_, 0.0f);
    writeIndex_ = 0;
}

void DelayBlock::reset() {
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    writeIndex_ = 0;
}

void DelayBlock::setDelayMs(double delayMs) {
    delayMs_ = delayMs;
    prepare(sampleRate_);  // resizes buffer_ to match -- see the header comment on setLiveParam
}

void DelayBlock::setFeedback(double feedback) { feedback_ = feedback; }
void DelayBlock::setMix(double mix) { mix_ = mix; }

bool DelayBlock::setLiveParam(const std::string& key, double value) {
    if (key == "delay_ms") {
        setDelayMs(value);
        return true;
    }
    if (key == "feedback") {
        setFeedback(value);
        return true;
    }
    if (key == "mix") {
        setMix(value);
        return true;
    }
    return false;
}

void DelayBlock::process(float* buffer, std::size_t numSamples) {
    if (buffer_.empty()) return;
    const float fb = static_cast<float>(feedback_);
    const float mix = static_cast<float>(mix_);
    const std::size_t n = buffer_.size();
    for (std::size_t i = 0; i < numSamples; ++i) {
        const float x = buffer[i];
        const float delayed = buffer_[writeIndex_];
        buffer[i] = x * (1.0f - mix) + delayed * mix;
        buffer_[writeIndex_] = x + delayed * fb;
        writeIndex_ = (writeIndex_ + 1) % n;
    }
}

}  // namespace audio_engine
