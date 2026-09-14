// Audio engine process entry point.
//
// Usage: audio_engine <control-socket-path> [--audio] [--block-size N]
//
// Starts the control socket server (see control_socket.hpp) bound to the
// given Unix domain socket path and blocks forever, dispatching incoming
// commands to an EngineState. Without --audio, that's the entire process
// (proves out preset loading / control-plane behavior end to end, with no
// audio device opened at all -- this is what the gtest suite's subprocess
// integration test uses).
//
// --audio additionally opens the system's default audio input/output
// device via a real-time IAudioIoBackend and streams it through
// EngineState::processAudioBlock -- see README.md "Real-time audio I/O"
// and "Latency". Only available when built with
// -DAUDIO_ENGINE_WITH_PORTAUDIO=ON (off by default; see CMakeLists.txt
// and README.md "Deviations" for why JUCE itself isn't used).
//
// --block-size N overrides AudioIoConfig's default (64 samples, ~1.33ms
// @ 48kHz) -- go higher if you hear crackling/dropouts, lower if your
// machine has headroom and you want even less latency.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <thread>

#include "audio_engine/control_socket.hpp"
#include "audio_engine/engine_state.hpp"
#include "audio_engine/resource_manager.hpp"

#ifdef AUDIO_ENGINE_WITH_PORTAUDIO
#include "audio_engine/portaudio_backend.hpp"
#endif

namespace {
std::atomic<bool> g_stopRequested{false};
void handleSignal(int) { g_stopRequested.store(true); }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <control-socket-path> [--audio]\n";
        return 2;
    }
    const std::string socketPath = argv[1];
    bool wantAudio = false;
    // Only consumed when built with AUDIO_ENGINE_WITH_PORTAUDIO; parsed
    // unconditionally below so a plain build still accepts (and ignores)
    // the flag rather than rejecting it as unrecognized.
    [[maybe_unused]] std::size_t blockSizeOverride = 0;  // 0 == use AudioIoConfig's default
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--audio") {
            wantAudio = true;
        } else if (arg == "--block-size" && i + 1 < argc) {
            blockSizeOverride = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    auto loader = std::make_shared<audio_engine::FileAssetLoader>();
    audio_engine::EngineState engineState(loader);
    audio_engine::ControlSocketServer server;

    try {
        server.start(socketPath, [&engineState](const nlohmann::json& cmd) {
            return engineState.handleCommand(cmd);
        });
    } catch (const std::exception& e) {
        std::cerr << "failed to start control socket: " << e.what() << "\n";
        return 1;
    }

    std::cerr << "audio-engine: listening on " << socketPath << "\n";

#ifdef AUDIO_ENGINE_WITH_PORTAUDIO
    audio_engine::PortAudioBackend audioBackend;
    if (wantAudio) {
        audio_engine::AudioIoConfig config;
        config.sampleRate = engineState.sampleRate();
        if (blockSizeOverride > 0) {
            config.blockSize = blockSizeOverride;
        }
        try {
            audioBackend.start(config, [&engineState](float* buffer, std::size_t numSamples) {
                engineState.processAudioBlock(buffer, numSamples);
            });
        } catch (const std::exception& e) {
            std::cerr << "failed to start audio I/O: " << e.what() << "\n";
            server.stop();
            return 1;
        }
        std::cerr << "audio-engine: streaming default audio device (sample rate "
                  << config.sampleRate << " Hz, block size " << config.blockSize << ")\n";
        std::cerr << "audio-engine: negotiated latency: input "
                  << (audioBackend.inputLatencySeconds() * 1000.0) << " ms, output "
                  << (audioBackend.outputLatencySeconds() * 1000.0) << " ms (round-trip is "
                     "roughly the sum, plus USB/driver overhead not visible to PortAudio)\n";
    }
#else
    if (wantAudio) {
        std::cerr << "audio-engine: --audio requires a build with "
                     "-DAUDIO_ENGINE_WITH_PORTAUDIO=ON\n";
        server.stop();
        return 2;
    }
#endif

    while (!g_stopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

#ifdef AUDIO_ENGINE_WITH_PORTAUDIO
    audioBackend.stop();
#endif
    server.stop();
    return 0;
}
