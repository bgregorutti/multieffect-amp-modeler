// TS808 Tube Screamer style overdrive.
//
// Structurally this is *not* a milder Big Muff, and modelling it as one is the
// usual way to get it wrong. Two things define the circuit:
//
//  * **The dry signal never leaves.** The clipping diodes sit in the feedback
//    loop of a *non-inverting* op-amp stage, so the output is the input plus
//    whatever the boosted-and-clipped path adds. That is why a Tube Screamer
//    sounds like a boost with hair on it rather than a fuzz, and why it cleans
//    up when the guitar's volume is rolled back.
//
//  * **Bass is never amplified into the clipper.** The feedback network's
//    input leg is a resistor in series with a capacitor (4.7k / 0.047uF), so
//    stage gain falls to unity below ~720 Hz. That single highpass is the
//    entire reason for the midrange focus -- a *gain* shape before the
//    clipping, not a tone control after it.
//
// Ported from vst-python/src/pedals/tubescreamer.py; see pedal_dsp.hpp.
//
//   in -> input HPF
//      -> [4x oversampled] dry + clip(highpass(dry) * Drive)
//      -> DC block -> fixed LPF -> Tone -> Level -> out
#pragma once

#include <string>
#include <vector>

#include "audio_engine/effect_block.hpp"
#include "audio_engine/pedal_dsp.hpp"
#include "audio_engine/preset_model.hpp"

namespace audio_engine {

class TubeScreamerBlock : public EffectBlock {
public:
    using EffectBlock::process;

    // Knob positions, normalized 0..1.
    explicit TubeScreamerBlock(double drive = 0.5, double tone = 0.5, double level = 0.5,
                               bool bypass = false);
    explicit TubeScreamerBlock(const ParamMap& params);

    void setDrive(double value);
    void setTone(double value);
    void setLevel(double value);
    void setBypass(bool enabled) { bypass_ = enabled; }

    double drive() const { return drive_; }
    double tone() const { return tone_; }
    double level() const { return level_; }
    bool bypass() const { return bypass_; }

    double latencySamples() const { return oversampler_.latencySamples(); }

    void prepare(double sampleRate) override;
    void process(float* buffer, std::size_t numSamples) override;
    void reset() override;
    // Recognizes "drive", "tone", "level", "bypass".
    bool setLiveParam(const std::string& key, double value) override;

private:
    void snapSmoothers();
    void processChunk(float* buffer, std::size_t numSamples) noexcept;

    double drive_;
    double tone_;
    double level_;
    bool bypass_;
    double sampleRate_ = 48000.0;
    int oversample_ = 4;

    Oversampler oversampler_;
    OnePole inputHp_;
    OnePole driveHp_;
    OnePole dcBlocker_;
    OnePole fixedLp_;
    OnePole toneDarkLp_;

    ParamSmoother driveGain_;
    ParamSmoother toneBlend_;
    ParamSmoother levelGain_;

    std::vector<double> base_;
    std::vector<double> oversampled_;
    std::vector<double> boost_;
};

}  // namespace audio_engine
