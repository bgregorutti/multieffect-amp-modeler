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
#include <string>

namespace audio_engine {

// Invoked once per audio block on the backend's real-time thread. `buffer`
// holds `numSamples` mono float samples in [-1, 1] captured from the input
// device on entry, and must be filled with the samples to send to the
// output device before returning. Real-time context: must not allocate,
// lock, or perform I/O.
using AudioCallback = std::function<void(float* buffer, std::size_t numSamples)>;

struct AudioIoConfig {
    double sampleRate = 48000.0;
    // 64 samples (~1.33ms @ 48kHz) -- lowered from an earlier 256-sample
    // default after real hardware testing surfaced round-trip latency as
    // a real, audible issue (see README.md "Latency"). Benchmarked safe:
    // the real-time-capped convolution engine (see
    // convolution.hpp/kMaxRealtimeIrSamples) uses ~38% of the per-block
    // budget at 64 samples, essentially the same margin as at 256 (block
    // work and budget both scale ~linearly with block size), so this
    // isn't trading safety margin for latency. Override with
    // `audio_engine ... --audio --block-size N` if your machine can't
    // keep up (more OS scheduling jitter matters more at smaller blocks
    // on a general-purpose, non-RTOS scheduler) or wants to go lower.
    std::size_t blockSize = 64;

    // Case-insensitive substring of the audio device's name to open for
    // both capture and playback (e.g. "Scarlett", "USB Audio"). Empty --
    // the default -- means "whatever the OS currently calls the default
    // device", which is the right answer on a dev machine where you pick
    // your interface in the OS's own sound settings.
    //
    // It is NOT the right answer on a Pi: ALSA's default device there is
    // the onboard bcm2835, which has no capture side at all, so a USB
    // interface has to be named explicitly or start() fails with "no
    // default input audio device found". Matched as a name substring
    // rather than taken as a card index on purpose -- USB card numbering
    // isn't stable across boots, and a pedal has to come up correctly
    // every single time it's switched on. See `audio_engine
    // --list-devices` for the available names.
    std::string device;
};

class IAudioIoBackend {
public:
    virtual ~IAudioIoBackend() = default;

    // Opens the input/output device(s) selected by `config` -- the system
    // defaults unless `config.device` names one -- and starts `callback`
    // running on a real-time (or near-real-time) thread the backend owns.
    // Throws std::runtime_error on failure to open or start the device,
    // including when nothing matches `config.device`. Calling start()
    // while already running throws too.
    virtual void start(const AudioIoConfig& config, AudioCallback callback) = 0;

    // Stops the callback and closes the device(s). Safe to call when not
    // running. Implementations should also call this from their
    // destructor.
    virtual void stop() = 0;

    virtual bool isRunning() const = 0;
};

}  // namespace audio_engine
