// Audio engine process entry point.
//
// Usage: audio_engine <control-socket-path> [--audio] [--block-size N]
//                     [--device NAME]
//        audio_engine --list-devices
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
//
// --device NAME picks the audio interface by a case-insensitive substring
// of its name instead of using the OS default. Required on a Pi, whose
// ALSA default device is the onboard bcm2835 with no capture side at all
// -- see AudioIoConfig::device. --list-devices prints the names to choose
// from and exits; it needs no control socket, so it can be run on its own.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <string>
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
    // Handled before the usage check below: --list-devices is a pure
    // diagnostic that opens no socket and starts no engine, so requiring a
    // socket path alongside it would be needless ceremony -- on a headless
    // pedal this is the first thing you run after plugging an interface in.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--list-devices") {
#ifdef AUDIO_ENGINE_WITH_PORTAUDIO
            std::cout << audio_engine::PortAudioBackend::describeDevices();
            return 0;
#else
            std::cerr << "audio-engine: --list-devices requires a build with "
                         "-DAUDIO_ENGINE_WITH_PORTAUDIO=ON\n";
            return 2;
#endif
        }
    }

    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <control-socket-path> [--audio] [--block-size N] [--device NAME]\n"
                  << "       " << argv[0] << " --list-devices\n";
        return 2;
    }
    const std::string socketPath = argv[1];
    bool wantAudio = false;
    // Only consumed when built with AUDIO_ENGINE_WITH_PORTAUDIO; parsed
    // unconditionally below so a plain build still accepts (and ignores)
    // the flag rather than rejecting it as unrecognized.
    [[maybe_unused]] std::size_t blockSizeOverride = 0;  // 0 == use AudioIoConfig's default
    [[maybe_unused]] std::string deviceName;              // empty == use the OS default device
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--audio") {
            wantAudio = true;
        } else if (arg == "--block-size" && i + 1 < argc) {
            blockSizeOverride = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--device" && i + 1 < argc) {
            deviceName = argv[++i];
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
        config.device = deviceName;
        try {
            audioBackend.start(config, [&engineState](float* buffer, std::size_t numSamples) {
                engineState.processAudioBlock(buffer, numSamples);
            });
        } catch (const std::exception& e) {
            std::cerr << "failed to start audio I/O: " << e.what() << "\n";
            server.stop();
            return 1;
        }
        std::cerr << "audio-engine: streaming input \"" << audioBackend.inputDeviceName()
                  << "\" -> output \"" << audioBackend.outputDeviceName() << "\" (sample rate "
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

#ifdef AUDIO_ENGINE_WITH_PORTAUDIO
    std::uint64_t lastXruns = 0, lastOverBudget = 0, lastTotal = 0;
#endif
    while (!g_stopRequested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
#ifdef AUDIO_ENGINE_WITH_PORTAUDIO
        if (wantAudio) {
            const std::uint64_t xruns = audioBackend.xrunCount();
            const std::uint64_t overBudget = audioBackend.overBudgetCount();
            const std::uint64_t total = audioBackend.totalCallbackCount();
            if (xruns != lastXruns || overBudget != lastOverBudget) {
                std::cerr << "audio-engine: [health] +" << (xruns - lastXruns) << " device xrun(s), +"
                          << (overBudget - lastOverBudget) << " over-budget block(s) in the last ~2s ("
                          << (total - lastTotal) << " blocks processed, max block time so far: "
                          << (audioBackend.maxCallbackMicros() / 1000.0) << " ms)\n";
            }
            lastXruns = xruns;
            lastOverBudget = overBudget;
            lastTotal = total;
        }
#endif
    }

#ifdef AUDIO_ENGINE_WITH_PORTAUDIO
    if (wantAudio) {
        std::cerr << "audio-engine: [health] final totals: " << audioBackend.xrunCount()
                  << " device xrun(s), " << audioBackend.overBudgetCount() << " over-budget block(s) of "
                  << audioBackend.totalCallbackCount() << " processed, max block time "
                  << (audioBackend.maxCallbackMicros() / 1000.0) << " ms\n";
    }
    audioBackend.stop();
#endif
    server.stop();
    return 0;
}
