// Hand-rolled RIFF/WAVE reader + writer.
//
// Deliberately not using an external audio-file library: WAV is a small,
// well-specified chunked binary format and cabinet IR files the engine
// needs to load are always short, simple PCM/float mono files, so a
// from-scratch parser is both trivial and dependency-free (relevant given
// the "no GitHub-fetched C++ dependency" sandbox constraint -- see
// audio-engine/README.md).
//
// Supports: PCM 16-bit, PCM 32-bit (integer), IEEE float 32-bit, mono or
// interpreted as mono (multi-channel files are downmixed by averaging
// channels, since cabinet IRs for this engine are always mono). Samples
// are always returned/written normalized to the [-1, 1] float range.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace audio_engine {

struct WavParseError : std::runtime_error {
    explicit WavParseError(const std::string& msg) : std::runtime_error(msg) {}
};

struct WavData {
    double sampleRate = 0.0;
    int numChannels = 0;       // channels found in the source file
    int bitsPerSample = 0;     // bits found in the source file
    bool wasFloat = false;     // true if source format was IEEE float
    std::vector<float> samples;  // mono, normalized to [-1, 1]
};

// Parses raw WAV file bytes. Throws WavParseError on any structural
// problem: missing "RIFF"/"WAVE"/"fmt "/"data" chunks, truncated data,
// unsupported bits-per-sample/format code, etc.
WavData parseWavBytes(const std::vector<std::uint8_t>& bytes);
WavData parseWavFile(const std::string& path);

// Writer, primarily for generating test fixtures and for anything that
// needs to round-trip a WAV (e.g. exporting a captured impulse response).
// Always writes mono, either 16-bit PCM or 32-bit IEEE float.
enum class WavSampleFormat { Pcm16, Float32 };

std::vector<std::uint8_t> writeWavBytes(const std::vector<float>& monoSamples, double sampleRate,
                                         WavSampleFormat format = WavSampleFormat::Pcm16);
void writeWavFile(const std::string& path, const std::vector<float>& monoSamples, double sampleRate,
                   WavSampleFormat format = WavSampleFormat::Pcm16);

}  // namespace audio_engine
