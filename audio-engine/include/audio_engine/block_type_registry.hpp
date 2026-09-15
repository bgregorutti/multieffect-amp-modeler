// Static parameter *schema* (metadata, not values) for every native block
// type -- what a UI needs to render a real slider ("Gain", dB, -60..24)
// instead of a generic key/value text editor, without hardcoding that
// knowledge in the daemon or the mobile app (see engine_state.cpp's
// "list_block_types" command). Kept separate from each block's own
// .hpp/.cpp since this is purely descriptive and never touched by a real
// instance -- authored once here, per type, and kept in sync by hand with
// each block's actual EffectBlock::setLiveParam-recognized keys (a
// mismatch there is a real bug: a param this lists as adjustable that
// setLiveParam then rejects, or vice versa).
//
// VST3 blocks have no entry here: their schema is per-*asset* (different
// .vst3 files have different parameters), not per-type -- see
// register_asset's VST3 introspection case instead.
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace audio_engine {

struct BlockParamDescriptor {
    std::string key;    // matches the block's own setLiveParam key and params map key
    std::string label;   // human-readable, e.g. "Gain"
    std::string unit;    // e.g. "dB", "ms", "" for a unitless ratio
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    int stepCount = 0;   // 0 = continuous
};

struct BlockTypeDescriptor {
    std::string type;
    std::vector<BlockParamDescriptor> parameters;
};

void to_json(nlohmann::json& j, const BlockParamDescriptor& p);
void to_json(nlohmann::json& j, const BlockTypeDescriptor& t);

// Every native (non-"vst3") block type this engine recognizes, in a stable
// order. Types with no live-adjustable parameters at all ("passthrough")
// are omitted rather than listed with an empty array.
std::vector<BlockTypeDescriptor> listNativeBlockTypes();

}  // namespace audio_engine
