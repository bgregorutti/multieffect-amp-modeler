#include "audio_engine/tone_stack_block.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

using namespace audio_engine;

TEST(ToneStackBlock, AllZeroDbIsIdentity) {
    // Bass/mid/treble at 0dB each collapse to identity (same property
    // EqBlock.ZeroDbPeakingIsIdentity relies on for peaking, and the
    // equivalent A==1 collapse applies to the shelf bands too).
    ToneStackBlock ts;
    ts.prepare(48000.0);

    std::vector<float> input(64);
    for (size_t i = 0; i < input.size(); ++i) input[i] = std::sin(0.1f * static_cast<float>(i));
    std::vector<float> output = input;
    ts.process(output);

    for (size_t i = 0; i < input.size(); ++i) EXPECT_NEAR(output[i], input[i], 1e-3f);
}

TEST(ToneStackBlock, ConstructsFromParamsMap) {
    ParamMap params;
    params["bass_db"] = 6.0;
    params["mid_db"] = -3.0;
    params["treble_db"] = 4.0;
    ToneStackBlock ts(params);

    EXPECT_DOUBLE_EQ(ts.bassDb(), 6.0);
    EXPECT_DOUBLE_EQ(ts.midDb(), -3.0);
    EXPECT_DOUBLE_EQ(ts.trebleDb(), 4.0);
}

TEST(ToneStackBlock, DefaultsToZeroDbWhenParamsAbsent) {
    ToneStackBlock ts(ParamMap{});
    EXPECT_DOUBLE_EQ(ts.bassDb(), 0.0);
    EXPECT_DOUBLE_EQ(ts.midDb(), 0.0);
    EXPECT_DOUBLE_EQ(ts.trebleDb(), 0.0);
}

TEST(ToneStackBlock, LiveSettersUpdateGainWithoutReconstructing) {
    ToneStackBlock ts;
    ts.prepare(48000.0);

    ts.setBassDb(6.0);
    ts.setMidDb(-4.0);
    ts.setTrebleDb(3.0);

    EXPECT_DOUBLE_EQ(ts.bassDb(), 6.0);
    EXPECT_DOUBLE_EQ(ts.midDb(), -4.0);
    EXPECT_DOUBLE_EQ(ts.trebleDb(), 3.0);
}

TEST(ToneStackBlock, ImpulseResponseIsStableAndFinite) {
    ToneStackBlock ts(9.0, -9.0, 9.0);
    ts.prepare(48000.0);

    std::vector<float> buf(4096, 0.0f);
    buf[0] = 1.0f;
    ts.process(buf);

    for (float s : buf) ASSERT_TRUE(std::isfinite(s));
}

TEST(ToneStackBlock, ResetClearsHistory) {
    ToneStackBlock ts(6.0, 6.0, 6.0);
    ts.prepare(48000.0);
    std::vector<float> buf = {1.0f, 0.5f, -0.3f};
    ts.process(buf);
    ts.reset();

    ToneStackBlock fresh(6.0, 6.0, 6.0);
    fresh.prepare(48000.0);

    std::vector<float> probe = {0.2f};
    std::vector<float> probeAfterReset = probe;
    ts.process(probe);
    fresh.process(probeAfterReset);
    EXPECT_NEAR(probe[0], probeAfterReset[0], 1e-6f);
}
