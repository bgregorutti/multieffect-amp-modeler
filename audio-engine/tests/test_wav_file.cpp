#include "audio_engine/wav_file.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

using namespace audio_engine;

namespace {

// Hand-assembles a minimal, valid mono WAV file byte-for-byte (not via
// writeWavBytes -- this is an independent construction so the parser test
// isn't just checking "the writer and reader agree with each other").
std::vector<std::uint8_t> buildMinimalPcm16Wav(const std::vector<std::int16_t>& samples, std::uint32_t sampleRate) {
    std::vector<std::uint8_t> out;
    auto putTag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    auto putU32 = [&](std::uint32_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 24) & 0xFF);
    };
    auto putU16 = [&](std::uint16_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
    };

    const std::uint16_t numChannels = 1;
    const std::uint16_t bitsPerSample = 16;
    const std::uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    const std::uint16_t blockAlign = numChannels * (bitsPerSample / 8);
    const std::uint32_t dataSize = static_cast<std::uint32_t>(samples.size()) * 2;

    putTag("RIFF");
    putU32(36 + dataSize);
    putTag("WAVE");
    putTag("fmt ");
    putU32(16);
    putU16(1);  // PCM
    putU16(numChannels);
    putU32(sampleRate);
    putU32(byteRate);
    putU16(blockAlign);
    putU16(bitsPerSample);
    putTag("data");
    putU32(dataSize);
    for (std::int16_t s : samples) putU16(static_cast<std::uint16_t>(s));

    return out;
}

}  // namespace

TEST(WavFile, ParsesHandAssembledPcm16Wav) {
    std::vector<std::int16_t> raw = {0, 16384, -16384, 32767, -32768};
    auto bytes = buildMinimalPcm16Wav(raw, 44100);

    WavData wav = parseWavBytes(bytes);
    EXPECT_DOUBLE_EQ(wav.sampleRate, 44100.0);
    EXPECT_EQ(wav.numChannels, 1);
    EXPECT_EQ(wav.bitsPerSample, 16);
    EXPECT_FALSE(wav.wasFloat);
    ASSERT_EQ(wav.samples.size(), raw.size());

    EXPECT_NEAR(wav.samples[0], 0.0f, 1e-6f);
    EXPECT_NEAR(wav.samples[1], 16384.0f / 32768.0f, 1e-6f);
    EXPECT_NEAR(wav.samples[2], -16384.0f / 32768.0f, 1e-6f);
    EXPECT_NEAR(wav.samples[3], 32767.0f / 32768.0f, 1e-6f);
    EXPECT_NEAR(wav.samples[4], -1.0f, 1e-6f);
}

TEST(WavFile, WriteThenParseRoundTripsPcm16) {
    std::vector<float> original = {0.0f, 0.5f, -0.5f, 0.999f, -1.0f};
    auto bytes = writeWavBytes(original, 48000.0, WavSampleFormat::Pcm16);
    WavData wav = parseWavBytes(bytes);

    EXPECT_DOUBLE_EQ(wav.sampleRate, 48000.0);
    ASSERT_EQ(wav.samples.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_NEAR(wav.samples[i], original[i], 1e-3f) << "index " << i;
    }
}

TEST(WavFile, WriteThenParseRoundTripsFloat32Exactly) {
    std::vector<float> original = {0.0f, 0.123456f, -0.987654f, 1.0f, -1.0f};
    auto bytes = writeWavBytes(original, 96000.0, WavSampleFormat::Float32);
    WavData wav = parseWavBytes(bytes);

    EXPECT_TRUE(wav.wasFloat);
    EXPECT_EQ(wav.bitsPerSample, 32);
    ASSERT_EQ(wav.samples.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) {
        EXPECT_FLOAT_EQ(wav.samples[i], original[i]) << "index " << i;
    }
}

TEST(WavFile, FileRoundTrip) {
    std::vector<float> original = {0.1f, -0.2f, 0.3f, -0.4f};
    std::string path = "/tmp/audio_engine_test_fixture.wav";
    writeWavFile(path, original, 48000.0, WavSampleFormat::Pcm16);
    WavData wav = parseWavFile(path);
    ASSERT_EQ(wav.samples.size(), original.size());
    for (size_t i = 0; i < original.size(); ++i) EXPECT_NEAR(wav.samples[i], original[i], 1e-3f);
}

TEST(WavFile, StereoIsDownmixedToMonoByAveraging) {
    // Hand-build a 2-channel, 16-bit PCM file: L=32767 (~1.0), R=-32768 (~-1.0)
    // for one frame -- averaged mono sample should be ~0.
    std::vector<std::uint8_t> out;
    auto putTag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    auto putU32 = [&](std::uint32_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 24) & 0xFF);
    };
    auto putU16 = [&](std::uint16_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
    };
    putTag("RIFF");
    putU32(36 + 4);
    putTag("WAVE");
    putTag("fmt ");
    putU32(16);
    putU16(1);
    putU16(2);  // stereo
    putU32(44100);
    putU32(44100 * 2 * 2);
    putU16(4);
    putU16(16);
    putTag("data");
    putU32(4);
    putU16(static_cast<std::uint16_t>(static_cast<std::int16_t>(32767)));
    putU16(static_cast<std::uint16_t>(static_cast<std::int16_t>(-32768)));

    WavData wav = parseWavBytes(out);
    EXPECT_EQ(wav.numChannels, 2);
    ASSERT_EQ(wav.samples.size(), 1u);
    EXPECT_NEAR(wav.samples[0], 0.0f, 1e-3f);
}

TEST(WavFile, RejectsTooSmallFile) {
    std::vector<std::uint8_t> bytes = {'R', 'I', 'F'};
    EXPECT_THROW(parseWavBytes(bytes), WavParseError);
}

TEST(WavFile, RejectsMissingRiffTag) {
    auto bytes = buildMinimalPcm16Wav({0, 1, 2}, 44100);
    bytes[0] = 'X';
    EXPECT_THROW(parseWavBytes(bytes), WavParseError);
}

TEST(WavFile, RejectsMissingDataChunk) {
    std::vector<std::uint8_t> out;
    auto putTag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    auto putU32 = [&](std::uint32_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 24) & 0xFF);
    };
    auto putU16 = [&](std::uint16_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
    };
    putTag("RIFF");
    putU32(36);
    putTag("WAVE");
    putTag("fmt ");
    putU32(16);
    putU16(1);
    putU16(1);
    putU32(44100);
    putU32(88200);
    putU16(2);
    putU16(16);
    // no "data" chunk at all
    EXPECT_THROW(parseWavBytes(out), WavParseError);
}

TEST(WavFile, RejectsUnsupportedBitDepth) {
    std::vector<std::uint8_t> out;
    auto putTag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    auto putU32 = [&](std::uint32_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 24) & 0xFF);
    };
    auto putU16 = [&](std::uint16_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
    };
    putTag("RIFF");
    putU32(36 + 1);
    putTag("WAVE");
    putTag("fmt ");
    putU32(16);
    putU16(1);
    putU16(1);
    putU32(44100);
    putU32(44100);
    putU16(1);
    putU16(8);  // unsupported 8-bit
    putTag("data");
    putU32(1);
    out.push_back(128);
    EXPECT_THROW(parseWavBytes(out), WavParseError);
}

TEST(WavFile, RejectsNonexistentFile) {
    EXPECT_THROW(parseWavFile("/nonexistent/path/does_not_exist.wav"), WavParseError);
}
