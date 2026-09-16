// Noise gate (downward expander), for taming hiss from upstream pedals.
//
// **What this can and cannot do.** A gate removes noise *in the gaps*: it
// shuts the path when you are not playing, so a distortion block's hiss never
// reaches the amp. It cannot remove noise riding underneath a note you are
// actually playing -- while open it passes the signal through untouched, hiss
// included. Doing that needs spectral subtraction against a noise profile,
// which costs an FFT of latency and smears transients. Every guitar noise gate
// ever built works the way this one does, because in practice the hiss is only
// objectionable in the silences.
//
// Three details separate a gate that works from one that chatters:
//
//  * **Hysteresis** -- opens at the threshold but does not close until the
//    signal falls a few dB below it, so a signal sitting near the threshold
//    cannot flap the gate at audio rate.
//  * **Hold** -- once open it stays open for a minimum time, letting notes
//    decay naturally instead of being chopped off.
//  * **Range** -- the closed state attenuates by a set amount rather than
//    going silent. A gate slamming to digital zero is *more* noticeable,
//    because the noise floor vanishing entirely is itself an audible event.
//
// Unlike the distortion blocks this needs no ParamSmoother: its output gain is
// already ramped by the attack/release coefficients, and the remaining
// parameters (thresholds, times) feed a state machine rather than multiplying
// the signal, so changing them mid-stream cannot produce a step discontinuity.
//
// Ported from vst-python/src/pedals/noisegate.py; see pedal_dsp.hpp.
#pragma once

#include <string>

#include "audio_engine/effect_block.hpp"
#include "audio_engine/pedal_dsp.hpp"
#include "audio_engine/preset_model.hpp"

namespace audio_engine {

class NoiseGateBlock : public EffectBlock {
public:
    using EffectBlock::process;

    // Parameters are in real units (dB, ms), matching the block registry's
    // convention for delay rather than the normalized 0..1 the distortion
    // pedals' knob positions use.
    struct Settings {
        double thresholdDb = -45.0;
        double rangeDb = -60.0;
        double attackMs = 1.0;
        double holdMs = 40.0;
        double releaseMs = 120.0;
        double hysteresisDb = 6.0;
        bool bypass = false;
    };

    NoiseGateBlock();
    explicit NoiseGateBlock(const Settings& settings);
    explicit NoiseGateBlock(const ParamMap& params);

    const Settings& settings() const { return settings_; }
    void setSettings(const Settings& settings);

    // Whether the gate is currently passing signal -- drives a UI indicator.
    bool isOpen() const { return isOpen_; }
    double latencySamples() const { return 0.0; }  // causal, no lookahead

    void prepare(double sampleRate) override;
    void process(float* buffer, std::size_t numSamples) override;
    void reset() override;
    // Recognizes "threshold_db", "range_db", "attack_ms", "hold_ms",
    // "release_ms", "hysteresis_db", "bypass".
    bool setLiveParam(const std::string& key, double value) override;

private:
    void recomputeCoefficients();

    Settings settings_;
    double sampleRate_ = 48000.0;

    double envelopeDecay_ = 0.0;
    double attackCoef_ = 0.0;
    double releaseCoef_ = 0.0;
    double openThreshold_ = 0.0;
    double closeThreshold_ = 0.0;
    double floorGain_ = 0.0;
    long holdSamples_ = 0;

    double envelope_ = 0.0;
    double gain_ = 0.0;
    bool isOpen_ = false;
    long holdCounter_ = 0;
};

}  // namespace audio_engine
