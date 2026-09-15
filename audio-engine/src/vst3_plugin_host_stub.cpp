// Built instead of vst3_plugin_host.cpp when AUDIO_ENGINE_WITH_VST3 is OFF
// (the default) -- see CMakeLists.txt. Keeps the "vst3" block type
// resolvable (createEffectBlock/IAssetLoader::loadVst3 always compile) so a
// preset referencing a vst3 asset fails with one clear, actionable error
// rather than a link error or a silently-wrong passthrough.
#include <stdexcept>

#include "audio_engine/vst3_host.hpp"

namespace audio_engine {

std::unique_ptr<IHostedPlugin> loadVst3Plugin(const std::string& bundlePath) {
    throw std::runtime_error("cannot load VST3 plugin '" + bundlePath +
                              "': audio_engine was built without -DAUDIO_ENGINE_WITH_VST3=ON");
}

}  // namespace audio_engine
