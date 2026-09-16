#include "audio_engine/pedal_dsp.hpp"

#include <algorithm>
#include <cmath>

namespace audio_engine {

namespace {
constexpr double kPi = 3.14159265358979323846;

// numpy's sinc: sin(pi*x)/(pi*x), and exactly 1 at zero.
double sinc(double x) noexcept {
    if (x == 0.0) return 1.0;
    const double px = kPi * x;
    return std::sin(px) / px;
}
}  // namespace

double softClip(double x, double bias) noexcept {
    if (bias == 0.0) return std::tanh(x);
    return std::tanh(x + bias) - std::tanh(bias);
}

// --- OnePole -----------------------------------------------------------

void OnePole::prepare(double sampleRate, double cutoffHz, Mode mode) {
    mode_ = mode;
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    // Clamp rather than reject: a block constructed before prepare() supplies
    // a real rate should not be able to produce an unstable coefficient.
    const double cutoff = std::clamp(cutoffHz, 1.0e-4, rate * 0.5 - 1.0);
    a_ = 1.0 - std::exp(-2.0 * kPi * cutoff / rate);
    state_ = 0.0;
}

double OnePole::processSample(double x) noexcept {
    state_ += a_ * (x - state_);
    return mode_ == Mode::Lowpass ? state_ : x - state_;
}

void OnePole::process(const double* in, double* out, std::size_t numSamples) noexcept {
    if (mode_ == Mode::Lowpass) {
        for (std::size_t i = 0; i < numSamples; ++i) {
            state_ += a_ * (in[i] - state_);
            out[i] = state_;
        }
    } else {
        for (std::size_t i = 0; i < numSamples; ++i) {
            const double x = in[i];
            state_ += a_ * (x - state_);
            out[i] = x - state_;
        }
    }
}

// --- Oversampler -------------------------------------------------------

void Oversampler::prepare(int factor, std::size_t numTaps, double transition) {
    factor_ = std::max(1, factor);
    if (factor_ == 1) {
        taps_.assign(1, 1.0);
    } else {
        // scipy.signal.firwin(numTaps, transition / factor) with its default
        // symmetric Hamming window and DC normalization. Recomputed here
        // rather than pasted in as 65 constants so the design is legible --
        // test_python_parity.cpp asserts these against the exported taps, so a
        // divergence surfaces as its own failure rather than a wrong waveform.
        if (numTaps % 2 == 0) ++numTaps;
        const double cutoff = transition / static_cast<double>(factor_);
        const double centre = static_cast<double>(numTaps - 1) / 2.0;
        taps_.assign(numTaps, 0.0);
        double sum = 0.0;
        for (std::size_t i = 0; i < numTaps; ++i) {
            const double m = static_cast<double>(i) - centre;
            const double window =
                0.54 - 0.46 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                       static_cast<double>(numTaps - 1));
            taps_[i] = cutoff * sinc(cutoff * m) * window;
            sum += taps_[i];
        }
        for (double& tap : taps_) tap /= sum;  // unity gain at DC
    }

    const std::size_t historySize = taps_.size() - 1;
    upHistory_.assign(historySize, 0.0);
    downHistory_.assign(historySize, 0.0);
    // Sized once, here: process() never resizes these.
    stuffed_.assign(kPedalMaxChunk * static_cast<std::size_t>(factor_), 0.0);
    upScratch_.assign(historySize + kPedalMaxChunk * static_cast<std::size_t>(factor_), 0.0);
    downScratch_.assign(historySize + kPedalMaxChunk * static_cast<std::size_t>(factor_), 0.0);
}

void Oversampler::reset() {
    std::fill(upHistory_.begin(), upHistory_.end(), 0.0);
    std::fill(downHistory_.begin(), downHistory_.end(), 0.0);
}

double Oversampler::latencySamples() const {
    if (factor_ == 1) return 0.0;
    // Each of the two FIRs contributes (numTaps-1)/2 at the oversampled rate.
    return static_cast<double>(taps_.size() - 1) / static_cast<double>(factor_);
}

void Oversampler::filter(const double* in, double* out, std::size_t n,
                         std::vector<double>& history, std::vector<double>& scratch) noexcept {
    const std::size_t historySize = history.size();
    const std::size_t numTaps = taps_.size();
    // Lay the carried-over tail ahead of this chunk so the convolution can
    // read straight back across the block boundary.
    std::copy(history.begin(), history.end(), scratch.begin());
    std::copy(in, in + n, scratch.begin() + static_cast<std::ptrdiff_t>(historySize));

    for (std::size_t i = 0; i < n; ++i) {
        double acc = 0.0;
        const double* window = scratch.data() + i + historySize;
        for (std::size_t k = 0; k < numTaps; ++k) acc += taps_[k] * window[-static_cast<std::ptrdiff_t>(k)];
        out[i] = acc;
    }

    const std::size_t total = historySize + n;
    std::copy(scratch.begin() + static_cast<std::ptrdiff_t>(total - historySize),
              scratch.begin() + static_cast<std::ptrdiff_t>(total), history.begin());
}

void Oversampler::upsample(const double* in, double* out, std::size_t numSamples) noexcept {
    if (factor_ == 1) {
        std::copy(in, in + numSamples, out);
        return;
    }
    // The scratch buffers are sized for kPedalMaxChunk, so anything longer is
    // split here rather than trusting the caller to do it. The pedal blocks
    // already chunk their own buffers, but this class is public and a direct
    // caller passing a longer block used to run straight off the end of
    // stuffed_ -- silent heap corruption that happened to look like passband
    // error. Splitting is exact: both directions carry their filter state
    // across the boundary.
    for (std::size_t offset = 0; offset < numSamples; offset += kPedalMaxChunk) {
        const std::size_t n = std::min(kPedalMaxChunk, numSamples - offset);
        upsampleChunk(in + offset, out + offset * static_cast<std::size_t>(factor_), n);
    }
}

void Oversampler::upsampleChunk(const double* in, double* out, std::size_t numSamples) noexcept {
    const std::size_t expanded = numSamples * static_cast<std::size_t>(factor_);
    std::fill(stuffed_.begin(), stuffed_.begin() + static_cast<std::ptrdiff_t>(expanded), 0.0);
    // Zero-stuffing spreads the energy across `factor` samples, so the
    // interpolation filter needs that gain handed back.
    const double gain = static_cast<double>(factor_);
    for (std::size_t i = 0; i < numSamples; ++i) {
        stuffed_[i * static_cast<std::size_t>(factor_)] = in[i] * gain;
    }
    filter(stuffed_.data(), out, expanded, upHistory_, upScratch_);
}

void Oversampler::downsample(const double* in, double* out, std::size_t numSamples) noexcept {
    if (factor_ == 1) {
        std::copy(in, in + numSamples, out);
        return;
    }
    for (std::size_t offset = 0; offset < numSamples; offset += kPedalMaxChunk) {
        const std::size_t n = std::min(kPedalMaxChunk, numSamples - offset);
        downsampleChunk(in + offset * static_cast<std::size_t>(factor_), out + offset, n);
    }
}

void Oversampler::downsampleChunk(const double* in, double* out, std::size_t numSamples) noexcept {
    const std::size_t expanded = numSamples * static_cast<std::size_t>(factor_);
    // Filter in place over the oversampled scratch, then keep every factor-th.
    filter(in, stuffed_.data(), expanded, downHistory_, downScratch_);
    for (std::size_t i = 0; i < numSamples; ++i) {
        out[i] = stuffed_[i * static_cast<std::size_t>(factor_)];
    }
}

// --- ParamSmoother -----------------------------------------------------

void ParamSmoother::prepare(double sampleRate, double rampMs) {
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double samples = std::max(1.0, rampMs * 1.0e-3 * rate);
    coef_ = 1.0 - std::exp(-1.0 / samples);
}

}  // namespace audio_engine
