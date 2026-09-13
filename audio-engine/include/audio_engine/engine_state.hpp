// Dispatches one control-socket command (see control_socket.hpp for the
// wire protocol) against a ResourceManager + the small bit of extra state
// (bypass, crossfade duration, active preset id) that isn't a "resource".
// Kept separate from ControlSocketServer (the transport) so command
// handling can be unit tested without opening any socket at all.
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "audio_engine/resource_manager.hpp"

namespace audio_engine {

// Mirrors control-daemon's WS error code style (`code` + `message`) -- see
// control-daemon/README.md "error" message shape.
enum class EngineErrorCode { ValidationError, NotFound, InternalError, UnknownCommand };
std::string toString(EngineErrorCode code);

class EngineState {
public:
    explicit EngineState(std::shared_ptr<IAssetLoader> loader, double sampleRate = 48000.0);

    // Parses and executes one command object (already-parsed JSON, one
    // line's worth). Returns the reply to send back -- either
    // {"ok": true, "cmd": ..., ...} or {"ok": false, "code": ..., "message": ...}.
    // Never throws: any exception from preset parsing / resource loading
    // is caught and turned into an "ok": false reply.
    nlohmann::json handleCommand(const nlohmann::json& command);

    // --- introspection, used by get_state and directly by tests ---
    bool bypass() const;
    int crossfadeMs() const;
    std::optional<std::string> currentPresetId() const;
    double sampleRate() const { return resourceManager_.sampleRate(); }
    ResourceManager& resourceManager() { return resourceManager_; }

private:
    nlohmann::json makeError(EngineErrorCode code, const std::string& message) const;
    nlohmann::json handleLoadPreset(const nlohmann::json& command);
    nlohmann::json handleSetBypass(const nlohmann::json& command);
    nlohmann::json handleCrossfadeMs(const nlohmann::json& command);
    nlohmann::json handleRegisterAsset(const nlohmann::json& command);
    nlohmann::json handleGetState() const;

    mutable std::mutex mutex_;
    ResourceManager resourceManager_;
    bool bypass_ = false;
    int crossfadeMs_ = 50;
    std::optional<std::string> currentPresetId_;
};

}  // namespace audio_engine
