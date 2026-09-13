#include "audio_engine/engine_state.hpp"

#include "audio_engine/nam_model.hpp"
#include "audio_engine/wav_file.hpp"

namespace audio_engine {

using nlohmann::json;

std::string toString(EngineErrorCode code) {
    switch (code) {
        case EngineErrorCode::ValidationError:
            return "validation_error";
        case EngineErrorCode::NotFound:
            return "not_found";
        case EngineErrorCode::InternalError:
            return "internal_error";
        case EngineErrorCode::UnknownCommand:
            return "unknown_command";
    }
    return "internal_error";
}

EngineState::EngineState(std::shared_ptr<IAssetLoader> loader, double sampleRate)
    : resourceManager_(std::move(loader), sampleRate) {}

bool EngineState::bypass() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bypass_;
}

int EngineState::crossfadeMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return crossfadeMs_;
}

std::optional<std::string> EngineState::currentPresetId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentPresetId_;
}

json EngineState::makeError(EngineErrorCode code, const std::string& message) const {
    return json{{"ok", false}, {"code", toString(code)}, {"message", message}};
}

json EngineState::handleCommand(const json& command) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!command.is_object()) {
        return makeError(EngineErrorCode::ValidationError, "command must be a JSON object");
    }
    auto cmdIt = command.find("cmd");
    if (cmdIt == command.end() || !cmdIt->is_string()) {
        return makeError(EngineErrorCode::ValidationError, "missing required string field 'cmd'");
    }
    const std::string cmd = cmdIt->get<std::string>();

    try {
        if (cmd == "load_preset") return handleLoadPreset(command);
        if (cmd == "set_bypass") return handleSetBypass(command);
        if (cmd == "crossfade_ms") return handleCrossfadeMs(command);
        if (cmd == "register_asset") return handleRegisterAsset(command);
        if (cmd == "get_state") return handleGetState();
    } catch (const PresetParseError& e) {
        return makeError(EngineErrorCode::ValidationError, e.what());
    } catch (const NamParseError& e) {
        return makeError(EngineErrorCode::ValidationError, e.what());
    } catch (const WavParseError& e) {
        return makeError(EngineErrorCode::ValidationError, e.what());
    } catch (const json::exception& e) {
        return makeError(EngineErrorCode::ValidationError, e.what());
    } catch (const std::exception& e) {
        return makeError(EngineErrorCode::InternalError, e.what());
    }

    return makeError(EngineErrorCode::UnknownCommand, "unknown command '" + cmd + "'");
}

json EngineState::handleLoadPreset(const json& command) {
    auto presetIt = command.find("preset");
    if (presetIt == command.end() || !presetIt->is_object()) {
        return makeError(EngineErrorCode::ValidationError, "load_preset requires an object field 'preset'");
    }
    Preset preset = presetIt->get<Preset>();  // throws PresetParseError on malformed shape
    resourceManager_.loadPreset(preset);      // throws std::runtime_error for unknown asset ids
    currentPresetId_ = preset.id;
    return json{{"ok", true}, {"cmd", "load_preset"}, {"preset_id", preset.id}};
}

json EngineState::handleSetBypass(const json& command) {
    auto it = command.find("bypass");
    if (it == command.end() || !it->is_boolean()) {
        return makeError(EngineErrorCode::ValidationError, "set_bypass requires a boolean field 'bypass'");
    }
    bypass_ = it->get<bool>();
    return json{{"ok", true}, {"cmd", "set_bypass"}, {"bypass", bypass_}};
}

json EngineState::handleCrossfadeMs(const json& command) {
    auto it = command.find("value");
    if (it == command.end() || !it->is_number()) {
        return makeError(EngineErrorCode::ValidationError, "crossfade_ms requires a numeric field 'value'");
    }
    double value = it->get<double>();
    if (value < 0) {
        return makeError(EngineErrorCode::ValidationError, "crossfade_ms 'value' must be >= 0");
    }
    crossfadeMs_ = static_cast<int>(value);
    return json{{"ok", true}, {"cmd", "crossfade_ms"}, {"value", crossfadeMs_}};
}

json EngineState::handleRegisterAsset(const json& command) {
    auto it = command.find("asset");
    if (it == command.end() || !it->is_object()) {
        return makeError(EngineErrorCode::ValidationError, "register_asset requires an object field 'asset'");
    }
    Asset asset = it->get<Asset>();  // throws PresetParseError on malformed shape
    resourceManager_.registerAsset(asset);
    return json{{"ok", true}, {"cmd", "register_asset"}, {"asset_id", asset.id}};
}

json EngineState::handleGetState() const {
    json state = {
        {"bypass", bypass_},
        {"crossfade_ms", crossfadeMs_},
        {"current_preset_id", currentPresetId_.has_value() ? json(*currentPresetId_) : json(nullptr)},
        {"sample_rate", resourceManager_.sampleRate()},
        {"registered_assets", resourceManager_.assets().size()},
    };
    return json{{"ok", true}, {"cmd", "get_state"}, {"state", state}};
}

}  // namespace audio_engine
