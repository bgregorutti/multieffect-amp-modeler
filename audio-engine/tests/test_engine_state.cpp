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
// called (every preset in this file omits nam_asset_id/ir_asset_id).
class UnusedAssetLoader : public IAssetLoader {
public:
    std::shared_ptr<INamModel> loadNam(const std::string&) override {
        ADD_FAILURE() << "loadNam should not be called by these tests";
        return nullptr;
    }
    std::shared_ptr<IrHandle> loadIr(const std::string&) override {
        ADD_FAILURE() << "loadIr should not be called by these tests";
        return nullptr;
    }
};

Preset makeGainPreset(double gainDb) {
    Preset p;
    p.id = "p1";
    p.name = "test";
    EffectBlockSpec gain;
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
