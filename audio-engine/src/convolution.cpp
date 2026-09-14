#include "audio_engine/convolution.hpp"

#include <algorithm>
#include <cmath>

#include "audio_engine/resample.hpp"
#include "audio_engine/wav_file.hpp"

namespace audio_engine {

ConvolutionEngine::ConvolutionEngine(std::vector<float> impulseResponse) { setImpulseResponse(std::move(impulseResponse)); }

void ConvolutionEngine::setImpulseResponse(std::vector<float> impulseResponse) {
    ir_ = std::move(impulseResponse);
    if (ir_.empty()) ir_.push_back(0.0f);
    reset();
}

void ConvolutionEngine::prepare(double /*sampleRate*/) { reset(); }

void ConvolutionEngine::reset() {
    // history_ holds the ir_.size()-1 most recent past input samples
    // (oldest first), so a fresh/just-switched-in IR starts from silence
    // rather than leaking samples from whatever was processed before it.
    history_.assign(ir_.size() > 0 ? ir_.size() - 1 : 0, 0.0f);
}

void ConvolutionEngine::process(float* buffer, std::size_t numSamples) {
    const std::size_t N = ir_.size();
    const std::size_t histLen = history_.size();  // == N - 1

    // extended = [history_ (oldest..newest), buffer (block, in order)]
    std::vector<float> extended;
    extended.reserve(histLen + numSamples);
    extended.insert(extended.end(), history_.begin(), history_.end());
    extended.insert(extended.end(), buffer, buffer + numSamples);

    std::vector<float> output(numSamples);
    for (std::size_t i = 0; i < numSamples; ++i) {
        double acc = 0.0;
        const std::size_t base = histLen + i;  // index of current sample in `extended`
        for (std::size_t k = 0; k < N; ++k) {
            // base - k is always >= 0 because base >= N-1 (i>=0, histLen==N-1).
            acc += static_cast<double>(ir_[k]) * static_cast<double>(extended[base - k]);
        }
        output[i] = static_cast<float>(acc);
    }

    std::copy(output.begin(), output.end(), buffer);

    if (histLen > 0) {
        history_.assign(extended.end() - static_cast<std::ptrdiff_t>(histLen), extended.end());
    }
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
