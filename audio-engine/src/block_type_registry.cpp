#include "audio_engine/block_type_registry.hpp"

namespace audio_engine {

using nlohmann::json;

void to_json(json& j, const BlockParamDescriptor& p) {
    j = json{
        {"key", p.key},           {"label", p.label},           {"unit", p.unit},
        {"min", p.minValue},      {"max", p.maxValue},          {"default", p.defaultValue},
        {"step_count", p.stepCount},
    };
}

void to_json(json& j, const BlockTypeDescriptor& t) { j = json{{"type", t.type}, {"parameters", t.parameters}}; }

std::vector<BlockTypeDescriptor> listNativeBlockTypes() {
    // Ranges are conventional musical defaults, not derived from any DSP
    // constraint (GainBlock/EqBlock/ToneStackBlock/DelayBlock don't clamp
    // their own inputs) -- a UI slider bound, not a validation rule.
    BlockTypeDescriptor gain;
    gain.type = "gain";
    gain.parameters = {{"gain_db", "Gain", "dB", -60.0, 24.0, 0.0, 0}};

    BlockTypeDescriptor volume;
    volume.type = "volume";
    volume.parameters = {{"gain_db", "Volume", "dB", -60.0, 24.0, 0.0, 0}};

    BlockTypeDescriptor eq;
    eq.type = "eq";
    eq.parameters = {{"gain_db", "Gain", "dB", -24.0, 24.0, 0.0, 0}};

    BlockTypeDescriptor toneStack;
    toneStack.type = "tone_stack";
    toneStack.parameters = {
        {"bass_db", "Bass", "dB", -15.0, 15.0, 0.0, 0},
        {"mid_db", "Mid", "dB", -15.0, 15.0, 0.0, 0},
        {"treble_db", "Treble", "dB", -15.0, 15.0, 0.0, 0},
    };

    BlockTypeDescriptor delay;
    delay.type = "delay";
    delay.parameters = {
        {"delay_ms", "Time", "ms", 1.0, 2000.0, 300.0, 0},
        {"feedback", "Feedback", "", 0.0, 0.95, 0.3, 0},
        {"mix", "Mix", "", 0.0, 1.0, 0.5, 0},
    };

    // The modelled pedals. Their knobs are pot positions, so they are
    // unitless 0..1 (matching the Python reference and the VST3 convention)
    // rather than the real units delay uses. step_count 1 marks a
    // two-position switch.
    //
    // Deliberately NOT listed: "bypass". Each block class understands it (the
    // Python reference has it, and the block tests use it), but a preset block
    // already carries an `enabled` flag that the engine honours by skipping
    // the block entirely -- see EffectBlockSpec::enabled and the `if
    // (!blockSpec.enabled) continue;` in resource_manager.cpp. Advertising
    // "bypass" as well would put a second, competing off-switch on every pedal
    // in the app's UI, next to the real per-block toggle. One off-switch, and
    // it is `enabled`.
    BlockTypeDescriptor bigMuff;
    bigMuff.type = "big_muff";
    bigMuff.parameters = {
        {"sustain", "Sustain", "", 0.0, 1.0, 0.7, 0},
        {"tone", "Tone", "", 0.0, 1.0, 0.5, 0},
        {"volume", "Volume", "", 0.0, 1.0, 0.5, 0},
        {"pad_15db", "-15 dB Pad", "", 0.0, 1.0, 0.0, 1},
    };

    BlockTypeDescriptor tubeScreamer;
    tubeScreamer.type = "tube_screamer";
    tubeScreamer.parameters = {
        {"drive", "Drive", "", 0.0, 1.0, 0.5, 0},
        {"tone", "Tone", "", 0.0, 1.0, 0.5, 0},
        {"level", "Level", "", 0.0, 1.0, 0.5, 0},
    };

    // Real units here, unlike the two above: a gate's threshold and timings
    // are absolute quantities a player reasons about directly, not knob
    // positions.
    BlockTypeDescriptor noiseGate;
    noiseGate.type = "noise_gate";
    noiseGate.parameters = {
        {"threshold_db", "Threshold", "dB", -90.0, -10.0, -45.0, 0},
        {"range_db", "Range", "dB", -90.0, 0.0, -60.0, 0},
        {"attack_ms", "Attack", "ms", 0.1, 50.0, 1.0, 0},
        {"hold_ms", "Hold", "ms", 0.0, 500.0, 40.0, 0},
        {"release_ms", "Release", "ms", 1.0, 1000.0, 120.0, 0},
        {"hysteresis_db", "Hysteresis", "dB", 0.0, 24.0, 6.0, 0},
    };

    return {gain, volume, eq, toneStack, delay, bigMuff, tubeScreamer, noiseGate};
}

}  // namespace audio_engine
