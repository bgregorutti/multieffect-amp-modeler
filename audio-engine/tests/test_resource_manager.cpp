#include "audio_engine/resource_manager.hpp"

#include <atomic>
#include <cmath>
#include <memory>

#include <gtest/gtest.h>

using namespace audio_engine;

namespace {

// Instance-counting test double for IAssetLoader: every loaded NAM model /
// IR increments a shared counter in its constructor and decrements it in
// its destructor, so tests can assert resources are actually released
// (not merely "replaced by a pointer nobody looks at again") when a new
// preset is loaded. No filesystem access at all -- storedPath is ignored.
struct CountingNamModel : public INamModel {
    explicit CountingNamModel(std::atomic<int>& liveCount, std::atomic<int>& totalCount) : live_(liveCount) {
        ++live_;
        ++totalCount;
    }
    ~CountingNamModel() override { --live_; }

    const NamModelMetadata& metadata() const override { return metadata_; }
    void prepare(double) override {}
    void process(float*, std::size_t) override {}

    std::atomic<int>& live_;
    NamModelMetadata metadata_;
};

struct CountingIrHandle : public IrHandle {
    explicit CountingIrHandle(std::atomic<int>& liveCount, std::atomic<int>& totalCount) : live_(liveCount) {
        ++live_;
        ++totalCount;
        samples = {1.0f};
    }
    ~CountingIrHandle() override { --live_; }
    std::atomic<int>& live_;
};

class CountingAssetLoader : public IAssetLoader {
public:
    std::atomic<int> liveNamCount{0};
    std::atomic<int> totalNamLoaded{0};
    std::atomic<int> liveIrCount{0};
    std::atomic<int> totalIrLoaded{0};

    std::shared_ptr<INamModel> loadNam(const std::string&) override {
        return std::make_shared<CountingNamModel>(liveNamCount, totalNamLoaded);
    }
    std::shared_ptr<IrHandle> loadIr(const std::string&, double) override {
        return std::make_shared<CountingIrHandle>(liveIrCount, totalIrLoaded);
    }
};

Preset makePreset(const std::string& id, const std::string& namAssetId, const std::string& irAssetId) {
    Preset p;
    p.id = id;
    p.name = "preset-" + id;
    p.nam_asset_id = namAssetId;
    p.ir_asset_id = irAssetId;
    EffectBlockSpec gain;
    gain.type = "gain";
    gain.enabled = true;
    gain.params["gain_db"] = 0.0;
    p.blocks.push_back(gain);
    return p;
}

}  // namespace

TEST(ResourceManager, LoadingSequentialPresetsReleasesPreviousResources) {
    auto loaderOwned = std::make_shared<CountingAssetLoader>();
    CountingAssetLoader& loader = *loaderOwned;
    ResourceManager manager(loaderOwned);

    Asset nam1;
    nam1.id = "nam1";
    nam1.kind = AssetKind::Nam;
    nam1.filename = "a.nam";
    nam1.stored_path = "/fake/a.nam";
    Asset ir1;
    ir1.id = "ir1";
    ir1.kind = AssetKind::Ir;
    ir1.filename = "a.wav";
    ir1.stored_path = "/fake/a.wav";
    Asset nam2 = nam1;
    nam2.id = "nam2";
    Asset ir2 = ir1;
    ir2.id = "ir2";
    Asset nam3 = nam1;
    nam3.id = "nam3";
    Asset ir3 = ir1;
    ir3.id = "ir3";

    manager.registerAsset(nam1);
    manager.registerAsset(ir1);
    manager.registerAsset(nam2);
    manager.registerAsset(ir2);
    manager.registerAsset(nam3);
    manager.registerAsset(ir3);

    // Load preset A.
    manager.loadPreset(makePreset("A", "nam1", "ir1"));
    EXPECT_EQ(loader.liveNamCount.load(), 1);
    EXPECT_EQ(loader.liveIrCount.load(), 1);
    ASSERT_NE(manager.currentChain(), nullptr);
    EXPECT_EQ(manager.currentChain()->presetId, "A");

    // Load preset B: preset A's resources must be released (only one
    // model + one IR resident at a time -- V1 explicitly does not run
    // multiple simulations in parallel, and this is a real RAM-budget
    // regression if it leaks).
    manager.loadPreset(makePreset("B", "nam2", "ir2"));
    EXPECT_EQ(loader.liveNamCount.load(), 1) << "preset A's NAM model was not released";
    EXPECT_EQ(loader.liveIrCount.load(), 1) << "preset A's IR was not released";
    EXPECT_EQ(manager.currentChain()->presetId, "B");

    // Load preset C, same story.
    manager.loadPreset(makePreset("C", "nam3", "ir3"));
    EXPECT_EQ(loader.liveNamCount.load(), 1);
    EXPECT_EQ(loader.liveIrCount.load(), 1);
    EXPECT_EQ(manager.currentChain()->presetId, "C");

    // Sanity: three distinct loads actually happened (not just returning
    // a cached instance three times, which would make the "released"
    // assertions above vacuous).
    EXPECT_EQ(loader.totalNamLoaded.load(), 3);
    EXPECT_EQ(loader.totalIrLoaded.load(), 3);
}

TEST(ResourceManager, PresetWithNoAssetsLoadsWithNullModelAndIr) {
    auto loader = std::make_shared<CountingAssetLoader>();
    ResourceManager manager(loader);

    Preset p;
    p.id = "no-assets";
    p.name = "Clean bypass";
    manager.loadPreset(p);

    ASSERT_NE(manager.currentChain(), nullptr);
    EXPECT_EQ(manager.currentChain()->namModel, nullptr);
    EXPECT_EQ(manager.currentChain()->ir, nullptr);
    EXPECT_EQ(manager.currentChain()->cabinet, nullptr);
}

TEST(ResourceManager, UnknownAssetIdThrows) {
    auto loader = std::make_shared<CountingAssetLoader>();
    ResourceManager manager(loader);
    Preset p = makePreset("X", "does-not-exist", "also-missing");
    EXPECT_THROW(manager.loadPreset(p), std::runtime_error);
}

TEST(ResourceManager, DisabledBlocksAreSkipped) {
    auto loader = std::make_shared<CountingAssetLoader>();
    ResourceManager manager(loader);

    Preset p;
    p.id = "p1";
    p.name = "test";
    EffectBlockSpec enabledGain;
    enabledGain.type = "gain";
    enabledGain.enabled = true;
    EffectBlockSpec disabledGain;
    disabledGain.type = "gain";
    disabledGain.enabled = false;
    p.blocks = {enabledGain, disabledGain};

    manager.loadPreset(p);
    EXPECT_EQ(manager.currentChain()->effects.size(), 1u);
}

TEST(ResourceManager, ChainProcessRunsNamThenEffectsThenCabinet) {
    auto loader = std::make_shared<CountingAssetLoader>();
    ResourceManager manager(loader);

    Asset nam;
    nam.id = "n";
    nam.kind = AssetKind::Nam;
    nam.filename = "n.nam";
    nam.stored_path = "/fake/n.nam";
    Asset ir;
    ir.id = "i";
    ir.kind = AssetKind::Ir;
    ir.filename = "i.wav";
    ir.stored_path = "/fake/i.wav";
    manager.registerAsset(nam);
    manager.registerAsset(ir);

    Preset p = makePreset("p1", "n", "i");  // includes a 0dB gain block
    manager.loadPreset(p);

    std::vector<float> buf = {1.0f, 0.0f, 0.0f};
    manager.currentChain()->process(buf.data(), buf.size());
    // CountingIrHandle's IR is {1.0} (identity), NAM stub is identity,
    // 0dB gain is identity -- so the chain as a whole should be identity.
    EXPECT_FLOAT_EQ(buf[0], 1.0f);
    EXPECT_FLOAT_EQ(buf[1], 0.0f);
    EXPECT_FLOAT_EQ(buf[2], 0.0f);
}

TEST(ResourceManager, ChainProcessClampsFinalOutputToUnitRange) {
    // Defensive safety net (see resource_manager.cpp's comment on
    // EngineChain::process): nothing here relies on any single block
    // misbehaving -- a preset can simply stack enough gain on its own to
    // exceed [-1, 1], and the chain's actual output to hardware must never
    // do that regardless of what a preset asks for.
    auto loader = std::make_shared<CountingAssetLoader>();
    ResourceManager manager(loader);

    Preset p;
    p.id = "loud";
    p.name = "loud";
    EffectBlockSpec bigGain;
    bigGain.type = "gain";
    bigGain.enabled = true;
    bigGain.params["gain_db"] = 40.0;  // ~100x linear gain
    p.blocks = {bigGain};
    manager.loadPreset(p);

    std::vector<float> buf = {0.5f, -0.5f, 0.01f};
    manager.currentChain()->process(buf.data(), buf.size());

    for (float s : buf) EXPECT_LE(std::fabs(s), 1.0f);
    EXPECT_FLOAT_EQ(buf[0], 1.0f);   // 0.5 * ~100 clamps to the ceiling
    EXPECT_FLOAT_EQ(buf[1], -1.0f);  // -0.5 * ~100 clamps to the floor
}

TEST(ResourceManager, CreateEffectBlockFallsBackToPassthroughForUnknownType) {
    EffectBlockSpec spec;
    spec.type = "some_future_block_type";
    auto block = createEffectBlock(spec);
    ASSERT_NE(block, nullptr);
    block->prepare(48000.0);
    std::vector<float> buf = {1.0f, 2.0f, 3.0f};
    std::vector<float> expected = buf;
    block->process(buf);
    EXPECT_EQ(buf, expected);
}
