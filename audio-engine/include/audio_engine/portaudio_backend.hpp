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
// Device selection: AudioIoConfig::device -- a case-insensitive substring
// of the device's name -- or the system's current default input/output
// device (Pa_GetDefaultInputDevice/OutputDevice) when that's empty. On
// macOS the default is usually what you want: pick your external interface
// in Audio MIDI Setup before starting the engine with --audio. On a Pi it
// is not, because ALSA's default device there is the onboard bcm2835,
// which has no capture side at all -- name the USB interface explicitly.
// `audio_engine --list-devices` prints the available names.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

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

    // The actual negotiated per-side latency PortAudio/the driver settled
    // on (Pa_GetStreamInfo), in seconds -- can differ from the
    // suggestedLatency requested in start(). 0 if not currently running.
    // Reported by main.cpp on startup so "how much latency am I actually
    // getting" is a measured number, not a guess -- see README.md
    // "Latency".
    double inputLatencySeconds() const;
    double outputLatencySeconds() const;

    // Real-time health counters, updated from inside the audio callback
    // (atomics only -- no I/O there, see paCallback's comment) and safe to
    // read from any thread. main.cpp polls these periodically to report
    // real xrun/timing data from the actual live callback, not an offline
    // stand-in -- see README.md "Real-time callback health monitoring".
    // xrunCount: how many callbacks PortAudio itself flagged with an
    // input/output underflow or overflow (PaStreamCallbackFlags) -- a
    // ground-truth signal this backend never surfaced before.
    std::uint64_t xrunCount() const { return xrunCount_.load(std::memory_order_relaxed); }
    // overBudgetCount: how many callbacks took longer (wall-clock, around
    // the engine callback_ call only) than the block's real-time budget
    // (blockSize / sampleRate) -- our own code being the bottleneck,
    // distinct from a device-level xrun.
    std::uint64_t overBudgetCount() const { return overBudgetCount_.load(std::memory_order_relaxed); }
    std::uint64_t totalCallbackCount() const { return totalCallbackCount_.load(std::memory_order_relaxed); }
    // Microseconds; 0 if no callback has run yet.
    std::uint64_t maxCallbackMicros() const { return maxCallbackMicros_.load(std::memory_order_relaxed); }

    // The devices actually opened; empty until start() succeeds. Logged by
    // main.cpp on startup so a headless pedal's journal records which
    // interface it really grabbed, not merely which one was asked for --
    // the whole point of matching AudioIoConfig::device by substring is
    // that the match isn't perfectly predictable in advance.
    const std::string& inputDeviceName() const { return inputDeviceName_; }
    const std::string& outputDeviceName() const { return outputDeviceName_; }

    // One line per available device (index, name, host API, channel counts,
    // default sample rate), marking PortAudio's current defaults -- what
    // `audio_engine --list-devices` prints, and what start() appends to its
    // "no such device" errors. Self-contained: initializes and terminates
    // PortAudio itself, so it works before (and without) start(). Never
    // throws -- returns PortAudio's own error text as its result instead,
    // since a diagnostic that can fail loudly is worse than useless.
    static std::string describeDevices();

private:
    static int paCallback(const void* input, void* output, unsigned long frameCount,
                           const PaStreamCallbackTimeInfo* timeInfo,
                           PaStreamCallbackFlags statusFlags, void* userData);

    // Index of the first device whose name contains `needle`
    // (case-insensitive) and that has at least one channel on the requested
    // side, or paNoDevice if nothing matches. The channel-count filter
    // matters on a Pi, where the onboard output-only device would otherwise
    // happily match a substring meant for a duplex USB interface. Requires
    // PortAudio to already be initialized.
    static PaDeviceIndex findDeviceByName(const std::string& needle, bool wantInput);

    PaStream* stream_ = nullptr;
    bool initialized_ = false;
    std::atomic<bool> running_{false};
    AudioCallback callback_;
    double sampleRate_ = 48000.0;
    std::string inputDeviceName_;
    std::string outputDeviceName_;

    std::atomic<std::uint64_t> xrunCount_{0};
    std::atomic<std::uint64_t> overBudgetCount_{0};
    std::atomic<std::uint64_t> totalCallbackCount_{0};
    std::atomic<std::uint64_t> maxCallbackMicros_{0};
};

}  // namespace audio_engine
