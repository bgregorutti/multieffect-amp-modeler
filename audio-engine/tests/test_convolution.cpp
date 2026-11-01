#include "audio_engine/convolution.hpp"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "audio_engine/wav_file.hpp"

using namespace audio_engine;

// Counts heap allocations while g_countAllocs is set, to prove process() is
// allocation-free (real-time safe). Replacing global operator new is
// program-wide, but it only counts; behaviour is unchanged.
namespace {
std::atomic<bool> g_countAllocs{false};
std::atomic<int> g_allocCount{0};
}  // namespace

// GCC flags malloc/free inside replaced operator new/delete as "mismatched"
// once inlined; that pairing is exactly what the replacement intends.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(std::size_t n) {
    if (g_countAllocs.load(std::memory_order_relaxed)) g_allocCount.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#pragma GCC diagnostic pop

namespace {

std::vector<float> randomSignal(std::size_t n, std::mt19937& rng, float amp = 0.5f) {
    std::uniform_real_distribution<float> u(-amp, amp);
    std::vector<float> v(n);
    for (float& s : v) s = u(rng);
    return v;
}

// Feeds `x` through `engine` in blocks cycling through `blockSizes`.
std::vector<float> streamThrough(ConvolutionEngine& engine, std::vector<float> x,
                                 const std::vector<std::size_t>& blockSizes) {
    std::size_t pos = 0, i = 0;
    while (pos < x.size()) {
        const std::size_t n = std::min(blockSizes[i++ % blockSizes.size()], x.size() - pos);
        engine.process(x.data() + pos, n);
        pos += n;
    }
    return x;
}

}  // namespace

TEST(Convolution, ImpulseIrIsIdentity) {
    ConvolutionEngine engine({1.0f});  // unit impulse IR: y[n] = x[n]
    engine.prepare(48000.0);

    std::vector<float> input = {0.1f, -0.2f, 0.3f, 0.0f, -0.9f, 1.0f};
    std::vector<float> expected = input;
    engine.process(input);
    // FFT round-off: exact zeros come back as ~1e-9, not bit-exact.
    for (size_t i = 0; i < input.size(); ++i) EXPECT_NEAR(input[i], expected[i], 1e-6f);
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
    EXPECT_NEAR(probe[0], 0.3f, 1e-6f);
    EXPECT_NEAR(probe[1], -0.4f, 1e-6f);
}

TEST(Convolution, LoadsImpulseResponseFromWavFile) {
    std::vector<float> irSamples = {1.0f, 0.5f, 0.25f};
    std::string path = "/tmp/audio_engine_test_ir.wav";
    writeWavFile(path, irSamples, 48000.0, WavSampleFormat::Float32);

    // Target rate matches the file's own rate -- no resampling should occur.
    // loadImpulseResponseFile also energy-normalizes (see
    // normalizeIrEnergy), so the loaded shape is proportional to the
    // source file, not identical to it -- check relative proportions and
    // that the result actually landed at unit L2 energy.
    std::vector<float> loaded = loadImpulseResponseFile(path, 48000.0);
    ASSERT_EQ(loaded.size(), irSamples.size());
    EXPECT_NEAR(loaded[1] / loaded[0], irSamples[1] / irSamples[0], 1e-5f);
    EXPECT_NEAR(loaded[2] / loaded[0], irSamples[2] / irSamples[0], 1e-5f);

    double sumSquares = 0.0;
    for (float s : loaded) sumSquares += static_cast<double>(s) * s;
    EXPECT_NEAR(std::sqrt(sumSquares), 1.0, 1e-5);
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

TEST(Convolution, NormalizeIrEnergyProducesUnitL2Norm) {
    // A deliberately "hot" IR (mirrors a real un-gain-staged room-capture
    // IR pack found during manual testing -- see README.md).
    std::vector<float> ir(500, 0.2f);
    std::vector<float> normalized = normalizeIrEnergy(ir);

    ASSERT_EQ(normalized.size(), ir.size());
    double sumSquares = 0.0;
    for (float s : normalized) sumSquares += static_cast<double>(s) * s;
    EXPECT_NEAR(std::sqrt(sumSquares), 1.0, 1e-5);

    // Shape (relative proportions) must be preserved -- normalization is a
    // uniform scale, not a reshape.
    EXPECT_NEAR(normalized[0], normalized[1], 1e-6f);
}

TEST(Convolution, NormalizeIrEnergyPreventsClippingOnHotIr) {
    // Reproduces the real bug: a hot IR (taps summing to a large L1 norm,
    // as measured against an actual "definitive speaker capture" pack)
    // convolved with a normal playing-level signal must not exceed
    // [-1, 1] once normalized -- see the header comment on
    // normalizeIrEnergy for the RMS-tracking property this relies on.
    std::vector<float> hotIr(2000, 0.05f);  // L1 norm = 100, deliberately hot
    std::vector<float> normalized = normalizeIrEnergy(hotIr);

    ConvolutionEngine eng(normalized);
    eng.prepare(48000.0);
    std::vector<float> signal(4000);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = 0.3f * std::sin(static_cast<float>(i) * 0.05f);  // moderate playing level
    }
    eng.process(signal.data(), signal.size());

    for (float s : signal) EXPECT_LE(std::fabs(s), 1.0f);
}

TEST(Convolution, NormalizeIrEnergyIsANoOpForSilentIr) {
    std::vector<float> silence(10, 0.0f);
    std::vector<float> result = normalizeIrEnergy(silence);
    EXPECT_EQ(result, silence);  // must not divide by ~0
}

// The FFT engine must reproduce the direct-form convolution (convolveFull,
// the old engine's algorithm) for any IR length -- shorter than, equal to,
// and not a multiple of the 64-sample partition -- and any host block size,
// including irregular ones.
TEST(Convolution, PartitionedFftMatchesDirectConvolution) {
    std::mt19937 rng(42);
    const std::vector<std::size_t> irLengths = {1, 7, 63, 64, 65, 1000, 8192};
    const std::vector<std::vector<std::size_t>> blockPatterns = {
        {1}, {63}, {64}, {256}, {1000}, {5, 64, 17, 128, 1, 300}};
    for (std::size_t irLen : irLengths) {
        std::vector<float> ir = normalizeIrEnergy(randomSignal(irLen, rng, 1.0f));
        std::vector<float> x = randomSignal(3 * 4096 + 37, rng);  // not a multiple of 64
        std::vector<float> ref = convolveFull(x, ir);
        ref.resize(x.size());
        for (const auto& pattern : blockPatterns) {
            ConvolutionEngine engine(ir);
            engine.prepare(48000.0);
            std::vector<float> y = streamThrough(engine, x, pattern);
            double maxErr = 0.0;
            for (std::size_t i = 0; i < y.size(); ++i) maxErr = std::max(maxErr, double(std::fabs(y[i] - ref[i])));
            EXPECT_LT(maxErr, 1e-5) << "irLen " << irLen << " first block " << pattern[0];
        }
    }
}

TEST(Convolution, ImpulseInGivesIrOut) {
    std::mt19937 rng(7);
    std::vector<float> ir = normalizeIrEnergy(randomSignal(5000, rng, 1.0f));
    ConvolutionEngine engine(ir);
    engine.prepare(48000.0);
    std::vector<float> x(6000, 0.0f);
    x[0] = 1.0f;
    std::vector<float> y = streamThrough(engine, x, {64, 13, 200});
    for (std::size_t i = 0; i < ir.size(); ++i) ASSERT_NEAR(y[i], ir[i], 1e-6f) << "index " << i;
    for (std::size_t i = ir.size(); i < y.size(); ++i) ASSERT_NEAR(y[i], 0.0f, 1e-6f) << "index " << i;
}

TEST(Convolution, ResetClearsHistoryButKeepsIr) {
    std::mt19937 rng(9);
    std::vector<float> ir = normalizeIrEnergy(randomSignal(700, rng, 1.0f));
    ConvolutionEngine engine(ir);
    engine.prepare(48000.0);
    std::vector<float> noise = randomSignal(1000, rng);
    engine.process(noise);  // leaves a tail in the convolver
    engine.reset();
    std::vector<float> x(800, 0.0f);
    x[0] = 1.0f;
    engine.process(x);
    for (std::size_t i = 0; i < ir.size(); ++i) ASSERT_NEAR(x[i], ir[i], 1e-6f) << "index " << i;
}

TEST(Convolution, SilentIrOutputsSilence) {
    ConvolutionEngine engine(std::vector<float>(300, 0.0f));
    engine.prepare(48000.0);
    std::mt19937 rng(3);
    std::vector<float> x = randomSignal(500, rng);
    engine.process(x);
    for (float s : x) EXPECT_EQ(s, 0.0f);
}

// Real-time safety: no heap allocation in the audio path (the old engine
// allocated two vectors per block).
TEST(Convolution, ProcessDoesNotAllocate) {
    std::mt19937 rng(5);
    ConvolutionEngine engine(normalizeIrEnergy(randomSignal(kMaxRealtimeIrSamples, rng, 1.0f)));
    engine.prepare(48000.0);
    std::vector<float> x = randomSignal(64 * 50, rng);
    g_allocCount = 0;
    g_countAllocs = true;
    for (std::size_t pos = 0; pos < x.size(); pos += 64) engine.process(x.data() + pos, 64);
    engine.process(x.data(), 17);
    engine.process(x.data(), 300);
    engine.reset();
    g_countAllocs = false;
    EXPECT_EQ(g_allocCount.load(), 0);
}
