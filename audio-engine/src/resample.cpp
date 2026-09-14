#include "audio_engine/resample.hpp"

#include <algorithm>
#include <cmath>

namespace audio_engine {

std::vector<float> resampleLinear(const std::vector<float>& samples, double fromRate, double toRate) {
    if (samples.empty() || fromRate <= 0.0 || toRate <= 0.0) {
        return samples;
    }
    if (std::abs(fromRate - toRate) < 1e-6) {
        return samples;
    }

    const double ratio = fromRate / toRate;
    const auto outLen = static_cast<std::size_t>(std::llround(static_cast<double>(samples.size()) / ratio));
    std::vector<float> out(outLen);

    const std::size_t lastIndex = samples.size() - 1;
    for (std::size_t i = 0; i < outLen; ++i) {
        const double srcPos = static_cast<double>(i) * ratio;
        const auto idx0 = static_cast<std::size_t>(srcPos);
        const double frac = srcPos - static_cast<double>(idx0);
        const float s0 = samples[std::min(idx0, lastIndex)];
        const float s1 = samples[std::min(idx0 + 1, lastIndex)];
        out[i] = static_cast<float>(s0 + (s1 - s0) * frac);
    }
    return out;
}

}  // namespace audio_engine
