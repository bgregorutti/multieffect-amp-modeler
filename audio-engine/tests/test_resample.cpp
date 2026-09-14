#include "audio_engine/resample.hpp"

#include <gtest/gtest.h>

using namespace audio_engine;

TEST(Resample, MatchingRatesReturnsSamplesUnchanged) {
    std::vector<float> samples = {0.1f, 0.2f, 0.3f, 0.4f};
    std::vector<float> result = resampleLinear(samples, 48000.0, 48000.0);
    EXPECT_EQ(result, samples);
}

TEST(Resample, EmptyInputReturnsEmpty) {
    EXPECT_TRUE(resampleLinear({}, 44100.0, 48000.0).empty());
}

TEST(Resample, NonPositiveRateReturnsInputUnchanged) {
    std::vector<float> samples = {1.0f, 2.0f};
    EXPECT_EQ(resampleLinear(samples, 0.0, 48000.0), samples);
    EXPECT_EQ(resampleLinear(samples, 48000.0, 0.0), samples);
}

TEST(Resample, UpsamplingProducesExpectedLength) {
    // 100 samples @ 44100 Hz -> ~108.8ms @ 48000 Hz.
    std::vector<float> samples(100, 0.0f);
    std::vector<float> result = resampleLinear(samples, 44100.0, 48000.0);
    EXPECT_NEAR(static_cast<double>(result.size()), 100.0 * 48000.0 / 44100.0, 1.0);
}

TEST(Resample, DownsamplingProducesExpectedLength) {
    std::vector<float> samples(200, 0.0f);
    std::vector<float> result = resampleLinear(samples, 48000.0, 44100.0);
    EXPECT_NEAR(static_cast<double>(result.size()), 200.0 * 44100.0 / 48000.0, 1.0);
}

TEST(Resample, LinearlyInterpolatesARamp) {
    // A perfect linear ramp resampled with linear interpolation should
    // still be (approximately) a linear ramp over the same value range.
    std::vector<float> ramp;
    for (int i = 0; i < 10; ++i) ramp.push_back(static_cast<float>(i));

    std::vector<float> result = resampleLinear(ramp, 10.0, 20.0);
    ASSERT_GE(result.size(), 2u);
    EXPECT_NEAR(result.front(), 0.0f, 1e-4f);
    // Values should be monotonically non-decreasing, matching the ramp.
    for (std::size_t i = 1; i < result.size(); ++i) {
        EXPECT_GE(result[i], result[i - 1] - 1e-4f);
    }
}
