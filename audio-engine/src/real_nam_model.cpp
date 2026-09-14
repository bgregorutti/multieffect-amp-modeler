#include "audio_engine/real_nam_model.hpp"

namespace audio_engine {

namespace {
// Generous upper bound for a single process() call's block size -- must
// cover the largest --block-size an operator could reasonably pass to
// audio_engine --audio (default 64, see audio_io_backend.hpp; this leaves
// well over an order of magnitude of headroom). Reset()'s maxBufferSize
// tells NeuralAmpModelerCore how large a buffer to expect internally.
constexpr int kMaxBufferSize = 8192;
}  // namespace

RealNamModel::RealNamModel(NamModelMetadata metadata, std::unique_ptr<nam::DSP> dsp)
    : metadata_(std::move(metadata)), dsp_(std::move(dsp)) {
    scratchIn_.resize(kMaxBufferSize);
    scratchOut_.resize(kMaxBufferSize);
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
        buffer[i] = static_cast<float>(scratchOut_[i]);
    }
}

}  // namespace audio_engine
