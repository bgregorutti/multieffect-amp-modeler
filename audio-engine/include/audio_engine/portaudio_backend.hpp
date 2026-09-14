// IAudioIoBackend implementation backed by PortAudio -- the fastest path
// to real device I/O on a dev machine, since (unlike JUCE/
// NeuralAmpModelerCore, see README.md "Deviations") it's a single small C
// library installable from a normal package manager (e.g. `brew install
// portaudio` on macOS), not something that requires cloning a GitHub repo.
//
// Only compiled when the AUDIO_ENGINE_WITH_PORTAUDIO CMake option is on
// (default OFF) -- a production/Pi build need not link PortAudio at all if
// it ends up using a different backend; see README.md "Real-time audio
// I/O". This header itself is therefore only ever included behind that
// same #ifdef, so it's safe to include <portaudio.h> directly rather than
// forward-declaring its types.
//
// Device selection: always the system's current default input/output
// device (Pa_GetDefaultInputDevice/OutputDevice), not a name/index passed
// in -- on macOS, pick your external interface as the system default in
// Audio MIDI Setup before starting the engine with --audio. Deliberately
// minimal for now: picking a specific device by name is a reasonable
// follow-up if/when it's actually needed, not guessed at here.
#pragma once

#include <atomic>

#include <portaudio.h>

#include "audio_engine/audio_io_backend.hpp"

namespace audio_engine {

class PortAudioBackend : public IAudioIoBackend {
public:
    PortAudioBackend();
    ~PortAudioBackend() override;

    PortAudioBackend(const PortAudioBackend&) = delete;
    PortAudioBackend& operator=(const PortAudioBackend&) = delete;

    void start(const AudioIoConfig& config, AudioCallback callback) override;
    void stop() override;
    bool isRunning() const override { return running_.load(); }

private:
    static int paCallback(const void* input, void* output, unsigned long frameCount,
                           const PaStreamCallbackTimeInfo* timeInfo,
                           PaStreamCallbackFlags statusFlags, void* userData);

    PaStream* stream_ = nullptr;
    bool initialized_ = false;
    std::atomic<bool> running_{false};
    AudioCallback callback_;
};

}  // namespace audio_engine
