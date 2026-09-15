#include "audio_engine/resource_manager.hpp"

#include <algorithm>
#include <stdexcept>

#include "audio_engine/delay_block.hpp"
#include "audio_engine/eq_block.hpp"
#include "audio_engine/gain_block.hpp"
#include "audio_engine/passthrough_block.hpp"
#include "audio_engine/tone_stack_block.hpp"
#include "audio_engine/wav_file.hpp"

#ifdef AUDIO_ENGINE_WITH_REAL_NAM
#include <filesystem>

#include <NAM/get_dsp.h>

#include "audio_engine/real_nam_model.hpp"
#endif

namespace audio_engine {

void EngineChain::process(float* buffer, std::size_t numSamples) {
    // `processOrder` is the preset's own block order (amp and cab
    // included), not a fixed amp -> effects -> cab topology: the daemon
    // sends a fully resolved chain whose ordering is a user-visible
    // decision (a delay before vs. after the cab is audibly different), so
    // the engine replays exactly what it was given.
    for (EffectBlock* block : processOrder) block->process(buffer, numSamples);

    // Defensive final clamp, not a design assumption that anything above
    // is broken: cabinet-IR energy normalization (convolution.hpp) makes
    // clipping rare rather than impossible (a resonant IR can still peak
    // above unity on a loud transient -- see that header's comment), and
    // nothing stops a user-authored preset from stacking enough gain/EQ
    // boost to do the same. Scoped to the whole chain's final output, not
    // to individual EffectBlocks -- each block stays free to produce
    // whatever an isolated unit test expects; only what would actually
    // reach the audio device gets bounded.
    for (std::size_t i = 0; i < numSamples; ++i) {
        buffer[i] = std::clamp(buffer[i], -1.0f, 1.0f);
    }
}

void EngineChain::prepare(double sampleRate) {
    if (namModel) namModel->prepare(sampleRate);
    for (auto& block : effects) block->prepare(sampleRate);
    if (cabinet) cabinet->prepare(sampleRate);
}

std::unique_ptr<EffectBlock> createEffectBlock(const EffectBlockSpec& spec) {
    // "gain" and "volume" are the same GainBlock DSP under two distinct
    // type names, so a rig chain (and its UI) can tell an input-trim-style
    // gain stage apart from an output-level one at a glance, even though
    // the underlying processing is identical. See tone_stack_block.hpp for
    // "tone_stack".
    if (spec.type == "gain") return std::make_unique<GainBlock>(spec.params);
    if (spec.type == "volume") return std::make_unique<GainBlock>(spec.params);
    if (spec.type == "eq") return std::make_unique<EqBlock>(spec.params);
    if (spec.type == "tone_stack") return std::make_unique<ToneStackBlock>(spec.params);
    if (spec.type == "delay") return std::make_unique<DelayBlock>(spec.params);
    // "passthrough" and any unrecognized type: fail safe, not fail closed.
    return std::make_unique<PassthroughBlock>();
}

std::shared_ptr<INamModel> FileAssetLoader::loadNam(const std::string& storedPath) {
    NamModelMetadata meta = parseNamModelFile(storedPath);
#ifdef AUDIO_ENGINE_WITH_REAL_NAM
    // nam::get_dsp throws nam::NamFileValidationError (a std::runtime_error)
    // on a file its own, more thorough validation rejects -- deliberately
    // not caught/translated here, same as parseNamModelFile's NamParseError
    // above: both propagate to ResourceManager::loadPreset's caller
    // (EngineState::handleLoadPreset), which already turns any exception
    // into a typed control-socket error reply.
    // Explicit std::filesystem::path: a bare std::string is ambiguous
    // between get_dsp's path overload and its nlohmann::json overload
    // (json has an implicit string constructor).
    auto dsp = nam::get_dsp(std::filesystem::path(storedPath));
    return std::make_shared<RealNamModel>(std::move(meta), std::move(dsp));
#else
    return std::make_shared<StubNamModel>(std::move(meta));
#endif
}

std::shared_ptr<IrHandle> FileAssetLoader::loadIr(const std::string& storedPath, double targetSampleRate) {
    auto handle = std::make_shared<IrHandle>();
    handle->samples = loadImpulseResponseFile(storedPath, targetSampleRate);
    return handle;
}

std::unique_ptr<EffectBlock> FileAssetLoader::loadVst3(const std::string& storedPath) {
    return std::make_unique<Vst3EffectBlock>(loadVst3Plugin(storedPath));
}

ResourceManager::ResourceManager(std::shared_ptr<IAssetLoader> loader, double sampleRate)
    : loader_(std::move(loader)), sampleRate_(sampleRate) {}

void ResourceManager::registerAsset(const Asset& asset) { assets_[asset.id] = asset; }

void ResourceManager::loadPreset(const Preset& preset) {
    auto chain = std::make_unique<EngineChain>();
    chain->presetId = preset.id;

    // Asset references live on the blocks now (the daemon's ResolvedBlock
    // carries its own asset_id), so validate every one of them up front --
    // including blocks that are currently disabled, since a preset that
    // names an asset the engine was never told about is a desync with the
    // daemon regardless of whether that block happens to be switched on.
    // Doing it before any loading also means an unknown id leaves the
    // previously loaded chain untouched rather than half-replaced.
    for (const auto& blockSpec : preset.blocks) {
        if (!blockSpec.asset_id.has_value()) continue;
        if (assets_.find(*blockSpec.asset_id) == assets_.end()) {
            throw std::runtime_error("preset '" + preset.id + "' block '" + blockSpec.id +
                                     "' references unknown asset_id '" + *blockSpec.asset_id + "'");
        }
    }

    chain->effects.reserve(preset.blocks.size());
    chain->processOrder.reserve(preset.blocks.size());
    for (const auto& blockSpec : preset.blocks) {
        if (!blockSpec.enabled) continue;

        // "nam" and "ir" blocks are the amp and the cab: they're ordinary
        // chain positions, but their processing comes from a loaded binary
        // asset rather than from createEffectBlock(). A block of either
        // type with no asset_id has nothing to play through, so it's
        // simply skipped (fail safe, same spirit as the unknown-type
        // fallback in createEffectBlock).
        if (blockSpec.type == "nam") {
            if (!blockSpec.asset_id.has_value()) continue;
            const Asset& asset = assets_.at(*blockSpec.asset_id);
            chain->namModel = loader_->loadNam(asset.stored_path);
            if (chain->namModel) chain->processOrder.push_back(chain->namModel.get());
            continue;
        }
        if (blockSpec.type == "ir") {
            if (!blockSpec.asset_id.has_value()) continue;
            const Asset& asset = assets_.at(*blockSpec.asset_id);
            chain->ir = loader_->loadIr(asset.stored_path, sampleRate_);
            if (!chain->ir) continue;
            chain->cabinet = std::make_unique<ConvolutionEngine>(chain->ir->samples);
            chain->processOrder.push_back(chain->cabinet.get());
            continue;
        }

        // "vst3" is an ordinary chain position like any effect -- unlike
        // "nam"/"ir" it can appear any number of times in one rig, so (unlike
        // those) it has no dedicated EngineChain field and just lands in
        // `effects` alongside gain/eq/delay blocks.
        if (blockSpec.type == "vst3") {
            if (!blockSpec.asset_id.has_value()) continue;
            const Asset& asset = assets_.at(*blockSpec.asset_id);
            chain->effects.push_back(loader_->loadVst3(asset.stored_path));
            chain->processOrder.push_back(chain->effects.back().get());
            continue;
        }

        chain->effects.push_back(createEffectBlock(blockSpec));
        chain->processOrder.push_back(chain->effects.back().get());
    }

    chain->prepare(sampleRate_);

    // Replacing this pointer drops the old chain's shared_ptrs (namModel,
    // ir) here; if nothing else holds a reference, their destructors run
    // synchronously, before this call returns -- see
    // test_resource_manager.cpp.
    current_ = std::move(chain);
}

}  // namespace audio_engine
