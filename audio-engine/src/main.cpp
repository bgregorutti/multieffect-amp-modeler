// Audio engine process entry point.
//
// Usage: audio_engine <control-socket-path> [--audio]
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
// EngineState::processAudioBlock -- see README.md "Real-time audio I/O".
// Only available when built with -DAUDIO_ENGINE_WITH_PORTAUDIO=ON (off by
// default; see CMakeLists.txt and README.md "Deviations" for why JUCE
// itself isn't used).
#include <atomic>
#include <chrono>
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
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--audio") {
            wantAudio = true;
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
