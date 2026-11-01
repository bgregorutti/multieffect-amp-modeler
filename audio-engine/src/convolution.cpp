#include "audio_engine/convolution.hpp"

#include <algorithm>
#include <cmath>

#include "FFTConvolver.h"
#include "audio_engine/resample.hpp"
#include "audio_engine/wav_file.hpp"

namespace audio_engine {

ConvolutionEngine::ConvolutionEngine(std::vector<float> impulseResponse) { setImpulseResponse(std::move(impulseResponse)); }

ConvolutionEngine::~ConvolutionEngine() = default;

void ConvolutionEngine::setImpulseResponse(std::vector<float> impulseResponse) {
    ir_ = std::move(impulseResponse);
    if (ir_.empty()) ir_.push_back(0.0f);
    // All allocation (partition spectra, FFT tables) happens here, at IR
    // load time on the control thread -- never in process().
    auto conv = std::make_unique<fftconvolver::FFTConvolver>();
    conv->init(kConvolutionPartitionSize, ir_.data(), ir_.size());
    conv_ = std::move(conv);
}

void ConvolutionEngine::prepare(double /*sampleRate*/) { reset(); }

void ConvolutionEngine::reset() {
    // Clears the input history / overlap so a fresh or just-switched-in IR
    // starts from silence. Keeps the IR spectra: no allocation.
    if (conv_) conv_->resetState();
}

void ConvolutionEngine::process(float* buffer, std::size_t numSamples) {
    if (!conv_) return;
    // FFTConvolver copies each input chunk into its own buffer before it
    // writes the matching output chunk, so in-place processing is safe.
    conv_->process(buffer, buffer, numSamples);
}

std::vector<float> truncateIrWithFadeOut(std::vector<float> ir, std::size_t maxSamples) {
    if (ir.size() <= maxSamples) {
        return ir;
    }
    ir.resize(maxSamples);

    const std::size_t fadeLen = std::min<std::size_t>(maxSamples, 256);
    if (fadeLen >= 2) {
        for (std::size_t i = 0; i < fadeLen; ++i) {
            // t sweeps 0 -> 1 across the fade window, so gain sweeps
            // 1 -> 0 and lands on exactly silence at the very last sample
            // -- no discontinuity at the cut point.
            const double t = static_cast<double>(i) / static_cast<double>(fadeLen - 1);
            const float gain = static_cast<float>(1.0 - t);
            ir[maxSamples - fadeLen + i] *= gain;
        }
    }
    return ir;
}

std::vector<float> normalizeIrEnergy(std::vector<float> ir) {
    double sumSquares = 0.0;
    for (float s : ir) {
        sumSquares += static_cast<double>(s) * static_cast<double>(s);
    }
    const double l2 = std::sqrt(sumSquares);
    if (l2 < 1e-9) {
        return ir;  // silent/degenerate IR -- nothing to normalize against
    }
    const float scale = static_cast<float>(1.0 / l2);
    for (float& s : ir) {
        s *= scale;
    }
    return ir;
}

std::vector<float> loadImpulseResponseFile(const std::string& path, double targetSampleRate) {
    WavData wav = parseWavFile(path);
    std::vector<float> resampled = resampleLinear(wav.samples, wav.sampleRate, targetSampleRate);
    std::vector<float> capped = truncateIrWithFadeOut(std::move(resampled), kMaxRealtimeIrSamples);
    return normalizeIrEnergy(std::move(capped));
}

std::vector<float> convolveFull(const std::vector<float>& input, const std::vector<float>& ir) {
    if (input.empty() || ir.empty()) return {};
    const std::size_t outLen = input.size() + ir.size() - 1;
    std::vector<float> output(outLen, 0.0f);
    for (std::size_t n = 0; n < input.size(); ++n) {
        if (input[n] == 0.0f) continue;
        for (std::size_t k = 0; k < ir.size(); ++k) {
            output[n + k] += input[n] * ir[k];
        }
    }
    return output;
}

}  // namespace audio_engine
