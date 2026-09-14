// Seam between the DSP chain (EngineChain/EngineState -- sample-rate and
// hardware agnostic) and whatever actually talks to a real audio device.
// Same "narrow interface + swappable backend" pattern used everywhere else
// hardware shows up in this project: control-daemon's
// FootswitchInputBackend/AudioEngineClient, this package's own
// INamModel/IAssetLoader. A dev machine can implement this with PortAudio
// (see portaudio_backend.hpp, compiled only when AUDIO_ENGINE_WITH_PORTAUDIO
// is on -- see README.md "Real-time audio I/O"); a Raspberry Pi build can
// supply a different implementation later without EngineChain or
// EngineState changing at all.
#pragma once

#include <cstddef>
#include <functional>

namespace audio_engine {

// Invoked once per audio block on the backend's real-time thread. `buffer`
// holds `numSamples` mono float samples in [-1, 1] captured from the input
// device on entry, and must be filled with the samples to send to the
// output device before returning. Real-time context: must not allocate,
// lock, or perform I/O.
using AudioCallback = std::function<void(float* buffer, std::size_t numSamples)>;

struct AudioIoConfig {
    double sampleRate = 48000.0;
    std::size_t blockSize = 256;
};

class IAudioIoBackend {
public:
    virtual ~IAudioIoBackend() = default;

    // Opens the system's default input/output device(s) per `config` and
    // starts `callback` running on a real-time (or near-real-time) thread
    // the backend owns. Throws std::runtime_error on failure to open or
    // start the device. Calling start() while already running throws too.
    virtual void start(const AudioIoConfig& config, AudioCallback callback) = 0;

    // Stops the callback and closes the device(s). Safe to call when not
    // running. Implementations should also call this from their
    // destructor.
    virtual void stop() = 0;

    virtual bool isRunning() const = 0;
};

}  // namespace audio_engine
