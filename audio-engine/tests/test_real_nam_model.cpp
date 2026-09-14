// Tests for RealNamModel, the real WaveNet/LSTM inference adapter over
// vendored NeuralAmpModelerCore. Only compiled/run when
// AUDIO_ENGINE_WITH_REAL_NAM is on (see CMakeLists.txt) -- these depend on
// the vendored library actually being fetched.
//
// Fixtures: NeuralAmpModelerCore ships its own example .nam files (used by
// its own test suite) under example_models/ -- reusing those here means
// these tests exercise the exact same file shapes the library's authors
// test against, at no extra cost (already fetched by FetchContent), rather
// than hand-rolling fixtures that might not match real export shapes. See
// NAM_EXAMPLE_MODELS_DIR (set in CMakeLists.txt).
#include "audio_engine/real_nam_model.hpp"

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <NAM/get_dsp.h>

#include "audio_engine/nam_model.hpp"

using namespace audio_engine;

namespace {
std::string examplePath(const std::string& filename) { return std::string(NAM_EXAMPLE_MODELS_DIR) + "/" + filename; }

std::unique_ptr<RealNamModel> loadExample(const std::string& filename) {
    std::string path = examplePath(filename);
    NamModelMetadata meta = parseNamModelFile(path);
    auto dsp = nam::get_dsp(std::filesystem::path(path));
    return std::make_unique<RealNamModel>(std::move(meta), std::move(dsp));
}
}  // namespace

TEST(RealNamModel, LoadsStandardWaveNetExampleWithoutThrowing) {
    auto model = loadExample("wavenet.nam");
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->metadata().architecture, "WaveNet");
}

TEST(RealNamModel, LoadsSlimmableContainerExampleWithoutThrowing) {
    // The exact format that motivated adding real inference -- see
    // README.md "Real NAM inference": multi-gain-stage exports leave the
    // top-level "weights" empty and nest real weights per submodel.
    auto model = loadExample("slimmable_container.nam");
    ASSERT_NE(model, nullptr);
    EXPECT_EQ(model->metadata().architecture, "SlimmableContainer");
    EXPECT_EQ(model->metadata().numWeights, 0u);
}

TEST(RealNamModel, ProcessProducesBoundedNonIdentityOutput) {
    auto model = loadExample("wavenet.nam");
    model->prepare(48000.0);

    std::vector<float> signal(4800);  // 100ms
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = 0.3f * std::sin(static_cast<float>(i) * 0.05f);
    }
    std::vector<float> original = signal;

    const std::size_t blockSize = 64;
    for (std::size_t i = 0; i + blockSize <= signal.size(); i += blockSize) {
        model->process(signal.data() + i, blockSize);
    }

    double diffSum = 0.0;
    for (std::size_t i = 0; i < signal.size(); ++i) {
        diffSum += std::fabs(signal[i] - original[i]);
        // Not a hard architectural guarantee (a pathological model could
        // legitimately produce a hot output), but a real WaveNet forward
        // pass on a moderate-level sine should stay well within a sane
        // range -- catches a badly wired adapter (e.g. wrong buffer
        // indexing) producing garbage/NaN/huge values.
        ASSERT_TRUE(std::isfinite(signal[i]));
        EXPECT_LT(std::fabs(signal[i]), 10.0f);
    }
    // A real forward pass must not be a no-op.
    EXPECT_GT(diffSum / signal.size(), 1e-4);
}

TEST(RealNamModel, PrepareCanBeCalledMultipleTimes) {
    // Simulates switching presets back to a model already loaded once, or
    // a sample-rate change -- Reset() must be safe to call again.
    auto model = loadExample("wavenet.nam");
    model->prepare(48000.0);
    model->prepare(48000.0);
    model->prepare(44100.0);

    std::vector<float> buf(64, 0.1f);
    model->process(buf.data(), buf.size());
    for (float s : buf) EXPECT_TRUE(std::isfinite(s));
}

TEST(RealNamModel, ReportsMetadataFromTheOriginalParse) {
    auto model = loadExample("wavenet.nam");
    EXPECT_DOUBLE_EQ(model->metadata().sampleRate, 48000.0);
    EXPECT_EQ(model->metadata().name, "Test Model");
    EXPECT_EQ(model->metadata().modeledBy, "Steve");
}

// nam::DSP's default process() is the null operation (copies input straight
// to output -- see NeuralAmpModelerCore's dsp.cpp), so a plain nam::DSP
// with a known SetLoudness() is a precise, deterministic fixture for
// verifying the loudness-normalization gain itself, independent of any
// real network's own numerics -- see real_nam_model.cpp's
// loudnessNormalizationGain and README.md "NAM output loudness
// normalization" for why this exists (a real bug: one commercial "gain
// stage" export measured ~6dB hotter than its siblings and clipped with
// no compensation).
TEST(RealNamModel, NoGainAppliedWhenModelLoudnessMatchesTarget) {
    auto dsp = std::make_unique<nam::DSP>(1, 1, 48000.0);
    dsp->SetLoudness(kNamTargetLoudnessDb);
    NamModelMetadata meta;
    meta.architecture = "Test";
    RealNamModel model(meta, std::move(dsp));
    model.prepare(48000.0);

    std::vector<float> buf = {0.1f, 0.2f, -0.3f};
    model.process(buf.data(), buf.size());
    EXPECT_NEAR(buf[0], 0.1f, 1e-5f);
    EXPECT_NEAR(buf[1], 0.2f, 1e-5f);
    EXPECT_NEAR(buf[2], -0.3f, 1e-5f);
}

TEST(RealNamModel, AttenuatesAModelReportedAsLouderThanTarget) {
    // 6dB hotter than target -> should come out at ~half amplitude
    // (matching the real Ampeg "Gain 1" file's measured ~6dB excess).
    auto dsp = std::make_unique<nam::DSP>(1, 1, 48000.0);
    dsp->SetLoudness(kNamTargetLoudnessDb + 6.0);
    NamModelMetadata meta;
    meta.architecture = "Test";
    RealNamModel model(meta, std::move(dsp));
    model.prepare(48000.0);

    std::vector<float> buf = {0.8f};
    model.process(buf.data(), buf.size());
    const float expectedGain = static_cast<float>(std::pow(10.0, -6.0 / 20.0));  // ~0.501
    EXPECT_NEAR(buf[0], 0.8f * expectedGain, 1e-4f);
}

TEST(RealNamModel, NoGainAppliedWhenModelReportsNoLoudness) {
    // dsp.HasLoudness() defaults to false until SetLoudness() is called --
    // a model with no loudness metadata at all must not be guessed at.
    auto dsp = std::make_unique<nam::DSP>(1, 1, 48000.0);
    NamModelMetadata meta;
    meta.architecture = "Test";
    RealNamModel model(meta, std::move(dsp));
    model.prepare(48000.0);

    std::vector<float> buf = {0.5f};
    model.process(buf.data(), buf.size());
    EXPECT_NEAR(buf[0], 0.5f, 1e-5f);
}
