#include "audio_engine/eq_block.hpp"

#include <cmath>
#include <stdexcept>

namespace audio_engine {

namespace {
double paramAsDouble(const ParamMap& params, const std::string& key, double fallback) {
    auto it = params.find(key);
    if (it == params.end()) return fallback;
    if (auto* d = std::get_if<double>(&it->second)) return *d;
    return fallback;
}

EqFilterType parseFilterType(const ParamMap& params) {
    auto it = params.find("filter_type");
    if (it == params.end()) return EqFilterType::Peaking;
    auto* s = std::get_if<std::string>(&it->second);
    if (!s) return EqFilterType::Peaking;
    if (*s == "low_shelf") return EqFilterType::LowShelf;
    if (*s == "high_shelf") return EqFilterType::HighShelf;
    return EqFilterType::Peaking;
}
}  // namespace

EqBlock::EqBlock(EqFilterType type, double freqHz, double gainDb, double q)
    : type_(type), freqHz_(freqHz), gainDb_(gainDb), q_(q) {
    recomputeCoefficients();
}

EqBlock::EqBlock(const ParamMap& params)
    : EqBlock(parseFilterType(params), paramAsDouble(params, "freq_hz", 1000.0),
              paramAsDouble(params, "gain_db", 0.0), paramAsDouble(params, "q", 0.70710678)) {}

void EqBlock::setGainDb(double gainDb) {
    gainDb_ = gainDb;
    recomputeCoefficients();
}

void EqBlock::prepare(double sampleRate) {
    sampleRate_ = sampleRate;
    recomputeCoefficients();
    reset();
}

void EqBlock::reset() {
    x1_ = x2_ = y1_ = y2_ = 0.0;
}

// RBJ Audio EQ Cookbook formulas (peaking EQ / shelving filters).
void EqBlock::recomputeCoefficients() {
    const double A = std::pow(10.0, gainDb_ / 40.0);
    const double w0 = 2.0 * M_PI * freqHz_ / sampleRate_;
    const double cosw0 = std::cos(w0);
    const double sinw0 = std::sin(w0);
    const double qClamped = q_ > 1e-6 ? q_ : 1e-6;
    const double alpha = sinw0 / (2.0 * qClamped);

    double b0, b1, b2, a0, a1, a2;

    switch (type_) {
        case EqFilterType::Peaking: {
            b0 = 1 + alpha * A;
            b1 = -2 * cosw0;
            b2 = 1 - alpha * A;
            a0 = 1 + alpha / A;
            a1 = -2 * cosw0;
            a2 = 1 - alpha / A;
            break;
        }
        case EqFilterType::LowShelf: {
            const double sqrtA = std::sqrt(A);
            b0 = A * ((A + 1) - (A - 1) * cosw0 + 2 * sqrtA * alpha);
            b1 = 2 * A * ((A - 1) - (A + 1) * cosw0);
            b2 = A * ((A + 1) - (A - 1) * cosw0 - 2 * sqrtA * alpha);
            a0 = (A + 1) + (A - 1) * cosw0 + 2 * sqrtA * alpha;
            a1 = -2 * ((A - 1) + (A + 1) * cosw0);
            a2 = (A + 1) + (A - 1) * cosw0 - 2 * sqrtA * alpha;
            break;
        }
        case EqFilterType::HighShelf: {
            const double sqrtA = std::sqrt(A);
            b0 = A * ((A + 1) + (A - 1) * cosw0 + 2 * sqrtA * alpha);
            b1 = -2 * A * ((A - 1) + (A + 1) * cosw0);
            b2 = A * ((A + 1) + (A - 1) * cosw0 - 2 * sqrtA * alpha);
            a0 = (A + 1) - (A - 1) * cosw0 + 2 * sqrtA * alpha;
            a1 = 2 * ((A - 1) - (A + 1) * cosw0);
            a2 = (A + 1) - (A - 1) * cosw0 - 2 * sqrtA * alpha;
            break;
        }
        default:
            throw std::logic_error("unreachable EqFilterType");
    }

    coeffs_.b0 = b0 / a0;
    coeffs_.b1 = b1 / a0;
    coeffs_.b2 = b2 / a0;
    coeffs_.a1 = a1 / a0;
    coeffs_.a2 = a2 / a0;
}

void EqBlock::process(float* buffer, std::size_t numSamples) {
    const double b0 = coeffs_.b0, b1 = coeffs_.b1, b2 = coeffs_.b2;
    const double a1 = coeffs_.a1, a2 = coeffs_.a2;
    for (std::size_t i = 0; i < numSamples; ++i) {
        const double x0 = static_cast<double>(buffer[i]);
        const double y0 = b0 * x0 + b1 * x1_ + b2 * x2_ - a1 * y1_ - a2 * y2_;
        x2_ = x1_;
        x1_ = x0;
        y2_ = y1_;
        y1_ = y0;
        buffer[i] = static_cast<float>(y0);
    }
}

}  // namespace audio_engine
