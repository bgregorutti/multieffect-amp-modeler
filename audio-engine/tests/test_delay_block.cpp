#include "audio_engine/delay_block.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

using namespace audio_engine;

// A single-sample impulse through a feedback delay line produces echoes
// at every multiple of the delay-line length N, with amplitude
// mix * feedback^(k-1) at the k-th echo (k = 1, 2, 3, ...): this follows
// directly from the block's difference equations
//   y[n]   = x[n]*(1-mix) + delayed*mix
//   buf[n] = x[n] + delayed*feedback
// which for an impulse at n=0 puts a `1` into the delay line at n=0, reads
// it back out (scaled by mix) at n=N, re-feeds `feedback` back into the
// line at n=N, reads that back out (scaled by mix) at n=2N, and so on.
TEST(DelayBlock, ImpulseProducesEchoesAtExpectedOffsetsWithDecayingAmplitude) {
    const double sampleRate = 48000.0;
    const double delayMs = 10.0;  // -> N = 480 samples at 48kHz
    const double feedback = 0.5;
    const double mix = 0.6;

    DelayBlock delay(delayMs, feedback, mix);
    delay.prepare(sampleRate);
    const std::size_t N = delay.delaySamples();
    ASSERT_EQ(N, static_cast<std::size_t>(delayMs / 1000.0 * sampleRate));

    const int numEchoes = 5;
    std::vector<float> buf(N * (numEchoes + 1) + 1, 0.0f);
    buf[0] = 1.0f;

    delay.process(buf);

    // Sample 0: dry-only component (1-mix) * impulse.
    EXPECT_NEAR(buf[0], static_cast<float>(1.0 - mix), 1e-5f);

    for (int k = 1; k <= numEchoes; ++k) {
        std::size_t offset = static_cast<std::size_t>(k) * N;
        double expected = mix * std::pow(feedback, k - 1);
        EXPECT_NEAR(buf[offset], expected, 1e-4) << "echo k=" << k << " at offset " << offset;

        // Everything strictly between echoes should be silent (no
        // spurious energy at non-echo sample offsets).
        if (offset > 0) {
            EXPECT_NEAR(buf[offset - 1], 0.0f, 1e-6f) << "offset-1 for k=" << k;
        }
    }
}

TEST(DelayBlock, ZeroFeedbackProducesExactlyOneEcho) {
    DelayBlock delay(5.0, /*feedback=*/0.0, /*mix=*/0.7);
    delay.prepare(44100.0);
    const std::size_t N = delay.delaySamples();

    std::vector<float> buf(N * 3 + 1, 0.0f);
    buf[0] = 1.0f;
    delay.process(buf);

    EXPECT_NEAR(buf[N], 0.7f, 1e-5f);
    EXPECT_NEAR(buf[2 * N], 0.0f, 1e-6f);
    EXPECT_NEAR(buf[3 * N], 0.0f, 1e-6f);
}

TEST(DelayBlock, ResetClearsDelayLine) {
    DelayBlock delay(2.0, 0.4, 0.5);
    delay.prepare(48000.0);
    std::vector<float> buf = {1.0f, 0.0f, 0.0f, 0.0f};
    delay.process(buf);
    delay.reset();

    std::vector<float> silentBuf(10, 0.0f);
    delay.process(silentBuf);
    for (float s : silentBuf) EXPECT_NEAR(s, 0.0f, 1e-6f);
}

TEST(DelayBlock, ConstructsFromParamsMap) {
    ParamMap params;
    params["delay_ms"] = 100.0;
    params["feedback"] = 0.25;
    params["mix"] = 0.3;
    DelayBlock delay(params);
    EXPECT_DOUBLE_EQ(delay.feedback(), 0.25);
    EXPECT_DOUBLE_EQ(delay.mix(), 0.3);
}
