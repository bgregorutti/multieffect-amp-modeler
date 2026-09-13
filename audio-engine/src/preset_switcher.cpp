#include "audio_engine/preset_switcher.hpp"

#include <cmath>

namespace audio_engine {

void PresetSwitcher::beginCrossfade(ChainFn oldChain, ChainFn newChain, std::size_t durationSamples) {
    oldChain_ = std::move(oldChain);
    newChain_ = std::move(newChain);
    duration_ = durationSamples == 0 ? 1 : durationSamples;
    position_ = 0;
    crossfading_ = true;
}

void PresetSwitcher::process(const std::vector<float>& input, std::vector<float>& output) {
    output.resize(input.size());

    if (!crossfading_) {
        output = input;
        if (newChain_) newChain_(output.data(), output.size());
        return;
    }

    oldScratch_ = input;
    newScratch_ = input;
    if (oldChain_) oldChain_(oldScratch_.data(), oldScratch_.size());
    if (newChain_) newChain_(newScratch_.data(), newScratch_.size());

    constexpr double kHalfPi = M_PI / 2.0;

    for (std::size_t i = 0; i < input.size(); ++i) {
        if (!crossfading_) {
            // Window already completed mid-block (see below): remaining
            // samples in this same block are pure new-chain output.
            output[i] = newScratch_[i];
            continue;
        }

        const double t = (duration_ <= 1) ? 1.0 : static_cast<double>(position_) / static_cast<double>(duration_ - 1);
        const double gainOld = std::cos(t * kHalfPi);
        const double gainNew = std::sin(t * kHalfPi);
        output[i] = static_cast<float>(gainOld * oldScratch_[i] + gainNew * newScratch_[i]);

        ++position_;
        if (position_ >= duration_) {
            crossfading_ = false;  // window complete; future process() calls use newChain_ only
        }
    }
}

}  // namespace audio_engine
