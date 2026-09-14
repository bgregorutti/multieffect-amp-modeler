#include "audio_engine/portaudio_backend.hpp"

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
                                  const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags,
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

    self->callback_(out, static_cast<std::size_t>(frameCount));
    return paContinue;
}

void PortAudioBackend::start(const AudioIoConfig& config, AudioCallback callback) {
    if (running_.load()) {
        throw std::runtime_error("PortAudioBackend::start called while already running");
    }
    callback_ = std::move(callback);

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
