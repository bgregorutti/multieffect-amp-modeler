// Hosting one instantiated VST3 plugin, kept behind a narrow interface --
// same "narrow interface + swappable backend" pattern as INamModel/
// IAssetLoader, so tests and the rest of the engine never touch the raw
// VST3 SDK (IPluginFactory/IComponent/IAudioProcessor/IEditController)
// directly.
//
// IHostedPlugin and Vst3EffectBlock compile unconditionally -- they don't
// depend on the vendored SDK headers at all. Only loadVst3Plugin()'s real
// implementation (vst3_plugin_host.cpp) needs
// -DAUDIO_ENGINE_WITH_VST3=ON (see CMakeLists.txt "VST3 plugin hosting");
// with the flag off, a stub implementation (vst3_plugin_host_stub.cpp)
// throws a clear "not built with VST3 support" error instead of silently
// pretending to host a plugin.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "audio_engine/effect_block.hpp"

namespace audio_engine {

// One loaded, instantiated VST3 plugin (component + audio processor, and
// an edit controller if the plugin exposes adjustable parameters). Mono
// in, mono out from the engine's point of view -- see vst3_plugin_host.cpp
// for how a stereo-only plugin's bus layout is bridged to that.
class IHostedPlugin {
public:
    // Metadata for one plugin parameter, in the plugin's own plain units
    // (not normalized [0,1]) -- min/max/default are derived from the
    // plugin's own normalized<->plain conversion, the same trick any VST3
    // host UI uses to build a real-world-unit slider. `key` is the
    // stringified VST3 ParamID; stepCount is 0 for a continuous parameter.
    struct ParameterDescriptor {
        std::string key;
        std::string label;
        std::string unit;
        double minValue = 0.0;
        double maxValue = 1.0;
        double defaultValue = 0.0;
        int stepCount = 0;
    };

    virtual ~IHostedPlugin() = default;

    // (Re)configures the plugin for this sample rate. Safe to call again
    // if the sample rate changes, same contract as EffectBlock::prepare.
    virtual void prepare(double sampleRate) = 0;

    // Runs `numSamples` samples through the plugin in place.
    virtual void process(float* buffer, std::size_t numSamples) = 0;

    // Clears the plugin's internal state (delay lines, reverb tails, ...).
    virtual void reset() = 0;

    // The plugin's own parameters, queried from its edit controller.
    // Empty if the plugin has no edit controller (rare, but valid VST3).
    virtual std::vector<ParameterDescriptor> listParameters() const = 0;

    // Sets one parameter (by the stringified ParamID from
    // ParameterDescriptor::key) to a plain-units value, applied on the
    // next process() call. Returns false if `key` isn't a known ParamID.
    virtual bool setParam(const std::string& key, double value) = 0;
};

// Loads and instantiates one VST3 plugin from a `.vst3` bundle directory
// (`<bundlePath>/Contents/<arch>-linux/*.so` on Linux). Throws
// std::runtime_error, with a message naming what failed, if the bundle is
// missing its module, the module has no exported factory, no class of
// category "Audio Module Class" is found, or the plugin refuses
// initialization -- there is no silent/degraded fallback here, unlike an
// unrecognized block `type`: a preset that names a `vst3` block asset
// expects that exact plugin to be running, not a passthrough standing in
// for it unannounced.
std::unique_ptr<IHostedPlugin> loadVst3Plugin(const std::string& bundlePath);

// Wraps one hosted plugin as an ordinary chain block, so
// ResourceManager::loadPreset can treat a "vst3" block exactly like any
// other EffectBlock -- see createEffectBlock() / IAssetLoader::loadVst3.
class Vst3EffectBlock : public EffectBlock {
public:
    using EffectBlock::process;  // bring the std::vector<float>& convenience overload back into scope

    explicit Vst3EffectBlock(std::unique_ptr<IHostedPlugin> plugin) : plugin_(std::move(plugin)) {}

    void prepare(double sampleRate) override { plugin_->prepare(sampleRate); }
    void process(float* buffer, std::size_t numSamples) override { plugin_->process(buffer, numSamples); }
    void reset() override { plugin_->reset(); }
    bool setLiveParam(const std::string& key, double value) override { return plugin_->setParam(key, value); }

    std::vector<IHostedPlugin::ParameterDescriptor> listParameters() const { return plugin_->listParameters(); }

private:
    std::unique_ptr<IHostedPlugin> plugin_;
};

}  // namespace audio_engine
