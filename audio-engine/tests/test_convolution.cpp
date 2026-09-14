#include "audio_engine/convolution.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "audio_engine/wav_file.hpp"

using namespace audio_engine;

TEST(Convolution, ImpulseIrIsIdentity) {
    ConvolutionEngine engine({1.0f});  // unit impulse IR: y[n] = x[n]
    engine.prepare(48000.0);

    std::vector<float> input = {0.1f, -0.2f, 0.3f, 0.0f, -0.9f, 1.0f};
    std::vector<float> expected = input;
    engine.process(input);
    for (size_t i = 0; i < input.size(); ++i) EXPECT_FLOAT_EQ(input[i], expected[i]);
}

// Hand-computed two-tap convolution: ir = [a, b]; y[n] = a*x[n] + b*x[n-1].
TEST(Convolution, TwoTapIrMatchesHandComputedResult) {
    const float a = 0.5f, b = 0.25f;
    ConvolutionEngine engine({a, b});
    engine.prepare(48000.0);

    std::vector<float> x = {1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> y = x;
    engine.process(y);

    std::vector<float> expected = {
        a * 1.0f + b * 0.0f,  // x[-1] = 0 (silence before stream start)
        a * 2.0f + b * 1.0f,
        a * 3.0f + b * 2.0f,
        a * 4.0f + b * 3.0f,
    };
    for (size_t i = 0; i < y.size(); ++i) EXPECT_NEAR(y[i], expected[i], 1e-6f) << "index " << i;
}

TEST(Convolution, StreamingAcrossMultipleBlocksMatchesOneShot) {
    std::vector<float> ir = {0.6f, 0.3f, 0.1f};
    std::vector<float> x = {1.0f, 0.5f, -0.5f, 0.25f, -0.25f, 0.0f, 1.0f, -1.0f};

    ConvolutionEngine oneShot(ir);
    oneShot.prepare(48000.0);
    std::vector<float> oneShotOut = x;
    oneShot.process(oneShotOut);

    ConvolutionEngine streaming(ir);
    streaming.prepare(48000.0);
    std::vector<float> streamingOut;
    // Feed in small, irregular block sizes.
    std::vector<size_t> blockSizes = {1, 3, 2, 2};
    size_t pos = 0;
    for (size_t bs : blockSizes) {
        std::vector<float> block(x.begin() + pos, x.begin() + pos + bs);
        streaming.process(block);
        streamingOut.insert(streamingOut.end(), block.begin(), block.end());
        pos += bs;
    }

    ASSERT_EQ(streamingOut.size(), oneShotOut.size());
    for (size_t i = 0; i < oneShotOut.size(); ++i) {
        EXPECT_NEAR(streamingOut[i], oneShotOut[i], 1e-6f) << "index " << i;
    }
}

TEST(Convolution, ConvolveFullMatchesHandComputedExample) {
    std::vector<float> input = {1.0f, 2.0f, 3.0f};
    std::vector<float> ir = {1.0f, -1.0f};
    // Full convolution length = 3 + 2 - 1 = 4.
    // y[0] = 1*1 = 1
    // y[1] = 1*(-1) + 2*1 = 1
    // y[2] = 2*(-1) + 3*1 = 1
    // y[3] = 3*(-1) = -3
    std::vector<float> expected = {1.0f, 1.0f, 1.0f, -3.0f};
    std::vector<float> result = convolveFull(input, ir);
    ASSERT_EQ(result.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) EXPECT_NEAR(result[i], expected[i], 1e-6f);
}

TEST(Convolution, ResetClearsHistoryBetweenIrSwaps) {
    ConvolutionEngine engine({0.5f, 0.5f});
    engine.prepare(48000.0);
    std::vector<float> primer = {1.0f, 1.0f, 1.0f};
    engine.process(primer);  // leaves nonzero history

    engine.setImpulseResponse({1.0f});  // swap to identity IR; should reset history
    std::vector<float> probe = {0.3f, -0.4f};
    engine.process(probe);
    EXPECT_FLOAT_EQ(probe[0], 0.3f);
    EXPECT_FLOAT_EQ(probe[1], -0.4f);
}

TEST(Convolution, LoadsImpulseResponseFromWavFile) {
    std::vector<float> irSamples = {1.0f, 0.5f, 0.25f};
    std::string path = "/tmp/audio_engine_test_ir.wav";
    writeWavFile(path, irSamples, 48000.0, WavSampleFormat::Float32);

    // Target rate matches the file's own rate -- no resampling should occur.
    std::vector<float> loaded = loadImpulseResponseFile(path, 48000.0);
    ASSERT_EQ(loaded.size(), irSamples.size());
    for (size_t i = 0; i < irSamples.size(); ++i) EXPECT_NEAR(loaded[i], irSamples[i], 1e-6f);
}

TEST(Convolution, LoadsImpulseResponseFromWavFileResamplingToTargetRate) {
    // A 44.1kHz IR (the common real-world case -- see README.md "Sample
    // rate policy") loaded against a 48kHz engine must come back resampled,
    // not played back at the wrong rate.
    std::vector<float> irSamples(441, 0.0f);
    irSamples[0] = 1.0f;
    std::string path = "/tmp/audio_engine_test_ir_44100.wav";
    writeWavFile(path, irSamples, 44100.0, WavSampleFormat::Float32);

    std::vector<float> loaded = loadImpulseResponseFile(path, 48000.0);
    // 441 samples @ 44.1kHz is 10ms; @ 48kHz that's 480 samples.
    EXPECT_EQ(loaded.size(), 480u);
}

TEST(Convolution, LoadsImpulseResponseFromWavFileTruncatesOverlongIr) {
    // A real 34623-sample cabinet IR (see README.md "Sample rate policy")
    // takes ~156% of one real-time block's budget to convolve -- loading
    // it must come back capped, not full-length.
    std::vector<float> irSamples(kMaxRealtimeIrSamples + 5000, 0.01f);
    std::string path = "/tmp/audio_engine_test_ir_overlong.wav";
    writeWavFile(path, irSamples, 48000.0, WavSampleFormat::Float32);

    std::vector<float> loaded = loadImpulseResponseFile(path, 48000.0);
    EXPECT_EQ(loaded.size(), kMaxRealtimeIrSamples);
}

TEST(Convolution, TruncateIrWithFadeOutLeavesShortIrUnchanged) {
    std::vector<float> ir = {1.0f, 0.5f, 0.25f};
    std::vector<float> result = truncateIrWithFadeOut(ir, 8192);
    EXPECT_EQ(result, ir);
}

TEST(Convolution, TruncateIrWithFadeOutCutsToLengthAndEndsInSilence) {
    std::vector<float> ir(1000, 0.5f);
    std::vector<float> result = truncateIrWithFadeOut(ir, 300);

    ASSERT_EQ(result.size(), 300u);
    EXPECT_FLOAT_EQ(result[0], 0.5f);           // untouched, well before the fade window
    EXPECT_FLOAT_EQ(result.back(), 0.0f);       // exactly silent at the cut -- no click
    // Strictly decreasing (not just non-increasing) across the fade window.
    for (std::size_t i = 300 - 256 + 1; i < 300; ++i) {
        EXPECT_LT(result[i], result[i - 1]);
    }
}
