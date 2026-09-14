#include "audio_engine/resource_manager.hpp"

#include <algorithm>
#include <stdexcept>

#include "audio_engine/delay_block.hpp"
#include "audio_engine/eq_block.hpp"
#include "audio_engine/gain_block.hpp"
#include "audio_engine/passthrough_block.hpp"
#include "audio_engine/wav_file.hpp"

#ifdef AUDIO_ENGINE_WITH_REAL_NAM
#include <filesystem>

#include <NAM/get_dsp.h>

#include "audio_engine/real_nam_model.hpp"
#endif

namespace audio_engine {

void EngineChain::process(float* buffer, std::size_t numSamples) {
    if (namModel) namModel->process(buffer, numSamples);
    for (auto& block : effects) block->process(buffer, numSamples);
    if (cabinet) cabinet->process(buffer, numSamples);

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
    if (spec.type == "gain") return std::make_unique<GainBlock>(spec.params);
    if (spec.type == "eq") return std::make_unique<EqBlock>(spec.params);
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

ResourceManager::ResourceManager(std::shared_ptr<IAssetLoader> loader, double sampleRate)
    : loader_(std::move(loader)), sampleRate_(sampleRate) {}

void ResourceManager::registerAsset(const Asset& asset) { assets_[asset.id] = asset; }

void ResourceManager::loadPreset(const Preset& preset) {
    auto chain = std::make_unique<EngineChain>();
    chain->presetId = preset.id;

    if (preset.nam_asset_id.has_value()) {
        auto it = assets_.find(*preset.nam_asset_id);
        if (it == assets_.end()) {
            throw std::runtime_error("preset '" + preset.id + "' references unknown nam_asset_id '" +
                                      *preset.nam_asset_id + "'");
        }
        chain->namModel = loader_->loadNam(it->second.stored_path);
    }

    if (preset.ir_asset_id.has_value()) {
        auto it = assets_.find(*preset.ir_asset_id);
        if (it == assets_.end()) {
            throw std::runtime_error("preset '" + preset.id + "' references unknown ir_asset_id '" +
                                      *preset.ir_asset_id + "'");
        }
        chain->ir = loader_->loadIr(it->second.stored_path, sampleRate_);
        chain->cabinet = std::make_unique<ConvolutionEngine>(chain->ir->samples);
    }

    chain->effects.reserve(preset.blocks.size());
    for (const auto& blockSpec : preset.blocks) {
        if (!blockSpec.enabled) continue;
        chain->effects.push_back(createEffectBlock(blockSpec));
    }

    chain->prepare(sampleRate_);

    // Replacing this pointer drops the old chain's shared_ptrs (namModel,
    // ir) here; if nothing else holds a reference, their destructors run
    // synchronously, before this call returns -- see
    // test_resource_manager.cpp.
    current_ = std::move(chain);
}

}  // namespace audio_engine
