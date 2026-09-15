// End-to-end coverage of Vst3PluginHost against the in-repo trivial gain
// test plugin (tests/vst3_test_plugin/gain_plugin.cpp), built and packaged
// into a real `.vst3` bundle directory by CMake (see CMakeLists.txt "VST3
// plugin hosting") -- no third-party plugin binary needed. Only built when
// AUDIO_ENGINE_WITH_VST3 is ON.
#include "audio_engine/vst3_host.hpp"

#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

using namespace audio_engine;

namespace {
std::string bundleDir() { return VST3_TEST_PLUGIN_BUNDLE_DIR; }
}  // namespace

TEST(Vst3Host, LoadsTheInRepoTestPluginWithoutThrowing) {
    EXPECT_NO_THROW(loadVst3Plugin(bundleDir()));
}

TEST(Vst3Host, UnknownBundlePathThrows) {
    EXPECT_THROW(loadVst3Plugin(bundleDir() + "-does-not-exist"), std::runtime_error);
}

TEST(Vst3Host, ExposesTheGainParameterDescriptor) {
    auto plugin = loadVst3Plugin(bundleDir());
    auto params = plugin->listParameters();
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(params[0].key, "0");
    EXPECT_EQ(params[0].label, "Gain");
    EXPECT_EQ(params[0].unit, "x");
    EXPECT_NEAR(params[0].minValue, 0.0, 1e-9);
    EXPECT_NEAR(params[0].maxValue, 2.0, 1e-9);
    EXPECT_NEAR(params[0].defaultValue, 1.0, 1e-9);
    EXPECT_EQ(params[0].stepCount, 0);
}

TEST(Vst3Host, DefaultGainIsUnity) {
    auto plugin = loadVst3Plugin(bundleDir());
    plugin->prepare(48000.0);
    std::vector<float> buffer = {0.1f, -0.5f, 1.0f, -1.0f, 0.0f};
    std::vector<float> expected = buffer;
    plugin->process(buffer.data(), buffer.size());
    for (size_t i = 0; i < buffer.size(); ++i) EXPECT_NEAR(buffer[i], expected[i], 1e-5f);
}

TEST(Vst3Host, SetParamChangesGainOnTheNextProcessCall) {
    auto plugin = loadVst3Plugin(bundleDir());
    plugin->prepare(48000.0);
    ASSERT_TRUE(plugin->setParam("0", 2.0));  // plain gain 2.0x

    std::vector<float> buffer = {0.25f, -0.25f, 0.5f};
    plugin->process(buffer.data(), buffer.size());
    EXPECT_NEAR(buffer[0], 0.5f, 1e-4f);
    EXPECT_NEAR(buffer[1], -0.5f, 1e-4f);
    EXPECT_NEAR(buffer[2], 1.0f, 1e-4f);
}

TEST(Vst3Host, SetParamWithUnknownKeyReturnsFalse) {
    auto plugin = loadVst3Plugin(bundleDir());
    plugin->prepare(48000.0);
    EXPECT_FALSE(plugin->setParam("999", 1.0));
}

TEST(Vst3Host, ProcessHandlesBlocksLargerThanTheInternalChunkSize) {
    auto plugin = loadVst3Plugin(bundleDir());
    plugin->prepare(48000.0);
    ASSERT_TRUE(plugin->setParam("0", 2.0));

    std::vector<float> buffer(5000, 0.1f);  // > the host's internal 4096-sample chunk ceiling
    plugin->process(buffer.data(), buffer.size());
    for (float sample : buffer) EXPECT_NEAR(sample, 0.2f, 1e-4f);
}
