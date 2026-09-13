// Audio engine process entry point.
//
// Usage: audio_engine <control-socket-path>
//
// Starts the control socket server (see control_socket.hpp) bound to the
// given Unix domain socket path and blocks forever, dispatching incoming
// commands to an EngineState. This is deliberately the entire "process":
// there is no real-time audio I/O thread here yet (see README.md "Out of
// scope" -- no ALSA/JACK/PortAudio backend exists in this sandbox), so
// today this process only proves out preset loading / control-plane
// behavior end to end; the real-time audio callback thread is the
// documented next step once real audio hardware is available to test
// against.
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

#include "audio_engine/control_socket.hpp"
#include "audio_engine/engine_state.hpp"
#include "audio_engine/resource_manager.hpp"

namespace {
std::atomic<bool> g_stopRequested{false};
void handleSignal(int) { g_stopRequested.store(true); }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <control-socket-path>\n";
        return 2;
    }
    const std::string socketPath = argv[1];

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

    while (!g_stopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.stop();
    return 0;
}
