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

    return {gain, volume, eq, toneStack, delay};
}

}  // namespace audio_engine
