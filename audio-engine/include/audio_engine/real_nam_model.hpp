// Real NAM (WaveNet/LSTM/etc.) inference, backed by the vendored
// NeuralAmpModelerCore library (github.com/sdatkinson/NeuralAmpModelerCore,
// MIT-licensed). Only compiled when the CMake option
// AUDIO_ENGINE_WITH_REAL_NAM is on (default OFF) -- see
// audio-engine/README.md "Real NAM inference" and CMakeLists.txt. This
// header is therefore only ever included behind that same #ifdef, so it's
// safe to include NeuralAmpModelerCore's own headers directly.
//
// Handles whatever architecture the vendored library itself supports
// (WaveNet, LSTM, and container/multi-submodel formats like
// "SlimmableContainer") via its own nam::get_dsp() factory -- this class
// is a thin adapter, not a reimplementation of any model-specific logic.
#pragma once

#include <memory>
#include <vector>

#include <NAM/dsp.h>

#include "audio_engine/nam_model.hpp"

namespace audio_engine {

// Target loudness (dB, same scale as nam::DSP::GetLoudness()) every real
// NAM model's output is normalized to when the model reports one -- see
// real_nam_model.cpp's applyLoudnessNormalization and the "NAM output
// loudness normalization" section of README.md for how this number was
// chosen. Different .nam files are not gain-consistent with each other
// any more than cabinet IRs are (see convolution.hpp's normalizeIrEnergy
// for the same problem on the IR side) -- confirmed with a real file: one
// commercial "gain stage" export measured ~6dB hotter than its siblings
// and clipped audibly with zero compensation.
inline constexpr double kNamTargetLoudnessDb = -22.0;

class RealNamModel : public INamModel {
public:
    using EffectBlock::process;  // bring the std::vector<float>& convenience overload back into scope

    RealNamModel(NamModelMetadata metadata, std::unique_ptr<nam::DSP> dsp);

    const NamModelMetadata& metadata() const override { return metadata_; }

    // Calls nam::DSP::Reset(sampleRate, maxBufferSize), which (by default)
    // also prewarms the model -- settles dilated-conv/recurrent history so
    // the first real audio block doesn't start from a "cold" state. This
    // can be relatively expensive (see README.md "Real NAM inference" for
    // measured cost) -- expected to run once per preset load, not per
    // audio block.
    void prepare(double sampleRate) override;

    // Converts our mono in-place float buffer to/from NAM_SAMPLE (double
    // by default) scratch buffers and calls nam::DSP::process. Real-time
    // caveat: the scratch buffers only grow (never shrink) on a
    // larger-than-expected call, which would allocate -- expected not to
    // happen in practice since prepare() sizes them generously (see
    // real_nam_model.cpp), but this is not a hard real-time guarantee the
    // way the rest of the DSP chain is.
    void process(float* buffer, std::size_t numSamples) override;

private:
    NamModelMetadata metadata_;
    std::unique_ptr<nam::DSP> dsp_;
    std::vector<NAM_SAMPLE> scratchIn_;
    std::vector<NAM_SAMPLE> scratchOut_;
    // Computed once in the constructor from dsp_->GetLoudness() (1.0f, a
    // no-op multiply, if the model doesn't report one). See
    // kNamTargetLoudnessDb above.
    float outputGain_ = 1.0f;
};

}  // namespace audio_engine
