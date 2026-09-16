// Shared DSP primitives for the modelled pedal blocks (big_muff_block.hpp,
// tube_screamer_block.hpp, noise_gate_block.hpp).
//
// These are a port of vst-python/src/pedals/dsp.py, which is the reference
// implementation: the Python models are where each pedal's behaviour was
// designed and measured, and tests/test_python_parity.cpp replays golden
// renders from them through these blocks. Keep the two in step -- a change
// here without the corresponding change there (and a regenerated fixture)
// will fail that test, which is the point of it.
//
// Two deliberate differences from the Python:
//
//  * **Everything is double internally.** EffectBlock's buffers are float,
//    so each block converts on the way in and out, but no intermediate DSP
//    state is ever stored at float precision. That keeps parity with the
//    float64 reference to ~1e-7 instead of accumulating float error through
//    four cascaded filters and an oversampler.
//
//  * **Parameters are smoothed** (ParamSmoother), which the Python models
//    deliberately do not do -- there, per-block parameter application keeps
//    renders block-size invariant; here, a knob moving mid-stream must not
//    step a gain discontinuously and click. See ParamSmoother's note on how
//    this stays compatible with bit-comparable renders.
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace audio_engine {

// Largest chunk the oversampler processes at once. process() loops over
// longer buffers in pieces of this size, so its scratch buffers can be
// allocated once in prepare() and never resized in the real-time path
// regardless of the host's block size.
inline constexpr std::size_t kPedalMaxChunk = 1024;

// Smooth, compressive saturation modelling diodes in a feedback loop.
// `bias` offsets the curve before clipping to generate even-order harmonics;
// the subtraction keeps silence in giving exactly silence out.
double softClip(double x, double bias) noexcept;

// One-pole (6 dB/octave) filter. The highpass is derived as x - lowpass(x),
// same as the Python reference -- not an independent recursion.
class OnePole {
public:
    enum class Mode { Lowpass, Highpass };

    void prepare(double sampleRate, double cutoffHz, Mode mode);
    void reset() { state_ = 0.0; }
    void process(const double* in, double* out, std::size_t numSamples) noexcept;
    double processSample(double x) noexcept;

private:
    double a_ = 1.0;
    double state_ = 0.0;
    Mode mode_ = Mode::Lowpass;
};

// Integer-ratio up/downsampling to wrap around a nonlinear section.
//
// A hard-driven clipper generates harmonics far past Nyquist; distorting at
// the base rate folds them back as inharmonic aliasing. Oversampling moves
// the fold-back point up and lets the decimation filter discard the rest.
// Only the nonlinear part of a chain belongs between up and down -- linear
// filtering is unaffected by aliasing and is cheaper outside.
class Oversampler {
public:
    // `factor` of 1 makes both directions a pass-through.
    void prepare(int factor, std::size_t numTaps = 65, double transition = 0.9);
    void reset();

    int factor() const { return factor_; }
    std::size_t numTaps() const { return taps_.size(); }
    const std::vector<double>& taps() const { return taps_; }

    // Round-trip group delay, in base-rate samples.
    double latencySamples() const;

    // `out` must hold numSamples * factor doubles.
    void upsample(const double* in, double* out, std::size_t numSamples) noexcept;
    // `in` holds numSamples * factor doubles; `out` holds numSamples.
    void downsample(const double* in, double* out, std::size_t numSamples) noexcept;

private:
    void filter(const double* in, double* out, std::size_t n, std::vector<double>& history,
                std::vector<double>& scratch) noexcept;

    int factor_ = 1;
    std::vector<double> taps_;
    std::vector<double> upHistory_;
    std::vector<double> downHistory_;
    std::vector<double> upScratch_;
    std::vector<double> downScratch_;
    std::vector<double> stuffed_;
};

// A one-pole ramp toward a target value, applied per sample.
//
// **Why this does not break parity with the Python reference.** The models in
// vst-python apply parameters per block with no ramp, so a render there is
// exactly what the coefficients say. Here, snap() sets current and target to
// the same value and next() then returns it unchanged forever -- no epsilon,
// no drift -- so a block that is configured and then processed produces the
// identical result. Blocks snap in their constructor, prepare() and reset();
// only setLiveParam() ramps. That means a preset load is bit-comparable with
// the reference while a slider drag is still click-free, which is exactly the
// split we want.
class ParamSmoother {
public:
    // `rampMs` is the time constant of the approach, not a hard duration.
    void prepare(double sampleRate, double rampMs = 20.0);

    // Jump immediately, cancelling any ramp in progress.
    void snap(double value) noexcept {
        current_ = value;
        target_ = value;
    }
    void setTarget(double value) noexcept { target_ = value; }

    double target() const noexcept { return target_; }
    double current() const noexcept { return current_; }
    bool isSmoothing() const noexcept { return current_ != target_; }

    double next() noexcept {
        // Exactly equal is the common case (nothing is moving) and must stay
        // exact, so it short-circuits rather than running the recursion.
        if (current_ == target_) return current_;
        current_ += (target_ - current_) * coef_;
        // Terminate the ramp rather than approaching forever: without this the
        // difference decays into denormals and isSmoothing() never clears.
        if (std::abs(target_ - current_) < 1e-12) current_ = target_;
        return current_;
    }

private:
    double current_ = 0.0;
    double target_ = 0.0;
    double coef_ = 1.0;
};

}  // namespace audio_engine
