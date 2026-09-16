// Unit tests for the shared pedal DSP primitives (pedal_dsp.hpp).
//
// Cross-language agreement with the Python reference is covered separately by
// test_python_parity.cpp; these cover the pieces in isolation, and the
// ParamSmoother contract the parity test depends on.

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "audio_engine/pedal_dsp.hpp"

using namespace audio_engine;

namespace {
constexpr double kSampleRate = 48000.0;

std::vector<double> sine(double freq, std::size_t numSamples, double amplitude = 1.0) {
    std::vector<double> out(numSamples);
    for (std::size_t i = 0; i < numSamples; ++i) {
        out[i] = amplitude * std::sin(2.0 * M_PI * freq * static_cast<double>(i) / kSampleRate);
    }
    return out;
}

// Peak of the back half of a signal, past any filter settling.
double settledPeak(const std::vector<double>& signal) {
    double peak = 0.0;
    for (std::size_t i = signal.size() / 2; i < signal.size(); ++i) {
        peak = std::max(peak, std::fabs(signal[i]));
    }
    return peak;
}

std::vector<double> run(OnePole& filter, const std::vector<double>& in) {
    std::vector<double> out(in.size());
    filter.process(in.data(), out.data(), in.size());
    return out;
}
}  // namespace

// --- softClip ----------------------------------------------------------

TEST(SoftClip, IsBoundedUnderExtremeDrive) {
    for (double x = -1000.0; x <= 1000.0; x += 7.3) {
        EXPECT_LE(std::fabs(softClip(x, 0.0)), 1.0);
        EXPECT_TRUE(std::isfinite(softClip(x, 0.15)));
    }
}

TEST(SoftClip, IsNearLinearForSmallSignals) {
    for (double x = -0.01; x <= 0.01; x += 0.001) {
        EXPECT_NEAR(softClip(x, 0.0), x, 1e-6);
    }
}

TEST(SoftClip, BiasBreaksSymmetryButKeepsSilenceSilent) {
    EXPECT_DOUBLE_EQ(softClip(0.0, 0.15), 0.0);
    // Symmetric without bias, asymmetric with it.
    EXPECT_NEAR(softClip(1.0, 0.0), -softClip(-1.0, 0.0), 1e-15);
    EXPECT_GT(std::fabs(softClip(1.0, 0.15) + softClip(-1.0, 0.15)), 1e-3);
}

// --- OnePole -----------------------------------------------------------

TEST(OnePoleFilter, LowpassPassesBelowAndBlocksAboveCutoff) {
    OnePole low;
    low.prepare(kSampleRate, 1000.0, OnePole::Mode::Lowpass);
    EXPECT_NEAR(settledPeak(run(low, sine(100.0, 12000))), 1.0, 0.02);

    OnePole high;
    high.prepare(kSampleRate, 1000.0, OnePole::Mode::Lowpass);
    EXPECT_LT(settledPeak(run(high, sine(10000.0, 12000))), 0.15);
}

TEST(OnePoleFilter, LowpassIsMinusThreeDbAtCutoff) {
    OnePole filter;
    filter.prepare(kSampleRate, 1000.0, OnePole::Mode::Lowpass);
    EXPECT_NEAR(settledPeak(run(filter, sine(1000.0, 12000))), 0.7071, 0.02);
}

TEST(OnePoleFilter, HighpassBlocksBelowAndPassesAboveCutoff) {
    OnePole low;
    low.prepare(kSampleRate, 1000.0, OnePole::Mode::Highpass);
    EXPECT_LT(settledPeak(run(low, sine(100.0, 12000))), 0.15);

    // A decade above the corner is within about half a dB of unity. A digital
    // one-pole highpass built as x - lowpass(x) never quite reaches unity near
    // Nyquist -- the lowpass output still carries phase there -- so this is a
    // floor rather than an equality.
    OnePole high;
    high.prepare(kSampleRate, 200.0, OnePole::Mode::Highpass);
    EXPECT_GT(settledPeak(run(high, sine(2000.0, 12000))), 0.95);
}

TEST(OnePoleFilter, ProcessSampleMatchesBlockProcessing) {
    const auto input = sine(700.0, 512);

    OnePole blockwise;
    blockwise.prepare(kSampleRate, 900.0, OnePole::Mode::Highpass);
    const auto expected = run(blockwise, input);

    OnePole sampleWise;
    sampleWise.prepare(kSampleRate, 900.0, OnePole::Mode::Highpass);
    for (std::size_t i = 0; i < input.size(); ++i) {
        EXPECT_NEAR(sampleWise.processSample(input[i]), expected[i], 1e-15);
    }
}

TEST(OnePoleFilter, ResetRestoresInitialState) {
    const auto input = sine(440.0, 256);
    OnePole filter;
    filter.prepare(kSampleRate, 800.0, OnePole::Mode::Lowpass);
    const auto first = run(filter, input);
    filter.reset();
    const auto second = run(filter, input);
    for (std::size_t i = 0; i < first.size(); ++i) EXPECT_DOUBLE_EQ(first[i], second[i]);
}

// --- Oversampler -------------------------------------------------------

TEST(OversamplerFilter, RoundTripPreservesAnAudibleSine) {
    Oversampler oversampler;
    oversampler.prepare(4);

    const auto input = sine(1000.0, 4096);
    std::vector<double> expanded(input.size() * 4);
    std::vector<double> output(input.size());
    oversampler.upsample(input.data(), expanded.data(), input.size());
    oversampler.downsample(expanded.data(), output.data(), input.size());

    const auto delay = static_cast<std::size_t>(oversampler.latencySamples());
    // Past the settling region, and loose enough for the interpolation
    // filter's passband ripple (~0.1% with firwin's default Hamming window).
    for (std::size_t i = input.size() / 2; i < input.size() - delay; ++i) {
        EXPECT_NEAR(output[i + delay], input[i], 3e-3);
    }
}

TEST(OversamplerFilter, ReportsAndDeliversItsLatency) {
    Oversampler oversampler;
    oversampler.prepare(4, 65);
    EXPECT_DOUBLE_EQ(oversampler.latencySamples(), 16.0);

    std::vector<double> impulse(256, 0.0);
    impulse[0] = 1.0;
    std::vector<double> expanded(impulse.size() * 4);
    std::vector<double> output(impulse.size());
    oversampler.upsample(impulse.data(), expanded.data(), impulse.size());
    oversampler.downsample(expanded.data(), output.data(), impulse.size());

    std::size_t peak = 0;
    for (std::size_t i = 0; i < output.size(); ++i) {
        if (std::fabs(output[i]) > std::fabs(output[peak])) peak = i;
    }
    EXPECT_EQ(peak, 16u);
}

TEST(OversamplerFilter, FactorOneIsAPassthrough) {
    Oversampler oversampler;
    oversampler.prepare(1);
    EXPECT_DOUBLE_EQ(oversampler.latencySamples(), 0.0);

    const auto input = sine(300.0, 128);
    std::vector<double> expanded(input.size());
    std::vector<double> output(input.size());
    oversampler.upsample(input.data(), expanded.data(), input.size());
    oversampler.downsample(expanded.data(), output.data(), input.size());
    for (std::size_t i = 0; i < input.size(); ++i) EXPECT_DOUBLE_EQ(output[i], input[i]);
}

TEST(OversamplerFilter, TapsSumToUnityAtDc) {
    Oversampler oversampler;
    oversampler.prepare(4);
    double sum = 0.0;
    for (double tap : oversampler.taps()) sum += tap;
    EXPECT_NEAR(sum, 1.0, 1e-12);
}

// --- ParamSmoother -----------------------------------------------------

// The property test_python_parity.cpp leans on: a snapped smoother is
// indistinguishable from no smoothing at all, forever, with no epsilon.
TEST(PedalParamSmoother, SnapIsExactAndStaysExact) {
    ParamSmoother smoother;
    smoother.prepare(kSampleRate);
    smoother.snap(0.37);

    EXPECT_FALSE(smoother.isSmoothing());
    for (int i = 0; i < 10000; ++i) EXPECT_DOUBLE_EQ(smoother.next(), 0.37);
}

TEST(PedalParamSmoother, SetTargetRampsRatherThanJumping) {
    ParamSmoother smoother;
    smoother.prepare(kSampleRate, 20.0);
    smoother.snap(0.0);
    smoother.setTarget(1.0);

    const double first = smoother.next();
    EXPECT_GT(first, 0.0);
    EXPECT_LT(first, 0.01) << "a 20 ms ramp must not arrive within one sample";
    EXPECT_TRUE(smoother.isSmoothing());
}

TEST(PedalParamSmoother, RampTerminatesExactlyOnTarget) {
    ParamSmoother smoother;
    smoother.prepare(kSampleRate, 5.0);
    smoother.snap(0.0);
    smoother.setTarget(1.0);

    // Well past the time constant: the ramp must land exactly, not approach
    // forever into denormals.
    for (int i = 0; i < 48000; ++i) smoother.next();
    EXPECT_DOUBLE_EQ(smoother.current(), 1.0);
    EXPECT_FALSE(smoother.isSmoothing());
}
