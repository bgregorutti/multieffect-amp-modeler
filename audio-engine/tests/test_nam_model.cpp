#include "audio_engine/nam_model.hpp"

#include <fstream>
#include <vector>

#include <gtest/gtest.h>

using namespace audio_engine;

namespace {
// Shaped like a real .nam file (github.com/sdatkinson/NeuralAmpModelerCore
// format): top-level architecture name, an architecture-specific config
// object, a flat weights array, plus the usual sample_rate/metadata.
constexpr const char* kValidNamJson = R"JSON(
{
  "version": "0.5.3",
  "architecture": "WaveNet",
  "config": {
    "layers": [{"input_size": 1, "condition_size": 1, "channels": 16, "head_size": 8,
                "kernel_size": 3, "dilations": [1, 2, 4, 8], "activation": "Tanh", "gated": false, "head_bias": true}]
  },
  "weights": [0.1, -0.2, 0.3, 0.05, -0.9, 1.0],
  "sample_rate": 48000,
  "metadata": {"name": "Test Amp", "modeled_by": "test-suite"}
}
)JSON";
}  // namespace

TEST(NamModel, ParsesValidMetadata) {
    NamModelMetadata meta = parseNamModelMetadata(kValidNamJson);
    EXPECT_EQ(meta.version, "0.5.3");
    EXPECT_EQ(meta.architecture, "WaveNet");
    EXPECT_TRUE(meta.config.is_object());
    EXPECT_EQ(meta.numWeights, 6u);
    EXPECT_DOUBLE_EQ(meta.sampleRate, 48000.0);
    EXPECT_EQ(meta.name, "Test Amp");
    EXPECT_EQ(meta.modeledBy, "test-suite");
}

TEST(NamModel, MissingArchitectureThrows) {
    constexpr const char* json = R"JSON({"config": {}, "weights": [1.0]})JSON";
    EXPECT_THROW(parseNamModelMetadata(json), NamParseError);
}

TEST(NamModel, MissingConfigThrows) {
    constexpr const char* json = R"JSON({"architecture": "LSTM", "weights": [1.0]})JSON";
    EXPECT_THROW(parseNamModelMetadata(json), NamParseError);
}

TEST(NamModel, ConfigNotAnObjectThrows) {
    constexpr const char* json = R"JSON({"architecture": "LSTM", "config": [1,2], "weights": [1.0]})JSON";
    EXPECT_THROW(parseNamModelMetadata(json), NamParseError);
}

TEST(NamModel, MissingWeightsIsAccepted) {
    // "weights" is informational, not required -- see nam_model.hpp.
    constexpr const char* json = R"JSON({"architecture": "LSTM", "config": {}})JSON";
    NamModelMetadata meta = parseNamModelMetadata(json);
    EXPECT_EQ(meta.numWeights, 0u);
}

TEST(NamModel, EmptyWeightsArrayIsAccepted) {
    // A real, valid shape: container architectures (e.g.
    // "SlimmableContainer") leave the top-level "weights" empty and nest
    // the real per-submodel weights under config.submodels[...] instead.
    constexpr const char* json = R"JSON({"architecture": "LSTM", "config": {}, "weights": []})JSON";
    NamModelMetadata meta = parseNamModelMetadata(json);
    EXPECT_EQ(meta.numWeights, 0u);
}

TEST(NamModel, ParsesSlimmableContainerShapeWithNestedWeights) {
    // Shaped like a real multi-gain-stage export (e.g. TONE3000-trained
    // models) -- top-level weights empty, real weights nested per submodel.
    constexpr const char* json = R"JSON(
    {
      "architecture": "SlimmableContainer",
      "config": {
        "submodels": [
          {"max_value": 0.5, "model": {"architecture": "WaveNet", "config": {}, "weights": [0.1, 0.2]}},
          {"max_value": 1.0, "model": {"architecture": "WaveNet", "config": {}, "weights": [0.3, 0.4, 0.5]}}
        ]
      },
      "weights": [],
      "sample_rate": 48000
    }
    )JSON";
    NamModelMetadata meta = parseNamModelMetadata(json);
    EXPECT_EQ(meta.architecture, "SlimmableContainer");
    EXPECT_EQ(meta.numWeights, 0u);
    ASSERT_TRUE(meta.config.contains("submodels"));
    EXPECT_EQ(meta.config["submodels"].size(), 2u);
}

TEST(NamModel, NonNumericWeightsThrows) {
    constexpr const char* json = R"JSON({"architecture": "LSTM", "config": {}, "weights": [1.0, "oops", 2.0]})JSON";
    EXPECT_THROW(parseNamModelMetadata(json), NamParseError);
}

TEST(NamModel, EmptyArchitectureStringThrows) {
    constexpr const char* json = R"JSON({"architecture": "", "config": {}, "weights": [1.0]})JSON";
    EXPECT_THROW(parseNamModelMetadata(json), NamParseError);
}

TEST(NamModel, MalformedJsonThrows) { EXPECT_THROW(parseNamModelMetadata("{ not json"), NamParseError); }

TEST(NamModel, MissingOptionalFieldsGetDefaults) {
    constexpr const char* json = R"JSON({"architecture": "LSTM", "config": {"hidden_size": 8}, "weights": [1.0, 2.0]})JSON";
    NamModelMetadata meta = parseNamModelMetadata(json);
    EXPECT_EQ(meta.version, "");
    EXPECT_DOUBLE_EQ(meta.sampleRate, 48000.0);  // documented default
    EXPECT_EQ(meta.name, "");
}

TEST(NamModel, ParsesFromFile) {
    std::string path = "/tmp/audio_engine_test_model.nam";
    {
        std::ofstream out(path);
        out << kValidNamJson;
    }
    NamModelMetadata meta = parseNamModelFile(path);
    EXPECT_EQ(meta.architecture, "WaveNet");
}

TEST(NamModel, NonexistentFileThrows) {
    EXPECT_THROW(parseNamModelFile("/nonexistent/path/model.nam"), NamParseError);
}

// StubNamModel: explicitly documented as NOT running real WaveNet/LSTM
// inference (see nam_model.hpp) -- it must behave as identity (or a fixed
// makeup gain), so the rest of the chain is testable without real
// inference existing yet.
TEST(NamModel, StubIsIdentityByDefault) {
    NamModelMetadata meta = parseNamModelMetadata(kValidNamJson);
    StubNamModel stub(meta);
    stub.prepare(48000.0);

    std::vector<float> buf = {0.1f, -0.2f, 0.3f, 1.0f, -1.0f};
    std::vector<float> expected = buf;
    stub.process(buf);
    for (size_t i = 0; i < buf.size(); ++i) EXPECT_FLOAT_EQ(buf[i], expected[i]);
    EXPECT_EQ(stub.metadata().architecture, "WaveNet");
}

TEST(NamModel, StubAppliesMakeupGainWhenConfigured) {
    NamModelMetadata meta = parseNamModelMetadata(kValidNamJson);
    StubNamModel stub(meta, /*makeupGainLinear=*/2.0f);
    stub.prepare(48000.0);
    std::vector<float> buf = {0.25f};
    stub.process(buf);
    EXPECT_FLOAT_EQ(buf[0], 0.5f);
}
