#include "audio_engine/eq_block.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

using namespace audio_engine;

// RBJ cookbook property: a peaking filter with gain_db == 0 has A == 1,
// which makes its numerator and denominator coefficients identical before
// normalization -- i.e. the whole filter collapses to H(z) == 1 (pure
// identity), regardless of freq_hz/Q. This is a direct, closed-form check
// of the coefficient derivation itself, not just "it doesn't blow up".
TEST(EqBlock, ZeroDbPeakingIsIdentity) {
    EqBlock eq(EqFilterType::Peaking, /*freqHz=*/1000.0, /*gainDb=*/0.0, /*q=*/0.9);
    eq.prepare(48000.0);

    const auto& c = eq.coefficients();
    EXPECT_NEAR(c.b0, 1.0, 1e-9);
    EXPECT_NEAR(c.b1, c.a1, 1e-9);
    EXPECT_NEAR(c.b2, c.a2, 1e-9);

    std::vector<float> input(64);
    for (size_t i = 0; i < input.size(); ++i) input[i] = std::sin(0.1f * static_cast<float>(i));
    std::vector<float> output = input;
    eq.process(output);

    for (size_t i = 0; i < input.size(); ++i) EXPECT_NEAR(output[i], input[i], 1e-4f);
}

TEST(EqBlock, PositiveGainBoostsCenterFrequencyImpulseResponse) {
    EqBlock eq(EqFilterType::Peaking, 1000.0, /*gainDb=*/12.0, 0.7);
    eq.prepare(48000.0);
    // A boost filter's DC-normalized gain coefficient b0 should exceed 1
    // for a positive dB boost (more energy passed through at b0 tap).
    EXPECT_GT(eq.coefficients().b0, 1.0);
}

// Impulse response must decay towards zero and never produce NaN/Inf --
// i.e. the filter is stable for a reasonable range of params.
TEST(EqBlock, ImpulseResponseIsStableAndDecays) {
    for (double q : {0.3, 0.7071, 2.0, 5.0}) {
        for (double gainDb : {-18.0, -6.0, 0.0, 6.0, 18.0}) {
            EqBlock eq(EqFilterType::Peaking, 2000.0, gainDb, q);
            eq.prepare(48000.0);

            std::vector<float> buf(4096, 0.0f);
            buf[0] = 1.0f;
            eq.process(buf);

            for (float s : buf) {
                ASSERT_TRUE(std::isfinite(s)) << "q=" << q << " gainDb=" << gainDb;
            }

            // Energy in the back half of the buffer must be small relative
            // to the front half -- i.e. the impulse response has decayed,
            // not sustained or grown (which would indicate instability).
            double frontEnergy = 0.0, backEnergy = 0.0;
            for (size_t i = 0; i < buf.size() / 2; ++i) frontEnergy += static_cast<double>(buf[i]) * buf[i];
            for (size_t i = buf.size() / 2; i < buf.size(); ++i) backEnergy += static_cast<double>(buf[i]) * buf[i];
            EXPECT_LT(backEnergy, frontEnergy + 1e-6) << "q=" << q << " gainDb=" << gainDb;
            EXPECT_NEAR(backEnergy, 0.0, 1e-3) << "q=" << q << " gainDb=" << gainDb;
        }
    }
}

TEST(EqBlock, ShelfFiltersAreStable) {
    for (EqFilterType type : {EqFilterType::LowShelf, EqFilterType::HighShelf}) {
        EqBlock eq(type, 500.0, 9.0, 0.7071);
        eq.prepare(48000.0);
        std::vector<float> buf(2048, 0.0f);
        buf[0] = 1.0f;
        eq.process(buf);
        for (float s : buf) ASSERT_TRUE(std::isfinite(s));
    }
}

TEST(EqBlock, ResetClearsHistory) {
    EqBlock eq(EqFilterType::Peaking, 1000.0, 12.0, 0.7);
    eq.prepare(48000.0);
    std::vector<float> buf = {1.0f, 0.5f, -0.3f};
    eq.process(buf);
    eq.reset();

    EqBlock fresh(EqFilterType::Peaking, 1000.0, 12.0, 0.7);
    fresh.prepare(48000.0);
    std::vector<float> probe = {0.2f};
    std::vector<float> probeAfterReset = probe;
    eq.process(probe);
    fresh.process(probeAfterReset);
    EXPECT_NEAR(probe[0], probeAfterReset[0], 1e-6f);
}

TEST(EqBlock, ConstructsFromParamsMap) {
    ParamMap params;
    params["freq_hz"] = 500.0;
    params["gain_db"] = -3.0;
    params["q"] = 1.4;
    params["filter_type"] = std::string("high_shelf");
    EqBlock eq(params);
    eq.prepare(48000.0);
    std::vector<float> buf(256, 0.0f);
    buf[0] = 1.0f;
    eq.process(buf);
    for (float s : buf) ASSERT_TRUE(std::isfinite(s));
}
