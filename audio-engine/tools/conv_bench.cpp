// Convolution benchmark: per-block CPU time of the cabinet IR convolution,
// against the real-time budget (blockSize / sampleRate).
//
// Compares:
//   naive      -- the first ConvolutionEngine (direct time-domain loop, two
//                 heap allocations per block), copied here verbatim as the
//                 "before" reference.
//   fft        -- current ConvolutionEngine (uniform partitioned FFT,
//                 kConvolutionPartitionSize).
//   two-stage  -- FFTConvolver's TwoStageFFTConvolver (64 head / 1024 tail),
//                 for reference only: without a background thread its tail
//                 work lands in bursts, so its worst block is what matters.
//
// It also checks that fft/two-stage match naive (max abs difference).
//
// Usage:
//   conv_bench [--block N] [--rate HZ] [--seconds S] [--ir file.wav]...
// Without --ir it uses synthetic decaying-noise IRs of 512..65536 taps.
// With --ir it loads each file like the engine does (resample + energy
// normalisation) but WITHOUT the length cap, so long IRs can be measured.
// Run it on the Pi with the service stopped, pinned like the engine:
//   sudo systemctl stop multieffect-audio-engine
//   taskset -c 3 ./build/conv_bench
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "TwoStageFFTConvolver.h"
#include "audio_engine/convolution.hpp"
#include "audio_engine/resample.hpp"
#include "audio_engine/wav_file.hpp"

using namespace audio_engine;
using Clock = std::chrono::steady_clock;

namespace {

// The original ConvolutionEngine::process, unchanged (see git history).
class NaiveConvolver {
public:
    explicit NaiveConvolver(std::vector<float> ir) : ir_(std::move(ir)), history_(ir_.size() - 1, 0.0f) {}

    void process(float* buffer, std::size_t numSamples) {
        const std::size_t N = ir_.size();
        const std::size_t histLen = history_.size();
        std::vector<float> extended;
        extended.reserve(histLen + numSamples);
        extended.insert(extended.end(), history_.begin(), history_.end());
        extended.insert(extended.end(), buffer, buffer + numSamples);
        std::vector<float> output(numSamples);
        for (std::size_t i = 0; i < numSamples; ++i) {
            double acc = 0.0;
            const std::size_t base = histLen + i;
            for (std::size_t k = 0; k < N; ++k) {
                acc += static_cast<double>(ir_[k]) * static_cast<double>(extended[base - k]);
            }
            output[i] = static_cast<float>(acc);
        }
        std::copy(output.begin(), output.end(), buffer);
        if (histLen > 0) history_.assign(extended.end() - static_cast<std::ptrdiff_t>(histLen), extended.end());
    }

private:
    std::vector<float> ir_;
    std::vector<float> history_;
};

struct Stats {
    double meanUs = 0, p99Us = 0, maxUs = 0;
    std::vector<float> out;
};

template <typename F>
Stats run(F&& processBlock, const std::vector<float>& input, std::size_t block) {
    Stats st;
    st.out = input;
    std::vector<double> times;
    times.reserve(input.size() / block + 1);
    for (std::size_t pos = 0; pos + block <= st.out.size(); pos += block) {
        const auto t0 = Clock::now();
        processBlock(st.out.data() + pos, block);
        const auto t1 = Clock::now();
        times.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    double sum = 0;
    for (double t : times) sum += t;
    st.meanUs = sum / static_cast<double>(times.size());
    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    st.p99Us = sorted[static_cast<std::size_t>(0.99 * static_cast<double>(sorted.size() - 1))];
    st.maxUs = sorted.back();
    st.out.resize(times.size() * block);
    return st;
}

double maxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double d = 0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) d = std::max(d, std::fabs(double(a[i]) - b[i]));
    return d;
}

std::vector<float> syntheticIr(std::size_t len, std::mt19937& rng) {
    std::normal_distribution<float> n(0.0f, 1.0f);
    std::vector<float> ir(len);
    for (std::size_t i = 0; i < len; ++i) {
        ir[i] = n(rng) * std::exp(-6.9f * static_cast<float>(i) / static_cast<float>(len));  // -60 dB at the end
    }
    return normalizeIrEnergy(std::move(ir));
}

void printRow(const char* name, const Stats& s, double budgetUs, double diff) {
    std::printf("  %-10s mean %8.1f us (%6.1f%%)  p99 %8.1f us  max %8.1f us (%6.1f%%)", name, s.meanUs,
                100.0 * s.meanUs / budgetUs, s.p99Us, s.maxUs, 100.0 * s.maxUs / budgetUs);
    if (diff >= 0) std::printf("  max|diff| vs naive %.2e", diff);
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t block = 64;
    double rate = 48000.0;
    double seconds = 10.0;
    std::vector<std::string> irFiles;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--block" && i + 1 < argc) block = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--rate" && i + 1 < argc) rate = std::strtod(argv[++i], nullptr);
        else if (a == "--seconds" && i + 1 < argc) seconds = std::strtod(argv[++i], nullptr);
        else if (a == "--ir" && i + 1 < argc) irFiles.push_back(argv[++i]);
        else {
            std::fprintf(stderr, "usage: %s [--block N] [--rate HZ] [--seconds S] [--ir file.wav]...\n", argv[0]);
            return 2;
        }
    }
    const double budgetUs = 1e6 * static_cast<double>(block) / rate;
    std::printf("block %zu @ %.0f Hz -> budget %.1f us per block\n", block, rate, budgetUs);

    std::mt19937 rng(1234);
    std::vector<std::pair<std::string, std::vector<float>>> irs;
    if (irFiles.empty()) {
        for (std::size_t len : {512u, 2048u, 8192u, 16384u, 32768u, 65536u}) {
            irs.emplace_back("synthetic " + std::to_string(len), syntheticIr(len, rng));
        }
    } else {
        for (const auto& f : irFiles) {
            WavData wav = parseWavFile(f);
            irs.emplace_back(f, normalizeIrEnergy(resampleLinear(wav.samples, wav.sampleRate, rate)));
        }
    }

    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    std::vector<float> input(static_cast<std::size_t>(seconds * rate));
    for (float& s : input) s = u(rng);

    for (const auto& [name, ir] : irs) {
        std::printf("%s: %zu taps (%.0f ms)\n", name.c_str(), ir.size(), 1000.0 * ir.size() / rate);

        // Naive cost grows with taps; cap its run so long IRs stay quick.
        const double naiveSeconds = std::min(seconds, std::max(0.25, 2.0 * 8192.0 / static_cast<double>(ir.size())));
        std::vector<float> naiveIn(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(naiveSeconds * rate));
        NaiveConvolver naive(ir);
        Stats sn = run([&](float* b, std::size_t n) { naive.process(b, n); }, naiveIn, block);
        printRow("naive", sn, budgetUs, -1);

        ConvolutionEngine fft(ir);
        fft.prepare(rate);
        Stats sf = run([&](float* b, std::size_t n) { fft.process(b, n); }, input, block);
        printRow("fft", sf, budgetUs, maxAbsDiff(sf.out, sn.out));

        // Not in-place safe (its head pass overwrites the input the tail
        // still needs), and the Darkglass fork refuses IRs <= 2 tail blocks.
        fftconvolver::TwoStageFFTConvolver two;
        if (two.init(64, 1024, ir.data(), ir.size())) {
            std::vector<float> scratch(block);
            Stats st = run(
                [&](float* b, std::size_t n) {
                    two.process(b, scratch.data(), n);
                    std::copy(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(n), b);
                },
                input, block);
            printRow("two-stage", st, budgetUs, maxAbsDiff(st.out, sn.out));
        } else {
            std::printf("  two-stage  n/a (IR too short for 1024-sample tail blocks)\n");
        }
    }
    return 0;
}
