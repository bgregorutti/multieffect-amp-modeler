// Big Muff Pi style fuzz, modelled as DSP rather than as a NAM capture.
//
// A .nam file bakes in the knob positions it was captured at, and two of the
// three knobs here are things an inference budget should never be spent on:
// Volume is a multiply and Tone is a linear filter network. Only Sustain
// reshapes the nonlinearity. Modelling the circuit directly costs a small
// fraction of one NAM inference and leaves the knobs continuously variable,
// which is what the mobile app's per-block parameter editing assumes.
//
// Ported from vst-python/src/pedals/bigmuff.py -- see pedal_dsp.hpp for what
// that means and for the parity test that enforces it.
//
//   in -> pad -> input HPF -> Sustain (pre-gain)
//      -> [4x oversampled] clip stage 1 -> clip stage 2
//      -> DC block -> tone stack -> Volume -> out
//
// Each clipping stage is a coupling highpass, a fixed gain, an asymmetric
// soft saturator (the diodes in the transistor's feedback loop) and a lowpass
// modelling the cap across those diodes -- the part naive distortion models
// leave out, and most of why they sound harsh.
#pragma once

#include <string>
#include <vector>

#include "audio_engine/effect_block.hpp"
#include "audio_engine/pedal_dsp.hpp"
#include "audio_engine/preset_model.hpp"

namespace audio_engine {

class BigMuffBlock : public EffectBlock {
public:
    using EffectBlock::process;

    // Knob positions, normalized 0..1, matching the VST3 convention and the
    // Python reference. `pad15dB` is the switchable -15 dB input pad.
    explicit BigMuffBlock(double sustain = 0.7, double tone = 0.5, double volume = 0.5,
                          bool pad15dB = false, bool bypass = false);
    explicit BigMuffBlock(const ParamMap& params);

    void setSustain(double value);
    void setTone(double value);
    void setVolume(double value);
    void setPad15dB(bool enabled);
    void setBypass(bool enabled) { bypass_ = enabled; }

    double sustain() const { return sustain_; }
    double tone() const { return tone_; }
    double volume() const { return volume_; }
    bool pad15dB() const { return pad15dB_; }
    bool bypass() const { return bypass_; }

    // Added latency in base-rate samples, from the oversampling filters.
    double latencySamples() const { return oversampler_.latencySamples(); }

    void prepare(double sampleRate) override;
    void process(float* buffer, std::size_t numSamples) override;
    void reset() override;
    // Recognizes "sustain", "tone", "volume", "pad_15db", "bypass".
    bool setLiveParam(const std::string& key, double value) override;

private:
    void snapSmoothers();
    void processChunk(float* buffer, std::size_t numSamples) noexcept;

    double sustain_;
    double tone_;
    double volume_;
    bool pad15dB_;
    bool bypass_;
    double sampleRate_ = 48000.0;
    int oversample_ = 4;

    Oversampler oversampler_;
    OnePole inputHp_;
    OnePole stageHp_[2];
    OnePole stageLp_[2];
    OnePole dcBlocker_;
    OnePole toneLp_;
    OnePole toneHp_;

    ParamSmoother padGain_;
    ParamSmoother sustainGain_;
    ParamSmoother toneBlend_;
    ParamSmoother volumeGain_;

    std::vector<double> base_;
    std::vector<double> oversampled_;
};

}  // namespace audio_engine
