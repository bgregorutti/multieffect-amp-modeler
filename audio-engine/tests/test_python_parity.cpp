// Cross-language parity: do these C++ blocks render what the Python
// reference models render?
//
// vst-python/src/pedals/ is the reference implementation -- where each
// pedal's behaviour was designed, measured and argued about. These blocks are
// a port of it. vst-python/tools/export_golden.py freezes a render from each
// Python model into tests/golden/pedal_parity.json, and this test replays the
// identical input through the C++ block and compares, so "the C++ sounds the
// same" is an assertion rather than a claim.
//
// **Why not bit-exact.** The Python models compute in float64 throughout;
// EffectBlock's buffers are float. These blocks keep every intermediate in
// double and round once on the way out, so what remains is a single float
// quantization -- about 1e-7 relative. The tolerance below is set from that,
// not tuned until it passed.
//
// **Why smoothed parameters do not spoil this.** ParamSmoother::snap() sets
// current and target to the same value and next() then returns it unchanged,
// so a block that is constructed and then processed behaves exactly as if it
// had no smoothing at all. Only setLiveParam() ramps. See
// PedalParameterSmoothing in test_pedal_blocks.cpp for the other half of that
// contract.
//
// If this test fails after a deliberate change to a Python model, regenerate
// the fixture:
//   cd vst-python && .venv/bin/python tools/export_golden.py \
//       ../audio-engine/tests/golden/pedal_parity.json

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "audio_engine/pedal_dsp.hpp"
#include "audio_engine/preset_model.hpp"
#include "audio_engine/resource_manager.hpp"

using namespace audio_engine;

namespace {

// One float quantization of a signal of order 1, with headroom for it
// accumulating a little through four cascaded filters.
constexpr double kParityTolerance = 1e-6;

const nlohmann::json& fixture() {
    static const nlohmann::json document = [] {
        std::ifstream file(PEDAL_PARITY_FIXTURE);
        EXPECT_TRUE(file.is_open())
            << "missing parity fixture " << PEDAL_PARITY_FIXTURE
            << " -- regenerate with vst-python/tools/export_golden.py";
        nlohmann::json parsed;
        file >> parsed;
        return parsed;
    }();
    return document;
}

std::vector<double> doubles(const nlohmann::json& array) {
    return array.get<std::vector<double>>();
}

// Builds the block through the real factory, so a type missing from
// createEffectBlock() fails here rather than silently falling back to
// passthrough and producing a flat mismatch.
std::unique_ptr<EffectBlock> makeBlock(const std::string& type, const nlohmann::json& params) {
    EffectBlockSpec spec;
    spec.id = "parity";
    spec.type = type;
    for (auto& [key, value] : params.items()) {
        if (value.is_boolean()) {
            spec.params[key] = value.get<bool>();
        } else {
            spec.params[key] = value.get<double>();
        }
    }
    return createEffectBlock(spec);
}

struct Mismatch {
    double maxAbsError = 0.0;
    std::size_t worstIndex = 0;
    double expected = 0.0;
    double actual = 0.0;
};

Mismatch compare(const std::vector<float>& actual, const std::vector<double>& expected) {
    Mismatch worst;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const double error = std::fabs(static_cast<double>(actual[i]) - expected[i]);
        if (error > worst.maxAbsError) {
            worst = {error, i, expected[i], static_cast<double>(actual[i])};
        }
    }
    return worst;
}

}  // namespace

TEST(PythonParity, FixtureLoads) {
    const auto& document = fixture();
    EXPECT_GT(document["input"].size(), 0u);
    EXPECT_EQ(document["cases"].size(), 4u);
    EXPECT_DOUBLE_EQ(document["sample_rate"].get<double>(), 48000.0);
}

// The oversampler's FIR is recomputed in C++ from the same windowed-sinc
// definition scipy.signal.firwin uses, rather than pasted in as 65 constants.
// Checking it directly means a divergence in the filter *design* reports
// itself, instead of showing up as a mysteriously wrong waveform downstream.
TEST(PythonParity, OversamplerTapsMatchScipyFirwin) {
    const auto expected = doubles(fixture()["fir_taps"]);

    Oversampler oversampler;
    oversampler.prepare(fixture()["oversample"].get<int>());
    const auto& actual = oversampler.taps();

    ASSERT_EQ(actual.size(), expected.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        worst = std::max(worst, std::fabs(actual[i] - expected[i]));
    }
    // The fixture stores 12 significant digits, which is the binding limit
    // here rather than anything about the computation.
    EXPECT_LT(worst, 1e-11) << "FIR taps diverge from scipy.signal.firwin";
}

TEST(PythonParity, EveryModelRendersWhatPythonRenders) {
    const auto& document = fixture();
    const auto input = doubles(document["input"]);
    const double sampleRate = document["sample_rate"].get<double>();
    const auto blockSize = document["block_size"].get<std::size_t>();

    for (const auto& testCase : document["cases"]) {
        const auto type = testCase["type"].get<std::string>();
        const auto expected = doubles(testCase["output"]);
        SCOPED_TRACE(type + " " + testCase["params"].dump());

        auto block = makeBlock(type, testCase["params"]);
        ASSERT_NE(block, nullptr);
        block->prepare(sampleRate);

        // Rendered block by block, at the same size Python used -- this is
        // also what proves the C++ state handling survives block boundaries.
        std::vector<float> actual(input.begin(), input.end());
        for (std::size_t offset = 0; offset < actual.size(); offset += blockSize) {
            block->process(actual.data() + offset,
                           std::min(blockSize, actual.size() - offset));
        }

        ASSERT_EQ(actual.size(), expected.size());
        const Mismatch worst = compare(actual, expected);
        EXPECT_LT(worst.maxAbsError, kParityTolerance)
            << "worst sample " << worst.worstIndex << ": python " << worst.expected
            << " vs c++ " << worst.actual;
    }
}

// The C++ blocks chunk long buffers internally (kPedalMaxChunk) and carry
// filter state across those boundaries, so a host handing them one big buffer
// must get the same answer as one handing them many small ones.
TEST(PythonParity, RenderIsIndependentOfHostBlockSize) {
    const auto& document = fixture();
    const auto input = doubles(document["input"]);
    const double sampleRate = document["sample_rate"].get<double>();

    for (const auto& testCase : document["cases"]) {
        const auto type = testCase["type"].get<std::string>();
        const auto expected = doubles(testCase["output"]);
        SCOPED_TRACE(type);

        for (std::size_t blockSize : {std::size_t{1}, std::size_t{37}, std::size_t{512},
                                      input.size()}) {
            auto block = makeBlock(type, testCase["params"]);
            block->prepare(sampleRate);

            std::vector<float> actual(input.begin(), input.end());
            for (std::size_t offset = 0; offset < actual.size(); offset += blockSize) {
                block->process(actual.data() + offset,
                               std::min(blockSize, actual.size() - offset));
            }

            const Mismatch worst = compare(actual, expected);
            EXPECT_LT(worst.maxAbsError, kParityTolerance)
                << type << " at block size " << blockSize << ", worst sample "
                << worst.worstIndex;
        }
    }
}
