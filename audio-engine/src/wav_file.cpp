#include "audio_engine/wav_file.hpp"

#include <cstring>
#include <fstream>

namespace audio_engine {

namespace {

std::uint32_t readU32LE(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t readU16LE(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8);
}

// 24-bit PCM is tightly packed (3 bytes/sample, no 4th padding byte) --
// read the 3 bytes and sign-extend to a full 32-bit two's-complement int.
std::int32_t readS24LE(const std::uint8_t* p) {
    std::int32_t raw = static_cast<std::int32_t>(p[0]) | (static_cast<std::int32_t>(p[1]) << 8) |
                        (static_cast<std::int32_t>(p[2]) << 16);
    if (raw & 0x00800000) {
        raw |= static_cast<std::int32_t>(0xFF000000);
    }
    return raw;
}

void writeU32LE(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

void writeU16LE(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void writeTag(std::vector<std::uint8_t>& out, const char tag[4]) {
    out.insert(out.end(), tag, tag + 4);
}

constexpr std::uint16_t kFormatPcm = 1;
constexpr std::uint16_t kFormatIeeeFloat = 3;

}  // namespace

WavData parseWavBytes(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 12) throw WavParseError("file too small to be a WAV file (< 12 bytes)");
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0) throw WavParseError("missing 'RIFF' chunk id");
    if (std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) throw WavParseError("missing 'WAVE' format id");

    std::size_t pos = 12;
    bool haveFmt = false;
    bool haveData = false;

    std::uint16_t formatCode = 0;
    std::uint16_t numChannels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPerSample = 0;

    std::vector<std::uint8_t> dataBytes;

    while (pos + 8 <= bytes.size()) {
        char chunkId[5] = {0};
        std::memcpy(chunkId, bytes.data() + pos, 4);
        std::uint32_t chunkSize = readU32LE(bytes.data() + pos + 4);
        std::size_t chunkDataStart = pos + 8;

        if (chunkDataStart + chunkSize > bytes.size()) {
            // Truncated / lying chunk size -- clamp to what we actually have
            // rather than reading out of bounds, but this is still a
            // malformed file for "data" (we need every declared byte).
            if (std::memcmp(chunkId, "data", 4) == 0) {
                throw WavParseError("'data' chunk size exceeds file length (truncated file)");
            }
            chunkSize = static_cast<std::uint32_t>(bytes.size() - chunkDataStart);
        }

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) throw WavParseError("'fmt ' chunk shorter than 16 bytes");
            const std::uint8_t* p = bytes.data() + chunkDataStart;
            formatCode = readU16LE(p + 0);
            numChannels = readU16LE(p + 2);
            sampleRate = readU32LE(p + 4);
            bitsPerSample = readU16LE(p + 14);
            haveFmt = true;
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            dataBytes.assign(bytes.begin() + chunkDataStart, bytes.begin() + chunkDataStart + chunkSize);
            haveData = true;
        }

        pos = chunkDataStart + chunkSize;
        if (chunkSize % 2 == 1) pos += 1;  // chunks are word-aligned/padded
    }

    if (!haveFmt) throw WavParseError("missing 'fmt ' chunk");
    if (!haveData) throw WavParseError("missing 'data' chunk");
    if (numChannels == 0) throw WavParseError("'fmt ' declares 0 channels");
    if (formatCode != kFormatPcm && formatCode != kFormatIeeeFloat) {
        throw WavParseError("unsupported WAV format code " + std::to_string(formatCode) +
                             " (only PCM=1 and IEEE float=3 are supported)");
    }
    bool isFloat = (formatCode == kFormatIeeeFloat);
    if (isFloat && bitsPerSample != 32) {
        throw WavParseError("IEEE float WAV must be 32-bit, got " + std::to_string(bitsPerSample));
    }
    if (!isFloat && bitsPerSample != 16 && bitsPerSample != 24 && bitsPerSample != 32) {
        throw WavParseError("unsupported PCM bits-per-sample " + std::to_string(bitsPerSample) +
                             " (only 16, 24, and 32 are supported)");
    }

    const std::size_t bytesPerSample = bitsPerSample / 8;
    const std::size_t frameSize = bytesPerSample * numChannels;
    if (frameSize == 0 || dataBytes.size() % frameSize != 0) {
        throw WavParseError("'data' chunk size is not a whole number of frames");
    }
    const std::size_t numFrames = dataBytes.size() / frameSize;

    WavData result;
    result.sampleRate = static_cast<double>(sampleRate);
    result.numChannels = numChannels;
    result.bitsPerSample = bitsPerSample;
    result.wasFloat = isFloat;
    result.samples.resize(numFrames);

    for (std::size_t frame = 0; frame < numFrames; ++frame) {
        double sum = 0.0;
        const std::uint8_t* framePtr = dataBytes.data() + frame * frameSize;
        for (int ch = 0; ch < numChannels; ++ch) {
            const std::uint8_t* samplePtr = framePtr + static_cast<std::size_t>(ch) * bytesPerSample;
            double normalized;
            if (isFloat) {
                float f;
                std::memcpy(&f, samplePtr, sizeof(float));
                normalized = static_cast<double>(f);
            } else if (bitsPerSample == 16) {
                std::int16_t raw = static_cast<std::int16_t>(readU16LE(samplePtr));
                normalized = static_cast<double>(raw) / 32768.0;
            } else if (bitsPerSample == 24) {
                std::int32_t raw = readS24LE(samplePtr);
                normalized = static_cast<double>(raw) / 8388608.0;  // 2^23
            } else {  // 32-bit PCM
                std::int32_t raw = static_cast<std::int32_t>(readU32LE(samplePtr));
                normalized = static_cast<double>(raw) / 2147483648.0;
            }
            sum += normalized;
        }
        result.samples[frame] = static_cast<float>(sum / numChannels);
    }

    return result;
}

WavData parseWavFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw WavParseError("could not open file: " + path);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseWavBytes(bytes);
}

std::vector<std::uint8_t> writeWavBytes(const std::vector<float>& monoSamples, double sampleRate,
                                         WavSampleFormat format) {
    const std::uint16_t numChannels = 1;
    const std::uint16_t bitsPerSample = (format == WavSampleFormat::Pcm16) ? 16 : 32;
    const std::uint16_t formatCode = (format == WavSampleFormat::Pcm16) ? kFormatPcm : kFormatIeeeFloat;
    const std::uint32_t byteRate =
        static_cast<std::uint32_t>(sampleRate) * numChannels * (bitsPerSample / 8);
    const std::uint16_t blockAlign = numChannels * (bitsPerSample / 8);
    const std::uint32_t dataSize = static_cast<std::uint32_t>(monoSamples.size()) * (bitsPerSample / 8);

    std::vector<std::uint8_t> out;
    out.reserve(44 + dataSize);

    writeTag(out, "RIFF");
    writeU32LE(out, 36 + dataSize);
    writeTag(out, "WAVE");

    writeTag(out, "fmt ");
    writeU32LE(out, 16);
    writeU16LE(out, formatCode);
    writeU16LE(out, numChannels);
    writeU32LE(out, static_cast<std::uint32_t>(sampleRate));
    writeU32LE(out, byteRate);
    writeU16LE(out, blockAlign);
    writeU16LE(out, bitsPerSample);

    writeTag(out, "data");
    writeU32LE(out, dataSize);

    for (float s : monoSamples) {
        if (format == WavSampleFormat::Pcm16) {
            float clamped = s < -1.0f ? -1.0f : (s > 1.0f ? 1.0f : s);
            std::int16_t raw = static_cast<std::int16_t>(clamped * 32767.0f);
            writeU16LE(out, static_cast<std::uint16_t>(raw));
        } else {
            std::uint8_t bytes4[4];
            std::memcpy(bytes4, &s, sizeof(float));
            out.insert(out.end(), bytes4, bytes4 + 4);
        }
    }

    return out;
}

void writeWavFile(const std::string& path, const std::vector<float>& monoSamples, double sampleRate,
                   WavSampleFormat format) {
    auto bytes = writeWavBytes(monoSamples, sampleRate, format);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw WavParseError("could not open file for writing: " + path);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace audio_engine
