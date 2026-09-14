// Dev diagnostic tool: runs a WAV file through a real .nam model using the
// exact same code path production audio (EngineChain -> RealNamModel) uses
// -- same resample-to-48kHz, same block size, same nam::get_dsp() loading
// -- and writes out both the resampled-but-unprocessed ("dry") and the
// NAM-processed ("wet") signal as separate WAV files, so the two can be
// directly compared (listened to, diffed, plotted) rather than inferred
// from aggregate stats alone. Built only when AUDIO_ENGINE_WITH_REAL_NAM
// is on (see CMakeLists.txt) -- needs the vendored NeuralAmpModelerCore.
//
// Usage: nam_render <input.wav> <model.nam> <dry_out.wav> <wet_out.wav> [block_size]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <NAM/get_dsp.h>

#include "audio_engine/nam_model.hpp"
#include "audio_engine/real_nam_model.hpp"
#include "audio_engine/resample.hpp"
#include "audio_engine/wav_file.hpp"

using namespace audio_engine;

namespace {
void printStats(const char* label, const std::vector<float>& buf) {
    double peak = 0.0, rms = 0.0;
    int nonFinite = 0;
    for (float s : buf) {
        if (!std::isfinite(s)) {
            nonFinite++;
            continue;
        }
        peak = std::max(peak, static_cast<double>(std::fabs(s)));
        rms += static_cast<double>(s) * s;
    }
    rms = std::sqrt(rms / buf.size());
    printf("%-6s samples=%zu peak=%.4f rms=%.4f%s\n", label, buf.size(), peak, rms,
           nonFinite > 0 ? "  *** NON-FINITE SAMPLES PRESENT ***" : "");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <input.wav> <model.nam> <dry_out.wav> <wet_out.wav> [block_size]\n", argv[0]);
        return 2;
    }
    const std::string inputPath = argv[1];
    const std::string namPath = argv[2];
    const std::string dryOutPath = argv[3];
    const std::string wetOutPath = argv[4];
    const std::size_t blockSize = argc > 5 ? static_cast<std::size_t>(std::stoul(argv[5])) : 64;
    constexpr double kEngineSampleRate = 48000.0;

    try {
        WavData input = parseWavFile(inputPath);
        printf("input: %s\n", inputPath.c_str());
        printf("  native: %d ch, %d bit, %s, %.0f Hz, %zu samples\n", input.numChannels, input.bitsPerSample,
               input.wasFloat ? "float" : "PCM", input.sampleRate, input.samples.size());

        std::vector<float> dry = resampleLinear(input.samples, input.sampleRate, kEngineSampleRate);
        if (input.sampleRate != kEngineSampleRate) {
            printf("  resampled %.0f Hz -> %.0f Hz (%zu -> %zu samples)\n", input.sampleRate, kEngineSampleRate,
                   input.samples.size(), dry.size());
        }

        NamModelMetadata meta = parseNamModelFile(namPath);
        auto dsp = nam::get_dsp(std::filesystem::path(namPath));
        printf("\nmodel: %s\n", namPath.c_str());
        printf("  architecture=%s expected_sample_rate=%.1f", meta.architecture.c_str(),
               dsp->GetExpectedSampleRate());
        if (dsp->HasLoudness()) printf(" loudness=%.2fdB", dsp->GetLoudness());
        printf("\n");

        RealNamModel model(meta, std::move(dsp));
        model.prepare(kEngineSampleRate);

        std::vector<float> wet = dry;
        std::size_t i = 0;
        for (; i + blockSize <= wet.size(); i += blockSize) {
            model.process(wet.data() + i, blockSize);
        }
        if (i < wet.size()) {
            model.process(wet.data() + i, wet.size() - i);  // final partial block
        }

        printf("\n=== stats (block_size=%zu) ===\n", blockSize);
        printStats("dry", dry);
        printStats("wet", wet);

        writeWavFile(dryOutPath, dry, kEngineSampleRate, WavSampleFormat::Float32);
        writeWavFile(wetOutPath, wet, kEngineSampleRate, WavSampleFormat::Float32);
        printf("\nwrote %s and %s (48kHz float32 mono, listen/diff directly)\n", dryOutPath.c_str(),
               wetOutPath.c_str());
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
