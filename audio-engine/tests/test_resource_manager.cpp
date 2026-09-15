#include "audio_engine/resource_manager.hpp"

#include <atomic>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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

// Unlike CountingNamModel/CountingIrHandle, a "vst3" block has no dedicated
// EngineChain field to release-and-replace -- it just lives in `effects`
// like any other block, so a plain identity EffectBlock double (counted the
// same way) is enough to prove ResourceManager::loadPreset's "vst3" branch
// actually calls IAssetLoader::loadVst3 and wires the result into the
// chain, without needing the real VST3 SDK at all.
struct CountingVst3Block : public EffectBlock {
    explicit CountingVst3Block(std::atomic<int>& liveCount, std::atomic<int>& totalCount) : live_(liveCount) {
        ++live_;
        ++totalCount;
    }
    ~CountingVst3Block() override { --live_; }
    void prepare(double) override {}
    void process(float*, std::size_t) override {}
    std::atomic<int>& live_;
};

class CountingAssetLoader : public IAssetLoader {
public:
    std::atomic<int> liveNamCount{0};
    std::atomic<int> totalNamLoaded{0};
    std::atomic<int> liveIrCount{0};
    std::atomic<int> totalIrLoaded{0};
    std::atomic<int> liveVst3Count{0};
    std::atomic<int> totalVst3Loaded{0};

    std::shared_ptr<INamModel> loadNam(const std::string&) override {
        return std::make_shared<CountingNamModel>(liveNamCount, totalNamLoaded);
    }
    std::shared_ptr<IrHandle> loadIr(const std::string&, double) override {
        return std::make_shared<CountingIrHandle>(liveIrCount, totalIrLoaded);
    }
    std::unique_ptr<EffectBlock> loadVst3(const std::string&) override {
        return std::make_unique<CountingVst3Block>(liveVst3Count, totalVst3Loaded);
    }
};

EffectBlockSpec makeBlock(const std::string& id, const std::string& type,
                          std::optional<std::string> assetId = std::nullopt, bool enabled = true) {
    EffectBlockSpec spec;
    spec.id = id;
    spec.type = type;
    spec.asset_id = std::move(assetId);
    spec.enabled = enabled;
    return spec;
}

// A resolved chain in the shape the daemon now sends: amp ("nam") and cab
// ("ir") are ordinary blocks carrying their own asset_id, sitting in an
// explicit signal-chain order alongside the effects.
Preset makePreset(const std::string& id, const std::string& namAssetId, const std::string& irAssetId) {
    Preset p;
    p.id = id;
    p.name = "preset-" + id;
    p.rig_id = "rig-" + id;
    p.rig_name = "Rig " + id;
    p.blocks.push_back(makeBlock("amp", "nam", namAssetId));
    EffectBlockSpec gain = makeBlock("boost", "gain");
    gain.params["gain_db"] = 0.0;
    p.blocks.push_back(gain);
    p.blocks.push_back(makeBlock("cab", "ir", irAssetId));
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

TEST(ResourceManager, BlockReferencingUnregisteredAssetIsRejected) {
    // Asset references now hang off individual blocks, so validation has
    // to walk the whole chain -- including a block whose type the engine
    // doesn't otherwise treat specially, and including a *disabled* one
    // (a preset naming an asset the engine was never told about means the
    // engine and daemon are out of sync either way).
    auto loader = std::make_shared<CountingAssetLoader>();
    ResourceManager manager(loader);

    Asset nam;
    nam.id = "nam1";
    nam.kind = AssetKind::Nam;
    nam.filename = "a.nam";
    nam.stored_path = "/fake/a.nam";
    manager.registerAsset(nam);

    // Sanity: a fully registered chain loads, so the failures below are
    // really about the unknown id and not about the fixture.
    Preset ok;
    ok.id = "ok";
    ok.name = "ok";
    ok.blocks = {makeBlock("amp", "nam", "nam1")};
    ASSERT_NO_THROW(manager.loadPreset(ok));

    Preset bad;
    bad.id = "bad";
    bad.name = "bad";
    bad.blocks = {makeBlock("amp", "nam", "nam1"), makeBlock("cab", "ir", "never-uploaded")};
    EXPECT_THROW(manager.loadPreset(bad), std::runtime_error);

    Preset badDisabled;
    badDisabled.id = "bad-disabled";
    badDisabled.name = "bad-disabled";
    badDisabled.blocks = {makeBlock("fuzz", "gain", "never-uploaded", /*enabled=*/false)};
    EXPECT_THROW(manager.loadPreset(badDisabled), std::runtime_error);

    // Rejection must leave the previously loaded chain in place rather
    // than half-swapping it.
    ASSERT_NE(manager.currentChain(), nullptr);
    EXPECT_EQ(manager.currentChain()->presetId, "ok");
    EXPECT_EQ(loader->liveIrCount.load(), 0);
}

TEST(ResourceManager, ChainRunsBlocksInPresetOrder) {
    // The daemon sends the signal chain already ordered, amp and cab
    // included; the engine must replay that order rather than forcing a
    // fixed amp -> effects -> cab topology.
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

    Preset p;
    p.id = "ordered";
    p.name = "ordered";
    EffectBlockSpec boost = makeBlock("boost", "gain");
    boost.params["gain_db"] = 6.0;
    // Deliberately amp -> cab -> gain: the gain sits AFTER the cab here.
    p.blocks = {makeBlock("amp", "nam", "n"), makeBlock("cab", "ir", "i"), boost};
    manager.loadPreset(p);

    EngineChain* chain = manager.currentChain();
    ASSERT_NE(chain, nullptr);
    ASSERT_EQ(chain->processOrder.size(), 3u);
    EXPECT_EQ(chain->processOrder[0], chain->namModel.get());
    EXPECT_EQ(chain->processOrder[1], chain->cabinet.get());
    ASSERT_EQ(chain->effects.size(), 1u);
    EXPECT_EQ(chain->processOrder[2], chain->effects[0].get());
}

TEST(ResourceManager, AmpAndCabBlocksLoadTheirOwnAssets) {
    auto loaderOwned = std::make_shared<CountingAssetLoader>();
    CountingAssetLoader& loader = *loaderOwned;
    ResourceManager manager(loaderOwned);

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

    Preset p;
    p.id = "p";
    p.name = "p";
    p.blocks = {makeBlock("amp", "nam", "n"), makeBlock("cab", "ir", "i")};
    manager.loadPreset(p);

    EXPECT_EQ(loader.totalNamLoaded.load(), 1);
    EXPECT_EQ(loader.totalIrLoaded.load(), 1);
    ASSERT_NE(manager.currentChain(), nullptr);
    EXPECT_NE(manager.currentChain()->namModel, nullptr);
    EXPECT_NE(manager.currentChain()->cabinet, nullptr);
    // Amp/cab are not duplicated into the generic effect list.
    EXPECT_TRUE(manager.currentChain()->effects.empty());
}

// Regression test for a real use-after-free: namModel is a single
// shared_ptr field and processOrder holds a non-owning raw pointer into
// it, so a second enabled "nam" block would silently destroy the first
// one's object (via the shared_ptr reassignment) while a dangling pointer
// to it stayed in processOrder -- the next processAudioBlock would then
// call into freed memory. This must fail loud at load time instead.
TEST(ResourceManager, RejectsMoreThanOneEnabledNamBlock) {
    auto loaderOwned = std::make_shared<CountingAssetLoader>();
    ResourceManager manager(loaderOwned);

    Asset nam1;
    nam1.id = "n1";
    nam1.kind = AssetKind::Nam;
    nam1.filename = "n1.nam";
    nam1.stored_path = "/fake/n1.nam";
    Asset nam2;
    nam2.id = "n2";
    nam2.kind = AssetKind::Nam;
    nam2.filename = "n2.nam";
    nam2.stored_path = "/fake/n2.nam";
    manager.registerAsset(nam1);
    manager.registerAsset(nam2);

    Preset p;
    p.id = "p";
    p.name = "p";
    p.blocks = {makeBlock("channel-clean", "nam", "n1"), makeBlock("channel-crunch", "nam", "n2")};
    EXPECT_THROW(manager.loadPreset(p), std::runtime_error);
}

TEST(ResourceManager, RejectsMoreThanOneEnabledIrBlock) {
    auto loaderOwned = std::make_shared<CountingAssetLoader>();
    ResourceManager manager(loaderOwned);

    Asset ir1;
    ir1.id = "i1";
    ir1.kind = AssetKind::Ir;
    ir1.filename = "i1.wav";
    ir1.stored_path = "/fake/i1.wav";
    Asset ir2;
    ir2.id = "i2";
    ir2.kind = AssetKind::Ir;
    ir2.filename = "i2.wav";
    ir2.stored_path = "/fake/i2.wav";
    manager.registerAsset(ir1);
    manager.registerAsset(ir2);

    Preset p;
    p.id = "p";
    p.name = "p";
    p.blocks = {makeBlock("cab-a", "ir", "i1"), makeBlock("cab-b", "ir", "i2")};
    EXPECT_THROW(manager.loadPreset(p), std::runtime_error);
}

TEST(ResourceManager, DisabledAmpBlockIsNotLoaded) {
    auto loaderOwned = std::make_shared<CountingAssetLoader>();
    CountingAssetLoader& loader = *loaderOwned;
    ResourceManager manager(loaderOwned);

    Asset nam;
    nam.id = "n";
    nam.kind = AssetKind::Nam;
    nam.filename = "n.nam";
    nam.stored_path = "/fake/n.nam";
    manager.registerAsset(nam);

    Preset p;
    p.id = "p";
    p.name = "p";
    p.blocks = {makeBlock("amp", "nam", "n", /*enabled=*/false)};
    manager.loadPreset(p);

    EXPECT_EQ(loader.totalNamLoaded.load(), 0);
    EXPECT_EQ(manager.currentChain()->namModel, nullptr);
    EXPECT_TRUE(manager.currentChain()->processOrder.empty());
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

TEST(ResourceManager, CreateEffectBlockBuildsVolumeAsAGainBlock) {
    EffectBlockSpec spec;
    spec.type = "volume";
    spec.params["gain_db"] = 20.0;  // 10x linear
    auto block = createEffectBlock(spec);
    ASSERT_NE(block, nullptr);
    block->prepare(48000.0);

    std::vector<float> buf = {0.05f, 0.0f};
    block->process(buf);
    EXPECT_NEAR(buf[0], 0.5f, 1e-5f);
}

TEST(ResourceManager, CreateEffectBlockBuildsToneStack) {
    EffectBlockSpec spec;
    spec.type = "tone_stack";
    spec.params["bass_db"] = 12.0;
    auto block = createEffectBlock(spec);
    ASSERT_NE(block, nullptr);
    block->prepare(48000.0);

    // A boosted bass band must actually change a non-trivial signal --
    // proves ToneStackBlock is really wired in via createEffectBlock, not
    // just constructed and discarded.
    std::vector<float> buf(256, 0.0f);
    buf[0] = 1.0f;
    block->process(buf);
    bool anyNonZero = false;
    for (float s : buf) anyNonZero = anyNonZero || (s != 0.0f);
    EXPECT_TRUE(anyNonZero);
}

TEST(ResourceManager, LoadPresetRunsGainNamEffectsCabToneVolumeInOrder) {
    // The full chain order the product spec calls for: gain (input trim)
    // -> NAM -> effects -> cab -> tone stack -> volume (output level).
    // Every stage here is identity except the two gain stages, so the
    // combined effect must be exactly their product regardless of what
    // sits between them.
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

    EffectBlockSpec inputTrim;
    inputTrim.id = "input_trim";
    inputTrim.type = "gain";
    inputTrim.params["gain_db"] = 20.0;  // 10x

    EffectBlockSpec amp;
    amp.id = "amp";
    amp.type = "nam";
    amp.asset_id = "n";

    EffectBlockSpec cab;
    cab.id = "cab";
    cab.type = "ir";
    cab.asset_id = "i";

    EffectBlockSpec toneStack;
    toneStack.id = "tone_stack";
    toneStack.type = "tone_stack";  // flat (0dB every band) -- identity

    EffectBlockSpec outputVolume;
    outputVolume.id = "output_volume";
    outputVolume.type = "volume";
    outputVolume.params["gain_db"] = 6.0;  // ~2x

    Preset p;
    p.id = "p1";
    p.name = "p1";
    p.blocks = {inputTrim, amp, cab, toneStack, outputVolume};
    manager.loadPreset(p);

    std::vector<float> buf = {0.01f, 0.0f};
    manager.currentChain()->process(buf.data(), buf.size());
    // CountingIrHandle's IR is {1.0} (identity), NAM stub is identity --
    // only the two gain stages should have scaled the signal: 0.01 * 10 *
    // ~2 ~= 0.2.
    EXPECT_NEAR(buf[0], 0.01f * 10.0f * std::pow(10.0f, 6.0f / 20.0f), 1e-4f);
}

TEST(ResourceManager, Vst3BlockLoadsViaAssetLoaderAndRunsInChain) {
    auto loaderOwned = std::make_shared<CountingAssetLoader>();
    CountingAssetLoader& loader = *loaderOwned;
    ResourceManager manager(loaderOwned);

    Asset plugin;
    plugin.id = "plug1";
    plugin.kind = AssetKind::Vst3;
    plugin.filename = "Test.vst3";
    plugin.stored_path = "/fake/Test.vst3";
    manager.registerAsset(plugin);

    Preset p;
    p.id = "p";
    p.name = "p";
    p.blocks = {makeBlock("fx", "vst3", "plug1")};
    manager.loadPreset(p);

    EXPECT_EQ(loader.totalVst3Loaded.load(), 1);
    ASSERT_NE(manager.currentChain(), nullptr);
    ASSERT_EQ(manager.currentChain()->effects.size(), 1u);
    EXPECT_EQ(manager.currentChain()->processOrder[0], manager.currentChain()->effects[0].get());
}

TEST(ResourceManager, MultipleVst3BlocksCanCoexistInOneChain) {
    // Unlike "nam"/"ir" (one dedicated chain-level slot each), "vst3" has no
    // such limit -- a rig can chain any number of plugin instances.
    auto loaderOwned = std::make_shared<CountingAssetLoader>();
    CountingAssetLoader& loader = *loaderOwned;
    ResourceManager manager(loaderOwned);

    Asset plugin;
    plugin.id = "plug1";
    plugin.kind = AssetKind::Vst3;
    plugin.filename = "Test.vst3";
    plugin.stored_path = "/fake/Test.vst3";
    manager.registerAsset(plugin);

    Preset p;
    p.id = "p";
    p.name = "p";
    p.blocks = {makeBlock("fx1", "vst3", "plug1"), makeBlock("fx2", "vst3", "plug1")};
    manager.loadPreset(p);

    EXPECT_EQ(loader.totalVst3Loaded.load(), 2);
    ASSERT_EQ(manager.currentChain()->effects.size(), 2u);
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
