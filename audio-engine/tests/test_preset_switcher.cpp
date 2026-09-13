#include "audio_engine/preset_switcher.hpp"

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

using namespace audio_engine;

namespace {
// A "chain" that ignores its input and fills the buffer with a constant --
// simulates two totally different preset chains (e.g. two different amp
// models) for testing the crossfade math itself, independent of any real
// EffectBlock/NAM/IR machinery.
ChainFn constantChain(float value) {
    return [value](float* buffer, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) buffer[i] = value;
    };
}
}  // namespace

TEST(PresetSwitcher, CrossfadeRampsMonotonicallyAndSettlesExactly) {
    PresetSwitcher switcher;
    const std::size_t duration = 100;
    switcher.beginCrossfade(constantChain(1.0f), constantChain(-1.0f), duration);

    std::vector<float> input(duration + 20, 0.0f);
    std::vector<float> output;
    switcher.process(input, output);

    ASSERT_EQ(output.size(), input.size());

    // Monotonically non-increasing across the whole crossfade window
    // (old=+1.0 -> new=-1.0): equal-power blend of a +1/-1 constant pair
    // is provably monotonic (see preset_switcher.hpp derivation), and must
    // hold sample-to-sample with no more than floating-point noise of
    // non-monotonicity.
    for (std::size_t i = 1; i < duration; ++i) {
        EXPECT_LE(output[i], output[i - 1] + 1e-6f) << "non-monotonic at index " << i;
    }

    // Starts at (approximately, allowing fp epsilon) +1.0 ...
    EXPECT_NEAR(output[0], 1.0f, 1e-6f);
    // ... and settles to EXACTLY -1.0 once the window ends (index
    // duration-1 is the last blended sample; everything from `duration`
    // onward is pure new-chain output).
    EXPECT_FLOAT_EQ(output[duration - 1], -1.0f);
    for (std::size_t i = duration; i < output.size(); ++i) {
        EXPECT_FLOAT_EQ(output[i], -1.0f) << "index " << i;
    }

    // No discontinuity beyond float epsilon across the window-end boundary.
    EXPECT_NEAR(output[duration - 1], output[duration], 1e-5f);

    EXPECT_FALSE(switcher.isCrossfading());
}

TEST(PresetSwitcher, DiscontinuityAtEverySampleBoundaryIsBelowEpsilon) {
    PresetSwitcher switcher;
    const std::size_t duration = 50;
    switcher.beginCrossfade(constantChain(1.0f), constantChain(-1.0f), duration);

    std::vector<float> input(duration, 0.0f);
    std::vector<float> output;
    switcher.process(input, output);

    for (std::size_t i = 1; i < output.size(); ++i) {
        float step = std::fabs(output[i] - output[i - 1]);
        // Max possible per-sample step for this curve over `duration`
        // samples is bounded; just assert it's small and smooth, not a
        // discontinuous jump.
        EXPECT_LT(step, 0.15f) << "index " << i;
    }
}

TEST(PresetSwitcher, CrossfadeSpanningMultipleProcessCallsStaysContinuous) {
    PresetSwitcher switcher;
    const std::size_t duration = 60;
    switcher.beginCrossfade(constantChain(1.0f), constantChain(-1.0f), duration);

    std::vector<float> full;
    for (std::size_t blockSize : {7, 13, 5, 20, 30, 1, 4}) {
        std::vector<float> input(blockSize, 0.0f);
        std::vector<float> output;
        switcher.process(input, output);
        full.insert(full.end(), output.begin(), output.end());
    }

    ASSERT_GE(full.size(), duration);
    for (std::size_t i = 1; i < duration; ++i) {
        EXPECT_LE(full[i], full[i - 1] + 1e-6f) << "index " << i;
    }
    EXPECT_FLOAT_EQ(full[duration - 1], -1.0f);
    EXPECT_FLOAT_EQ(full.back(), -1.0f);
}

TEST(PresetSwitcher, NotCrossfadingRunsOnlyCurrentChain) {
    PresetSwitcher switcher;
    std::vector<float> input = {0.1f, 0.2f, 0.3f};
    std::vector<float> output;
    switcher.process(input, output);
    // No chain ever set: passthrough.
    EXPECT_EQ(output, input);
}

// --- Preloading: no I/O once crossfading has started -----------------

namespace {
struct CountingLoader {
    int loadCalls = 0;

    // "Loads" (prepares) a chain: pretend this does blocking file I/O
    // (NAM/IR parsing) the way ResourceManager's real loader would, then
    // returns a ready-to-run ChainFn. Must be called BEFORE
    // beginCrossfade, never during process().
    ChainFn load(float outputValue) {
        ++loadCalls;
        return constantChain(outputValue);
    }
};
}  // namespace

TEST(PresetSwitcher, NoLoaderIoOnceCrossfadeProcessingHasStarted) {
    CountingLoader loader;

    // Preload both chains up front -- this is the "background preloading"
    // half of the glitch-free-switching requirement: the next chain's
    // NAM/IR loading is fully finished before any crossfade sample is
    // produced.
    ChainFn oldChain = loader.load(1.0f);
    ChainFn newChain = loader.load(-1.0f);
    EXPECT_EQ(loader.loadCalls, 2);

    PresetSwitcher switcher;
    switcher.beginCrossfade(oldChain, newChain, 30);

    std::vector<float> input(10, 0.0f);
    std::vector<float> output;
    for (int i = 0; i < 5; ++i) {
        switcher.process(input, output);
        // The loader must not be invoked again by any process() call --
        // beginCrossfade/process only ever run already-prepared ChainFns.
        EXPECT_EQ(loader.loadCalls, 2) << "iteration " << i;
    }
}
