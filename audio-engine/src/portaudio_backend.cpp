#include "audio_engine/portaudio_backend.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

namespace audio_engine {

namespace {
std::string paError(const char* what, PaError err) {
    return std::string(what) + ": " + Pa_GetErrorText(err);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Assumes PortAudio is already initialized -- Pa_GetDeviceCount and friends
// return an error/nothing useful otherwise. PortAudioBackend::describeDevices()
// wraps this in its own Pa_Initialize/Pa_Terminate pair for standalone use
// (--list-devices), while start()'s failure paths call it directly, already
// holding an initialization of their own.
std::string listDevices() {
    std::ostringstream out;
    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        out << "  " << paError("Pa_GetDeviceCount failed", static_cast<PaError>(count)) << "\n";
        return out.str();
    }
    if (count == 0) {
        out << "  (no audio devices at all -- on a Pi, check `aplay -l` / `arecord -l` and that\n"
               "   the service's user is in the 'audio' group)\n";
        return out.str();
    }
    const PaDeviceIndex defaultIn = Pa_GetDefaultInputDevice();
    const PaDeviceIndex defaultOut = Pa_GetDefaultOutputDevice();
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info == nullptr) continue;
        const PaHostApiInfo* host = Pa_GetHostApiInfo(info->hostApi);
        out << "  [" << i << "] \"" << (info->name != nullptr ? info->name : "(unnamed)") << "\""
            << " via " << ((host != nullptr && host->name != nullptr) ? host->name : "?")
            << " -- in=" << info->maxInputChannels << " out=" << info->maxOutputChannels
            << " default_rate=" << info->defaultSampleRate << " Hz";
        if (i == defaultIn) out << "  [system default input]";
        if (i == defaultOut) out << "  [system default output]";
        out << "\n";
    }
    return out.str();
}
}  // namespace

PortAudioBackend::PortAudioBackend() = default;

PortAudioBackend::~PortAudioBackend() { stop(); }

int PortAudioBackend::paCallback(const void* input, void* output, unsigned long frameCount,
                                  const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags statusFlags,
                                  void* userData) {
    auto* self = static_cast<PortAudioBackend*>(userData);
    auto* out = static_cast<float*>(output);
    const auto* in = static_cast<const float*>(input);

    // Our processing signature is a single in-place buffer (mono, matching
    // EffectBlock::process -- see effect_block.hpp), but PortAudio hands us
    // separate input/output buffers, so seed the output with the input
    // first. `input` can be null on the very first callback(s) on some
    // hosts before the input stream has warmed up -- fall back to silence
    // rather than reading through a null pointer.
    if (in != nullptr) {
        std::memcpy(out, in, sizeof(float) * frameCount);
    } else {
        std::memset(out, 0, sizeof(float) * frameCount);
    }

    // Ground truth from PortAudio/the driver itself: was this callback
    // preceded by an actual input or output underflow/overflow? Previously
    // discarded entirely -- see README.md "Real-time callback health
    // monitoring" for why this matters (distinguishing a real device-level
    // xrun from our own code being slow).
    if (statusFlags & (paInputUnderflow | paInputOverflow | paOutputUnderflow | paOutputOverflow)) {
        self->xrunCount_.fetch_add(1, std::memory_order_relaxed);
    }

    const auto t0 = std::chrono::steady_clock::now();
    self->callback_(out, static_cast<std::size_t>(frameCount));
    const auto t1 = std::chrono::steady_clock::now();

    // Atomics + a monotonic clock read only -- no locks, no I/O, real-time
    // safe. main.cpp polls these from a normal (non-real-time) thread.
    const auto micros = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    self->totalCallbackCount_.fetch_add(1, std::memory_order_relaxed);
    std::uint64_t prevMax = self->maxCallbackMicros_.load(std::memory_order_relaxed);
    while (micros > prevMax &&
           !self->maxCallbackMicros_.compare_exchange_weak(prevMax, micros, std::memory_order_relaxed)) {
    }
    const double budgetMicros = static_cast<double>(frameCount) / self->sampleRate_ * 1'000'000.0;
    if (static_cast<double>(micros) > budgetMicros) {
        self->overBudgetCount_.fetch_add(1, std::memory_order_relaxed);
    }

    return paContinue;
}

PaDeviceIndex PortAudioBackend::findDeviceByName(const std::string& needle, bool wantInput) {
    const std::string wanted = toLower(needle);
    const PaDeviceIndex count = Pa_GetDeviceCount();
    for (PaDeviceIndex i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info == nullptr || info->name == nullptr) continue;
        const int channels = wantInput ? info->maxInputChannels : info->maxOutputChannels;
        if (channels < 1) continue;
        if (toLower(info->name).find(wanted) != std::string::npos) return i;
    }
    return paNoDevice;
}

std::string PortAudioBackend::describeDevices() {
    const PaError err = Pa_Initialize();
    if (err != paNoError) {
        return paError("Pa_Initialize failed", err) + "\n";
    }
    const std::string devices = listDevices();
    Pa_Terminate();
    return devices;
}

void PortAudioBackend::start(const AudioIoConfig& config, AudioCallback callback) {
    if (running_.load()) {
        throw std::runtime_error("PortAudioBackend::start called while already running");
    }
    callback_ = std::move(callback);
    sampleRate_ = config.sampleRate;
    xrunCount_.store(0, std::memory_order_relaxed);
    overBudgetCount_.store(0, std::memory_order_relaxed);
    totalCallbackCount_.store(0, std::memory_order_relaxed);
    maxCallbackMicros_.store(0, std::memory_order_relaxed);

    PaError err = Pa_Initialize();
    if (err != paNoError) {
        throw std::runtime_error(paError("Pa_Initialize failed", err));
    }
    initialized_ = true;

    PaStreamParameters inputParams{};
    inputParams.device = config.device.empty() ? Pa_GetDefaultInputDevice()
                                               : findDeviceByName(config.device, /*wantInput=*/true);
    if (inputParams.device == paNoDevice) {
        const std::string available = listDevices();
        Pa_Terminate();
        initialized_ = false;
        throw std::runtime_error(
            (config.device.empty()
                 ? std::string("no default input audio device found -- select one in your OS's "
                               "audio settings, or name one explicitly with --device")
                 : "no audio input device whose name contains \"" + config.device + "\"") +
            ". Available devices:\n" + available);
    }
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paFloat32;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaStreamParameters outputParams{};
    outputParams.device = config.device.empty()
                              ? Pa_GetDefaultOutputDevice()
                              : findDeviceByName(config.device, /*wantInput=*/false);
    if (outputParams.device == paNoDevice) {
        const std::string available = listDevices();
        Pa_Terminate();
        initialized_ = false;
        throw std::runtime_error(
            (config.device.empty()
                 ? std::string("no default output audio device found -- select one in your OS's "
                               "audio settings, or name one explicitly with --device")
                 : "no audio output device whose name contains \"" + config.device + "\"") +
            ". Available devices:\n" + available);
    }
    outputParams.channelCount = 1;
    outputParams.sampleFormat = paFloat32;
    outputParams.suggestedLatency = Pa_GetDeviceInfo(outputParams.device)->defaultLowOutputLatency;
    outputParams.hostApiSpecificStreamInfo = nullptr;

    // Captured before Pa_OpenStream so the names are available to whoever
    // reports a failure, and so they reflect the resolved index rather than
    // the substring that was searched for.
    {
        const PaDeviceInfo* in = Pa_GetDeviceInfo(inputParams.device);
        const PaDeviceInfo* out = Pa_GetDeviceInfo(outputParams.device);
        inputDeviceName_ = (in != nullptr && in->name != nullptr) ? in->name : "(unknown)";
        outputDeviceName_ = (out != nullptr && out->name != nullptr) ? out->name : "(unknown)";
    }

    err = Pa_OpenStream(&stream_, &inputParams, &outputParams, config.sampleRate,
                         static_cast<unsigned long>(config.blockSize), paNoFlag,
                         &PortAudioBackend::paCallback, this);
    if (err != paNoError) {
        Pa_Terminate();
        initialized_ = false;
        throw std::runtime_error(paError("Pa_OpenStream failed", err));
    }

    err = Pa_StartStream(stream_);
    if (err != paNoError) {
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        Pa_Terminate();
        initialized_ = false;
        throw std::runtime_error(paError("Pa_StartStream failed", err));
    }

    running_.store(true);
}

double PortAudioBackend::inputLatencySeconds() const {
    if (!running_.load() || stream_ == nullptr) return 0.0;
    const PaStreamInfo* info = Pa_GetStreamInfo(stream_);
    return info != nullptr ? info->inputLatency : 0.0;
}

double PortAudioBackend::outputLatencySeconds() const {
    if (!running_.load() || stream_ == nullptr) return 0.0;
    const PaStreamInfo* info = Pa_GetStreamInfo(stream_);
    return info != nullptr ? info->outputLatency : 0.0;
}

void PortAudioBackend::stop() {
    if (!running_.load()) {
        return;
    }
    Pa_StopStream(stream_);
    Pa_CloseStream(stream_);
    stream_ = nullptr;
    if (initialized_) {
        Pa_Terminate();
        initialized_ = false;
    }
    running_.store(false);
}

}  // namespace audio_engine
