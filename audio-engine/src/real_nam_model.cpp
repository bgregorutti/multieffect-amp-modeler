#include "audio_engine/real_nam_model.hpp"

#include <cmath>

namespace audio_engine {

namespace {
// Generous upper bound for a single process() call's block size -- must
// cover the largest --block-size an operator could reasonably pass to
// audio_engine --audio (default 64, see audio_io_backend.hpp; this leaves
// well over an order of magnitude of headroom). Reset()'s maxBufferSize
// tells NeuralAmpModelerCore how large a buffer to expect internally.
constexpr int kMaxBufferSize = 8192;

// Linear gain that brings a model reporting `modelLoudnessDb` (dB) up/down
// to kNamTargetLoudnessDb. Standard dB-to-linear-gain conversion; not a
// clipping guarantee by itself (see the defensive clamp in
// EngineChain::process, resource_manager.cpp) -- confirmed empirically
// safe with good headroom (~9dB) against real files under both moderate
// and aggressive picking, not just derived from the formula alone.
float loudnessNormalizationGain(double modelLoudnessDb) {
    return static_cast<float>(std::pow(10.0, (kNamTargetLoudnessDb - modelLoudnessDb) / 20.0));
}
}  // namespace

RealNamModel::RealNamModel(NamModelMetadata metadata, std::unique_ptr<nam::DSP> dsp)
    : metadata_(std::move(metadata)), dsp_(std::move(dsp)) {
    scratchIn_.resize(kMaxBufferSize);
    scratchOut_.resize(kMaxBufferSize);
    // Not every model reports a loudness (older exports may not) -- leave
    // outputGain_ at its default 1.0 (no-op) in that case rather than
    // guessing at a correction with nothing to base it on.
    if (dsp_->HasLoudness()) {
        outputGain_ = loudnessNormalizationGain(dsp_->GetLoudness());
    }
}

void RealNamModel::prepare(double sampleRate) { dsp_->Reset(sampleRate, kMaxBufferSize); }

void RealNamModel::process(float* buffer, std::size_t numSamples) {
    if (numSamples > scratchIn_.size()) {
        // Defensive only -- prepare() sizes for kMaxBufferSize, which
        // should cover any real-time block size in practice (see above).
        scratchIn_.resize(numSamples);
        scratchOut_.resize(numSamples);
    }

    for (std::size_t i = 0; i < numSamples; ++i) {
        scratchIn_[i] = static_cast<NAM_SAMPLE>(buffer[i]);
    }

    NAM_SAMPLE* inPtr = scratchIn_.data();
    NAM_SAMPLE* outPtr = scratchOut_.data();
    dsp_->process(&inPtr, &outPtr, static_cast<int>(numSamples));

    for (std::size_t i = 0; i < numSamples; ++i) {
        buffer[i] = static_cast<float>(scratchOut_[i]) * outputGain_;
    }
}

}  // namespace audio_engine
