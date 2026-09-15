// Unit tests for EngineState::processAudioBlock -- the entry point the
// real-time audio callback (IAudioIoBackend, see main.cpp) drives once per
// block. Deliberately in-process (no control socket, no real audio
// device): the socket transport is already covered end to end by
// test_control_socket_integration.cpp, and no real audio hardware exists
// in this sandbox to test PortAudioBackend against (see README.md
// "Real-time audio I/O") -- this file only tests the DSP-dispatch logic
// processAudioBlock adds on top of ResourceManager/bypass.
#include "audio_engine/engine_state.hpp"

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "audio_engine/resource_manager.hpp"

using namespace audio_engine;

namespace {

// No filesystem access, no nam/ir assets -- a loader that's never actually
// called (no block in this file carries an asset_id).
class UnusedAssetLoader : public IAssetLoader {
public:
    std::shared_ptr<INamModel> loadNam(const std::string&) override {
        ADD_FAILURE() << "loadNam should not be called by these tests";
        return nullptr;
    }
    std::shared_ptr<IrHandle> loadIr(const std::string&, double) override {
        ADD_FAILURE() << "loadIr should not be called by these tests";
        return nullptr;
    }
    std::unique_ptr<EffectBlock> loadVst3(const std::string&) override {
        ADD_FAILURE() << "loadVst3 should not be called by these tests";
        return nullptr;
    }
};

Preset makeGainPreset(double gainDb) {
    Preset p;
    p.id = "p1";
    p.name = "test";
    p.rig_id = "r1";
    p.rig_name = "test rig";
    EffectBlockSpec gain;
    gain.id = "boost";
    gain.type = "gain";
    gain.enabled = true;
    gain.params["gain_db"] = gainDb;
    p.blocks.push_back(gain);
    return p;
}

}  // namespace

TEST(EngineStateAudio, NoPresetLoadedIsPassthrough) {
    EngineState state(std::make_shared<UnusedAssetLoader>());
    std::vector<float> buf = {0.5f, -0.25f, 1.0f};
    std::vector<float> expected = buf;

    state.processAudioBlock(buf.data(), buf.size());

    EXPECT_EQ(buf, expected);
}

TEST(EngineStateAudio, RunsCurrentChainOverTheBuffer) {
    EngineState state(std::make_shared<UnusedAssetLoader>());
    state.resourceManager().loadPreset(makeGainPreset(6.0));  // +6dB ~= x2

    std::vector<float> buf = {0.1f, 0.2f, 0.3f};
    state.processAudioBlock(buf.data(), buf.size());

    for (float sample : buf) {
        EXPECT_GT(sample, 0.15f);  // roughly doubled, not left untouched
    }
}

TEST(EngineStateAudio, BypassSkipsProcessingEvenWithAPresetLoaded) {
    EngineState state(std::make_shared<UnusedAssetLoader>());
    state.resourceManager().loadPreset(makeGainPreset(20.0));  // would clearly change the buffer

    auto reply = state.handleCommand({{"cmd", "set_bypass"}, {"bypass", true}});
    ASSERT_TRUE(reply["ok"].get<bool>());

    std::vector<float> buf = {0.1f, 0.2f, 0.3f};
    std::vector<float> expected = buf;
    state.processAudioBlock(buf.data(), buf.size());

    EXPECT_EQ(buf, expected);
}

TEST(EngineStateAudio, UnbypassResumesProcessing) {
    EngineState state(std::make_shared<UnusedAssetLoader>());
    state.resourceManager().loadPreset(makeGainPreset(6.0));

    state.handleCommand({{"cmd", "set_bypass"}, {"bypass", true}});
    state.handleCommand({{"cmd", "set_bypass"}, {"bypass", false}});

    std::vector<float> buf = {0.1f, 0.2f, 0.3f};
    state.processAudioBlock(buf.data(), buf.size());

    EXPECT_GT(buf[0], 0.15f);
}

TEST(EngineStateSetBlockParam, MutatesTheLiveChainWithoutReloading) {
    EngineState state(std::make_shared<UnusedAssetLoader>());
    state.resourceManager().loadPreset(makeGainPreset(0.0));  // 0dB = identity

    auto reply = state.handleCommand(
        {{"cmd", "set_block_param"}, {"block_id", "boost"}, {"param_key", "gain_db"}, {"value", 6.0206}});
    ASSERT_TRUE(reply["ok"].get<bool>()) << reply.dump();
    EXPECT_EQ(reply["block_id"], "boost");

    std::vector<float> buf = {0.25f};
    state.processAudioBlock(buf.data(), buf.size());
    EXPECT_NEAR(buf[0], 0.5f, 1e-3f);  // ~doubled, same block instance still in place
}

TEST(EngineStateSetBlockParam, UnknownBlockIdIsNotFound) {
    EngineState state(std::make_shared<UnusedAssetLoader>());
    state.resourceManager().loadPreset(makeGainPreset(0.0));

    auto reply =
        state.handleCommand({{"cmd", "set_block_param"}, {"block_id", "nope"}, {"param_key", "gain_db"}, {"value", 1.0}});
    EXPECT_FALSE(reply["ok"].get<bool>());
    EXPECT_EQ(reply["code"], "not_found");
}

TEST(EngineStateSetBlockParam, UnrecognizedParamKeyIsAValidationError) {
    EngineState state(std::make_shared<UnusedAssetLoader>());
    state.resourceManager().loadPreset(makeGainPreset(0.0));

    auto reply = state.handleCommand(
        {{"cmd", "set_block_param"}, {"block_id", "boost"}, {"param_key", "not_a_real_param"}, {"value", 1.0}});
    EXPECT_FALSE(reply["ok"].get<bool>());
    EXPECT_EQ(reply["code"], "validation_error");
}

TEST(EngineStateSetBlockParam, NoPresetLoadedIsNotFound) {
    EngineState state(std::make_shared<UnusedAssetLoader>());

    auto reply = state.handleCommand(
        {{"cmd", "set_block_param"}, {"block_id", "boost"}, {"param_key", "gain_db"}, {"value", 1.0}});
    EXPECT_FALSE(reply["ok"].get<bool>());
    EXPECT_EQ(reply["code"], "not_found");
}

TEST(EngineStateListBlockTypes, ReturnsGainAndToneStackAmongOthers) {
    EngineState state(std::make_shared<UnusedAssetLoader>());
    auto reply = state.handleCommand({{"cmd", "list_block_types"}});
    ASSERT_TRUE(reply["ok"].get<bool>());

    bool foundGain = false, foundToneStack = false;
    for (const auto& entry : reply["block_types"]) {
        if (entry["type"] == "gain") {
            foundGain = true;
            ASSERT_EQ(entry["parameters"].size(), 1u);
            EXPECT_EQ(entry["parameters"][0]["key"], "gain_db");
        }
        if (entry["type"] == "tone_stack") {
            foundToneStack = true;
            EXPECT_EQ(entry["parameters"].size(), 3u);
        }
    }
    EXPECT_TRUE(foundGain);
    EXPECT_TRUE(foundToneStack);
}

TEST(EngineStateRegisterAsset, NativeKindsGetNoParametersField) {
    // "parameters" is a VST3-only addition to the reply (see
    // handleRegisterAsset) -- a nam/ir asset's schema is nothing new, it's
    // just carried by the block type it's attached to.
    EngineState state(std::make_shared<UnusedAssetLoader>());
    auto reply = state.handleCommand(
        {{"cmd", "register_asset"},
         {"asset", {{"id", "n1"}, {"kind", "nam"}, {"filename", "a.nam"}, {"stored_path", "/fake/a.nam"}}}});
    ASSERT_TRUE(reply["ok"].get<bool>()) << reply.dump();
    EXPECT_EQ(reply["asset_id"], "n1");
    EXPECT_FALSE(reply.contains("parameters"));
}

TEST(EngineStateRegisterAsset, MalformedAssetIsAValidationError) {
    EngineState state(std::make_shared<UnusedAssetLoader>());
    auto reply = state.handleCommand({{"cmd", "register_asset"}, {"asset", {{"id", "n1"}}}});  // missing "kind" etc.
    EXPECT_FALSE(reply["ok"].get<bool>());
    EXPECT_EQ(reply["code"], "validation_error");
}
