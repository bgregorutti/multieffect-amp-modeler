// Dispatches one control-socket command (see control_socket.hpp for the
// wire protocol) against a ResourceManager + the small bit of extra state
// (bypass, crossfade duration, active preset id) that isn't a "resource".
// Kept separate from ControlSocketServer (the transport) so command
// handling can be unit tested without opening any socket at all.
#pragma once

#include <cstddef>
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

    // Runs the currently loaded chain over `buffer` in place (`numSamples`
    // valid samples), or leaves it untouched (passthrough) if bypassed or
    // if no preset has been loaded yet. Meant to be called once per block
    // from a real-time audio callback (see IAudioIoBackend/main.cpp) --
    // takes the same mutex as handleCommand so a control-socket command
    // (e.g. load_preset) can never race a concurrent audio block. Real-time
    // caveat: EngineChain::process performs no allocation/I/O, but this
    // mutex lock is not itself real-time-safe (a command could in theory
    // hold it briefly); acceptable for dev/test use on a normal OS
    // scheduler, flagged here rather than hidden -- see README.md "Known
    // limitations" for the production follow-up (a lock-free handoff).
    void processAudioBlock(float* buffer, std::size_t numSamples);

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
    nlohmann::json handleSetBlockParam(const nlohmann::json& command);
    nlohmann::json handleListBlockTypes() const;
    nlohmann::json handleGetState() const;

    mutable std::mutex mutex_;
    ResourceManager resourceManager_;
    bool bypass_ = false;
    int crossfadeMs_ = 50;
    std::optional<std::string> currentPresetId_;
};

}  // namespace audio_engine
