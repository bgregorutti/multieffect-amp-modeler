// Unit tests for EngineState::processAudioBlock -- the entry point the
// real-time audio callback (IAudioIoBackend, see main.cpp) drives once per
// block. Deliberately in-process (no control socket, no real audio
// device): the socket transport is already covered end to end by
// test_control_socket_integration.cpp, and no real audio hardware exists
// in this sandbox to test PortAudioBackend against (see README.md
// "Real-time audio I/O") -- this file only tests the DSP-dispatch logic
// processAudioBlock adds on top of ResourceManager/bypass.
#include "audio_engine/engine_state.hpp"

#include <chrono>
#include <atomic>
#include <future>
#include <memory>
#include <thread>
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

namespace {

struct SilentNamModel : public INamModel {
    const NamModelMetadata& metadata() const override { return metadata_; }
    void prepare(double) override {}
    void process(float*, std::size_t) override {}
    NamModelMetadata metadata_;
};

// loadNam blocks until the test releases it -- stands in for a slow disk
// read + NAM prewarm, so the test can act while a load is in progress.
class BlockingAssetLoader : public IAssetLoader {
public:
    std::promise<void> loadStarted;
    std::promise<void> release;

    std::shared_ptr<INamModel> loadNam(const std::string&) override {
        loadStarted.set_value();
        release.get_future().wait();
        return std::make_shared<SilentNamModel>();
    }
    std::shared_ptr<IrHandle> loadIr(const std::string&, double) override { return nullptr; }
    std::unique_ptr<EffectBlock> loadVst3(const std::string&) override { return nullptr; }
};

}  // namespace

TEST(EngineStateAudio, AudioKeepsRunningWhileANewAmpLoads) {
    auto loader = std::make_shared<BlockingAssetLoader>();
    EngineState state(loader);
    state.resourceManager().loadPreset(makeGainPreset(6.0));

    ASSERT_TRUE(state.handleCommand(nlohmann::json{
                    {"cmd", "register_asset"},
                    {"asset", {{"id", "nam1"}, {"kind", "nam"}, {"filename", "a.nam"}, {"stored_path", "/fake/a.nam"}}}})
                    .at("ok")
                    .get<bool>());

    nlohmann::json loadCmd = {
        {"cmd", "load_preset"},
        {"preset",
         {{"id", "p2"},
          {"name", "amp"},
          {"rig_id", "r2"},
          {"rig_name", "rig 2"},
          {"blocks", {{{"id", "amp"}, {"type", "nam"}, {"asset_id", "nam1"}, {"enabled", true}, {"params", nlohmann::json::object()}}}}}}};
    std::thread commandThread([&] { state.handleCommand(loadCmd); });
    loader->loadStarted.get_future().wait();

    // The load is now stuck inside loadNam. The audio callback must still
    // get through, still playing the previous chain (the +6dB gain).
    auto audio = std::async(std::launch::async, [&] {
        std::vector<float> buf = {0.1f, 0.2f};
        state.processAudioBlock(buf.data(), buf.size());
        return buf;
    });
    bool audioRan = audio.wait_for(std::chrono::seconds(2)) == std::future_status::ready;

    loader->release.set_value();
    commandThread.join();

    ASSERT_TRUE(audioRan) << "processAudioBlock was blocked by an in-progress load_preset";
    EXPECT_GT(audio.get()[0], 0.15f) << "audio should still run the previous chain during the load";
    EXPECT_EQ(state.currentPresetId(), std::optional<std::string>("p2"));
}

namespace {

class InstantAssetLoader : public IAssetLoader {
public:
    std::atomic<int> namLoads{0};
    std::atomic<int> irLoads{0};
    std::shared_ptr<INamModel> loadNam(const std::string&) override {
        ++namLoads;
        return std::make_shared<SilentNamModel>();
    }
    std::shared_ptr<IrHandle> loadIr(const std::string&, double) override {
        ++irLoads;
        auto ir = std::make_shared<IrHandle>();
        ir->samples = {1.0f, 0.5f, 0.25f};
        return ir;
    }
    std::unique_ptr<EffectBlock> loadVst3(const std::string&) override { return nullptr; }
};

nlohmann::json rigPresetCommand(const std::string& id, bool driveOn) {
    auto block = [](const std::string& blockId, const std::string& type, nlohmann::json assetId, bool enabled) {
        return nlohmann::json{
            {"id", blockId}, {"type", type}, {"asset_id", assetId}, {"enabled", enabled}, {"params", nlohmann::json::object()}};
    };
    return {{"cmd", "load_preset"},
            {"preset",
             {{"id", id},
              {"name", id},
              {"rig_id", "r1"},
              {"rig_name", "rig 1"},
              {"blocks",
               {block("amp", "nam", "nam1", true), block("drive", "tube_screamer", nullptr, driveOn),
                block("cab", "ir", "ir1", true)}}}}};
}

}  // namespace

TEST(EngineStateAudio, PresetSwitchesWhileAudioRunsKeepTheSameAmpAndCab) {
    auto loader = std::make_shared<InstantAssetLoader>();
    EngineState state(loader);
    for (const char* id : {"nam1", "ir1"}) {
        nlohmann::json asset = {{"id", id}, {"kind", id[0] == 'n' ? "nam" : "ir"}, {"filename", id}, {"stored_path", id}};
        ASSERT_TRUE(state.handleCommand({{"cmd", "register_asset"}, {"asset", asset}}).at("ok").get<bool>());
    }

    std::atomic<bool> stop{false};
    std::thread audio([&] {
        std::vector<float> buf(64, 0.1f);
        while (!stop) state.processAudioBlock(buf.data(), buf.size());
    });
    for (int i = 0; i < 200; ++i) {
        auto reply = state.handleCommand(rigPresetCommand(i % 2 ? "drive" : "clean", i % 2 == 1));
        ASSERT_TRUE(reply.at("ok").get<bool>()) << reply.dump();
    }
    stop = true;
    audio.join();

    EXPECT_EQ(loader->namLoads.load(), 1);
    EXPECT_EQ(loader->irLoads.load(), 1);
}
