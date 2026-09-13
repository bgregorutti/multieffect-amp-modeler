// Owns the "currently loaded" preset's resources: one NAM model, one
// cabinet IR, one effect chain -- and only one, per the product spec's V1
// scope ("running multiple simulations in parallel is explicitly out of
// scope for V1") and the shared 8GB-Pi RAM budget documented in
// ARCHITECTURE.md. Loading a new preset must actually release the
// previous preset's resources, not accumulate them -- see
// test_resource_manager.cpp, which asserts this with an instance-counting
// loader test double rather than just trusting it.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio_engine/convolution.hpp"
#include "audio_engine/effect_block.hpp"
#include "audio_engine/nam_model.hpp"
#include "audio_engine/preset_model.hpp"

namespace audio_engine {

// Holds cabinet IR sample data. A distinct (polymorphic, so its
// construction/destruction is observable by a test double) type rather
// than a bare std::vector<float>, specifically so tests can subclass it to
// count live instances -- see CountingAssetLoader in
// test_resource_manager.cpp.
struct IrHandle {
    std::vector<float> samples;
    virtual ~IrHandle() = default;
};

// Seam for loading the two binary asset kinds a preset can reference.
// Real loading (FileAssetLoader) reads from disk via wav_file.hpp /
// nam_model.hpp; tests substitute a counting/failing double without
// touching the filesystem.
class IAssetLoader {
public:
    virtual ~IAssetLoader() = default;
    virtual std::shared_ptr<INamModel> loadNam(const std::string& storedPath) = 0;
    virtual std::shared_ptr<IrHandle> loadIr(const std::string& storedPath) = 0;
};

// Reads real files from disk: `storedPath` for a "nam" asset is parsed via
// parseNamModelFile()+StubNamModel (see nam_model.hpp -- inference is
// still stubbed, only metadata parsing is real); `storedPath` for an "ir"
// asset is parsed via loadImpulseResponseFile() (wav_file.hpp).
class FileAssetLoader : public IAssetLoader {
public:
    std::shared_ptr<INamModel> loadNam(const std::string& storedPath) override;
    std::shared_ptr<IrHandle> loadIr(const std::string& storedPath) override;
};

// Builds one EffectBlock from a preset's EffectBlockSpec. Recognized
// `type` values: "gain", "eq", "delay", "passthrough". An unrecognized
// type falls back to a PassthroughBlock (fail safe rather than fail
// closed -- a preset referencing a not-yet-implemented block type still
// loads and plays, just without that block's processing).
std::unique_ptr<EffectBlock> createEffectBlock(const EffectBlockSpec& spec);

// The fully-prepared, ready-to-run resources for one preset: the NAM
// model (possibly null if the preset has no nam_asset_id), the cabinet IR
// (possibly null likewise), and the ordered effect chain built from
// preset.blocks. Every EffectBlock (including the NAM model) has already
// had prepare(sampleRate) called on it -- an EngineChain is meant to be
// fully "preloaded" before it's handed to a PresetSwitcher crossfade (see
// preset_switcher.hpp).
struct EngineChain {
    std::string presetId;
    std::shared_ptr<INamModel> namModel;         // null if preset has no nam_asset_id
    std::shared_ptr<IrHandle> ir;                 // null if preset has no ir_asset_id
    std::unique_ptr<ConvolutionEngine> cabinet;   // built from ir->samples; null iff ir is null
    std::vector<std::unique_ptr<EffectBlock>> effects;  // from preset.blocks, in order

    // Runs the whole chain over `buffer` in place, in signal-path order:
    // NAM model (amp/preamp sim) -> preset.blocks effects, in order ->
    // cabinet IR convolution. Suitable for adapting directly to a
    // PresetSwitcher::ChainFn via a lambda capturing `this`.
    void process(float* buffer, std::size_t numSamples);
    void prepare(double sampleRate);
};

class ResourceManager {
public:
    explicit ResourceManager(std::shared_ptr<IAssetLoader> loader, double sampleRate = 48000.0);

    // Registers (or replaces) metadata for one uploaded asset -- mirrors
    // control-daemon's `register_asset` WS command; the engine never
    // resolves an asset id it hasn't been told about.
    void registerAsset(const Asset& asset);
    const std::map<std::string, Asset>& assets() const { return assets_; }

    // Builds a brand new EngineChain for `preset` (loading its nam/ir
    // assets, if referenced and registered, and constructing its effect
    // blocks), then atomically replaces the current chain. The old
    // chain's shared_ptrs are dropped as part of this call, so if nothing
    // else is holding a reference (the expected steady-state case), its
    // NAM model / IR resources are released synchronously, before this
    // call returns.
    //
    // Throws PresetParseError (via preset validation) is not done here --
    // callers pass an already-parsed Preset; throws std::out_of_range /
    // a descriptive std::runtime_error if nam_asset_id/ir_asset_id is set
    // but not a registered asset id.
    void loadPreset(const Preset& preset);

    // The currently active chain, or nullptr if nothing has been loaded
    // yet. Non-owning raw pointer: lifetime is owned by ResourceManager.
    EngineChain* currentChain() const { return current_.get(); }

    double sampleRate() const { return sampleRate_; }

private:
    std::shared_ptr<IAssetLoader> loader_;
    std::map<std::string, Asset> assets_;
    std::unique_ptr<EngineChain> current_;
    double sampleRate_;
};

}  // namespace audio_engine
