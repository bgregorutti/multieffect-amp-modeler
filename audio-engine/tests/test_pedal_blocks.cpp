// Block-level tests for the modelled pedals.
//
// Circuit behaviour (mid scoop, midrange hump, gating thresholds) is asserted
// in the Python reference's own suite and pinned here by
// test_python_parity.cpp, so these cover what the port adds on top: engine
// plumbing (registry, factory, live params) and the parameter smoothing the
// Python models deliberately do not have.

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "audio_engine/big_muff_block.hpp"
#include "audio_engine/block_type_registry.hpp"
#include "audio_engine/noise_gate_block.hpp"
#include "audio_engine/resource_manager.hpp"
#include "audio_engine/tube_screamer_block.hpp"

using namespace audio_engine;

namespace {
constexpr double kSampleRate = 48000.0;

std::vector<float> sine(double freq, std::size_t numSamples, double amplitude = 0.3) {
    std::vector<float> out(numSamples);
    for (std::size_t i = 0; i < numSamples; ++i) {
        out[i] = static_cast<float>(
            amplitude * std::sin(2.0 * M_PI * freq * static_cast<double>(i) / kSampleRate));
    }
    return out;
}

double rms(const std::vector<float>& signal, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) sum += static_cast<double>(signal[i]) * signal[i];
    return std::sqrt(sum / static_cast<double>(to - from));
}

// Largest single-sample step in a signal -- a click detector.
double maxSlew(const std::vector<float>& signal, std::size_t from, std::size_t to) {
    double worst = 0.0;
    for (std::size_t i = from + 1; i < to; ++i) {
        worst = std::max(worst, std::fabs(static_cast<double>(signal[i]) - signal[i - 1]));
    }
    return worst;
}

std::unique_ptr<EffectBlock> makeBlock(const std::string& type, const ParamMap& params = {}) {
    EffectBlockSpec spec;
    spec.id = "test";
    spec.type = type;
    spec.params = params;
    auto block = createEffectBlock(spec);
    block->prepare(kSampleRate);
    return block;
}

const std::vector<std::string> kPedalTypes = {"big_muff", "tube_screamer", "noise_gate"};
}  // namespace

// --- Engine plumbing ---------------------------------------------------

TEST(PedalBlockRegistry, ListsEveryPedalType) {
    const auto types = listNativeBlockTypes();
    for (const auto& wanted : kPedalTypes) {
        const bool found = std::any_of(types.begin(), types.end(), [&](const auto& t) {
            return t.type == wanted;
        });
        EXPECT_TRUE(found) << wanted << " missing from listNativeBlockTypes()";
    }
}

// block_type_registry.hpp is explicit that a parameter it advertises which
// setLiveParam then rejects is a real bug. This checks both directions of
// that contract for every new type at once.
TEST(PedalBlockRegistry, EveryAdvertisedParameterIsLiveSettable) {
    for (const auto& descriptor : listNativeBlockTypes()) {
        if (std::find(kPedalTypes.begin(), kPedalTypes.end(), descriptor.type) ==
            kPedalTypes.end()) {
            continue;
        }
        auto block = makeBlock(descriptor.type);
        for (const auto& parameter : descriptor.parameters) {
            EXPECT_TRUE(block->setLiveParam(parameter.key, parameter.defaultValue))
                << descriptor.type << " rejects advertised parameter " << parameter.key;
        }
        EXPECT_FALSE(block->setLiveParam("not_a_real_parameter", 1.0))
            << descriptor.type << " accepts an unknown parameter";
    }
}

// A preset block already carries `enabled`, which the engine honours by
// skipping the block entirely. Advertising "bypass" as well would put a second
// off-switch on every pedal card in the app, next to the real toggle -- so the
// blocks understand the key but the registry must not offer it.
TEST(PedalBlockRegistry, DoesNotAdvertiseBypassAsAParameter) {
    for (const auto& descriptor : listNativeBlockTypes()) {
        for (const auto& parameter : descriptor.parameters) {
            EXPECT_NE(parameter.key, "bypass")
                << descriptor.type << " advertises bypass; use the block's `enabled` flag instead";
        }
    }
    // Still understood directly, which is what the bypass tests below rely on.
    EXPECT_TRUE(makeBlock("big_muff")->setLiveParam("bypass", 1.0));
}

TEST(PedalBlockFactory, BuildsRealBlocksNotPassthrough) {
    auto signal = sine(220.0, 4096);
    for (const auto& type : kPedalTypes) {
        auto reference = signal;
        auto block = makeBlock(type, {{"threshold_db", -10.0}});  // gate: shut on this signal
        auto processed = signal;
        block->process(processed.data(), processed.size());

        const bool changed = !std::equal(processed.begin(), processed.end(), reference.begin());
        EXPECT_TRUE(changed) << type << " left the signal untouched -- fell back to passthrough?";
    }
}

TEST(PedalBlockFactory, DefaultConstructedParamsMatchTheRegistry) {
    BigMuffBlock muff;
    EXPECT_DOUBLE_EQ(muff.sustain(), 0.7);
    EXPECT_DOUBLE_EQ(muff.tone(), 0.5);
    EXPECT_DOUBLE_EQ(muff.volume(), 0.5);
    EXPECT_FALSE(muff.pad15dB());

    TubeScreamerBlock screamer;
    EXPECT_DOUBLE_EQ(screamer.drive(), 0.5);
    EXPECT_DOUBLE_EQ(screamer.tone(), 0.5);
    EXPECT_DOUBLE_EQ(screamer.level(), 0.5);

    NoiseGateBlock gate;
    EXPECT_DOUBLE_EQ(gate.settings().thresholdDb, -45.0);
    EXPECT_DOUBLE_EQ(gate.settings().rangeDb, -60.0);
}

TEST(PedalBlocks, BypassIsAnExactPassthrough) {
    const auto input = sine(330.0, 2048);
    for (const auto& type : kPedalTypes) {
        auto block = makeBlock(type, {{"bypass", true}});
        auto buffer = input;
        block->process(buffer.data(), buffer.size());
        EXPECT_EQ(buffer, input) << type << " altered the signal while bypassed";
    }
}

TEST(PedalBlocks, SilenceInSilenceOut) {
    for (const auto& type : kPedalTypes) {
        auto block = makeBlock(type);
        std::vector<float> buffer(4096, 0.0f);
        block->process(buffer.data(), buffer.size());
        for (float sample : buffer) EXPECT_NEAR(sample, 0.0f, 1e-9f) << type;
    }
}

TEST(PedalBlocks, OutputStaysFiniteUnderExtremeInput) {
    for (const auto& type : kPedalTypes) {
        auto block = makeBlock(type);
        auto buffer = sine(440.0, 4096, 50.0);
        block->process(buffer.data(), buffer.size());
        for (float sample : buffer) EXPECT_TRUE(std::isfinite(sample)) << type;
    }
}

TEST(PedalBlocks, ReportTheirLatency) {
    EXPECT_DOUBLE_EQ(BigMuffBlock{}.latencySamples(), 16.0);
    EXPECT_DOUBLE_EQ(TubeScreamerBlock{}.latencySamples(), 16.0);
    // Gate detection is causal: no lookahead, so nothing to declare.
    EXPECT_DOUBLE_EQ(NoiseGateBlock{}.latencySamples(), 0.0);
}

TEST(PedalBlocks, ResetRestoresInitialState) {
    const auto input = sine(196.0, 2048);
    for (const auto& type : kPedalTypes) {
        auto block = makeBlock(type);
        auto first = input;
        block->process(first.data(), first.size());

        block->reset();
        auto second = input;
        block->process(second.data(), second.size());
        EXPECT_EQ(first, second) << type << " did not fully reset";
    }
}

// --- Parameter smoothing -----------------------------------------------
//
// The Python reference deliberately applies parameters per block with no ramp,
// which keeps its renders exactly reproducible. A real-time engine cannot do
// that: a gain stepping discontinuously mid-buffer clicks. These blocks
// therefore ramp on setLiveParam() while snapping on construction/prepare/
// reset, which is what keeps test_python_parity.cpp valid.

TEST(PedalParameterSmoothing, LiveChangeConvergesToTheConstructedValue) {
    const auto input = sine(110.0, 24000);

    // Reference: built at the destination value from the start.
    auto settled = makeBlock("big_muff", {{"sustain", 0.7}, {"tone", 0.5}, {"volume", 0.2}});
    auto expected = input;
    settled->process(expected.data(), expected.size());

    // Same block, but it arrives at volume=0.2 via a live parameter change.
    auto ramped = makeBlock("big_muff", {{"sustain", 0.7}, {"tone", 0.5}, {"volume", 0.9}});
    auto actual = input;
    const std::size_t change = 4096;
    ramped->process(actual.data(), change);
    ASSERT_TRUE(ramped->setLiveParam("volume", 0.2));
    ramped->process(actual.data() + change, actual.size() - change);

    // Long after the ramp, the two must agree: smoothing changes when a value
    // arrives, never where it lands.
    const std::size_t tail = actual.size() - 2048;
    EXPECT_NEAR(rms(actual, tail, actual.size()), rms(expected, tail, expected.size()), 1e-4);

    // And immediately after the change it must NOT have arrived yet -- that is
    // the difference between ramping and jumping.
    EXPECT_GT(rms(actual, change, change + 64), rms(expected, change, change + 64) * 1.5);
}

TEST(PedalParameterSmoothing, LiveChangeDoesNotClick) {
    const auto input = sine(80.0, 16000);
    const std::size_t change = 8000;

    for (const auto& type : {"big_muff", "tube_screamer"}) {
        // Baseline: the signal's own natural slew with nothing being moved.
        auto steady = makeBlock(type, {{"volume", 0.9}, {"level", 0.9}});
        auto reference = input;
        steady->process(reference.data(), reference.size());
        const double naturalSlew = maxSlew(reference, change - 500, change + 500);

        auto block = makeBlock(type, {{"volume", 0.9}, {"level", 0.9}});
        auto buffer = input;
        block->process(buffer.data(), change);
        // A ~19 dB cut: unsmoothed this would step the waveform by most of its
        // amplitude in a single sample.
        block->setLiveParam("volume", 0.1);
        block->setLiveParam("level", 0.1);
        block->process(buffer.data() + change, buffer.size() - change);

        EXPECT_LT(maxSlew(buffer, change - 500, change + 500), naturalSlew * 3.0)
            << type << " clicked on a live parameter change";
    }
}

TEST(PedalParameterSmoothing, ResetSnapsInsteadOfRamping) {
    auto block = makeBlock("tube_screamer", {{"level", 0.9}});
    ASSERT_TRUE(block->setLiveParam("level", 0.1));
    block->reset();  // mid-ramp

    // After a reset the block must behave as if 0.1 had always been set --
    // otherwise a preset load would render differently from the reference.
    auto reference = makeBlock("tube_screamer", {{"level", 0.1}});
    auto expected = sine(220.0, 4096);
    auto actual = expected;
    reference->process(expected.data(), expected.size());
    block->process(actual.data(), actual.size());
    EXPECT_EQ(actual, expected);
}

// --- Noise gate --------------------------------------------------------

TEST(NoiseGate, CrushesTheNoiseFloorButNotTheNote) {
    const std::size_t half = 24000;
    std::vector<float> buffer(half * 2);
    // Quiet hiss, then a loud note over the same hiss.
    std::srand(7);
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const double hiss = 0.002 * ((std::rand() / double(RAND_MAX)) * 2.0 - 1.0);
        const double note = i >= half ? 0.3 * std::sin(2.0 * M_PI * 220.0 * double(i) / kSampleRate)
                                      : 0.0;
        buffer[i] = static_cast<float>(hiss + note);
    }
    const auto input = buffer;

    auto gate = makeBlock("noise_gate", {{"threshold_db", -40.0}, {"range_db", -60.0}});
    gate->process(buffer.data(), buffer.size());

    // Past the release ramp, the floor should be gone.
    EXPECT_LT(rms(buffer, 12000, half), rms(input, 12000, half) / 50.0);
    // The note passes essentially untouched.
    EXPECT_NEAR(rms(buffer, half + 8000, buffer.size()),
                rms(input, half + 8000, input.size()), 1e-3);
}

TEST(NoiseGate, ReportsItsOpenState) {
    NoiseGateBlock gate;
    gate.prepare(kSampleRate);
    NoiseGateBlock::Settings settings = gate.settings();
    settings.thresholdDb = -40.0;
    settings.holdMs = 0.0;
    settings.releaseMs = 5.0;
    gate.setSettings(settings);

    EXPECT_FALSE(gate.isOpen());

    auto loud = sine(220.0, 4800, 0.3);
    gate.process(loud.data(), loud.size());
    EXPECT_TRUE(gate.isOpen());

    std::vector<float> silence(24000, 0.0f);
    gate.process(silence.data(), silence.size());
    EXPECT_FALSE(gate.isOpen());
}
