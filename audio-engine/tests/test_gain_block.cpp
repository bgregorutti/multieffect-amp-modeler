#include "audio_engine/gain_block.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

using namespace audio_engine;

TEST(GainBlock, UnityGainIsIdentity) {
    GainBlock block(0.0);
    block.prepare(48000.0);
    std::vector<float> buf = {0.1f, -0.5f, 1.0f, -1.0f, 0.0f};
    std::vector<float> expected = buf;
    block.process(buf);
    for (size_t i = 0; i < buf.size(); ++i) EXPECT_FLOAT_EQ(buf[i], expected[i]);
}

TEST(GainBlock, PlusSixDbRoughlyDoublesAmplitude) {
    GainBlock block(6.0206);  // 20*log10(2) ~= 6.0206 dB doubles amplitude
    block.prepare(48000.0);
    std::vector<float> buf = {0.25f};
    block.process(buf);
    EXPECT_NEAR(buf[0], 0.5f, 1e-3f);
}

TEST(GainBlock, MinusInfinityDbIsSilence) {
    GainBlock block(-200.0);
    block.prepare(48000.0);
    std::vector<float> buf = {1.0f, -1.0f, 0.5f};
    block.process(buf);
    for (float s : buf) EXPECT_NEAR(s, 0.0f, 1e-6f);
}

TEST(GainBlock, ConstructsFromParamsMap) {
    ParamMap params;
    params["gain_db"] = 0.0;
    GainBlock block(params);
    EXPECT_DOUBLE_EQ(block.gainDb(), 0.0);
    EXPECT_FLOAT_EQ(block.linearGain(), 1.0f);
}

TEST(GainBlock, DefaultsToUnityWhenParamMissing) {
    ParamMap params;  // no "gain_db" key
    GainBlock block(params);
    EXPECT_FLOAT_EQ(block.linearGain(), 1.0f);
}
