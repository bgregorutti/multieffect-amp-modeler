#include "audio_engine/portaudio_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>

namespace audio_engine {

namespace {
std::string paError(const char* what, PaError err) {
    return std::string(what) + ": " + Pa_GetErrorText(err);
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
    inputParams.device = Pa_GetDefaultInputDevice();
    if (inputParams.device == paNoDevice) {
        Pa_Terminate();
        initialized_ = false;
        throw std::runtime_error(
            "no default input audio device found -- select one in your OS's audio settings");
    }
    inputParams.channelCount = 1;
    inputParams.sampleFormat = paFloat32;
    inputParams.suggestedLatency = Pa_GetDeviceInfo(inputParams.device)->defaultLowInputLatency;
    inputParams.hostApiSpecificStreamInfo = nullptr;

    PaStreamParameters outputParams{};
    outputParams.device = Pa_GetDefaultOutputDevice();
    if (outputParams.device == paNoDevice) {
        Pa_Terminate();
        initialized_ = false;
        throw std::runtime_error(
            "no default output audio device found -- select one in your OS's audio settings");
    }
    outputParams.channelCount = 1;
    outputParams.sampleFormat = paFloat32;
    outputParams.suggestedLatency = Pa_GetDeviceInfo(outputParams.device)->defaultLowOutputLatency;
    outputParams.hostApiSpecificStreamInfo = nullptr;

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
