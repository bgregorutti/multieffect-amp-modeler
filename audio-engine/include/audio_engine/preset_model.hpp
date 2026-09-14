// Data model mirroring control-daemon's Pydantic schema
// (control-daemon/src/control_daemon/models.py) *exactly*, so a preset
// exported by the control daemon (or sent to the engine's control socket,
// see control_socket.hpp) can be deserialized here with no translation
// layer. Field names, optionality and defaults intentionally match:
//
//   ResolvedBlock:  id, type, asset_id (nullable), enabled, params
//                   (params opaque: string -> (double|string|bool))
//   AssetKind:      "nam" | "ir"
//   Asset:          id, kind, filename, stored_path, size_bytes, sha256, uploaded_at
//   ResolvedPreset: id, name, rig_id, rig_name, blocks
//
// The daemon models a *Rig* (an amp + cab + effects, where the amp/cab
// blocks are "pinned" = always on) containing several *Presets* (which
// only say which of that rig's non-pinned blocks are enabled). The engine
// is deliberately kept ignorant of all of that: the daemon flattens
// rig+preset into a `ResolvedPreset` and sends that, so what arrives here
// is simply "the exact ordered chain to play". Hence `Preset` below maps
// to the daemon's ResolvedPreset and `EffectBlockSpec` to its
// ResolvedBlock -- there are no rigs, no overrides and no timestamps on
// the wire.
//
// Two consequences worth spelling out:
//   * Asset references live on each *block* (`asset_id`), not on the
//     preset. There is no longer a preset-level nam_asset_id/ir_asset_id;
//     the amp is just a block of type "nam" and the cab a block of type
//     "ir", each carrying its own asset_id.
//   * `blocks` order is meaningful and explicit -- it is the signal chain
//     order, amp and cab included -- so it must be preserved exactly.
//
// Only the fields the audio engine actually needs to consume are modeled
// here (the daemon owns Rig/Bank/footswitch-mapping/DaemonState -- the
// engine only ever receives one ResolvedPreset at a time over the control
// socket). `Asset` is unchanged by the rig refactor and still mirrors the
// daemon's Asset model verbatim.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace audio_engine {

// EffectBlock.params values are opaque to the engine's preset model itself
// (each concrete EffectBlock implementation interprets its own keys), but
// they must round-trip the same dynamic-typed values the daemon accepts:
// float, int, str, bool. We collapse int/float to double (JSON numbers are
// not distinguished on the wire either way) which matches how
// nlohmann::json itself would deserialize a bare "1" vs "1.0" if we don't
// special-case it -- see ParamValue below for the actual representation.
using ParamValue = std::variant<double, std::string, bool>;
using ParamMap = std::map<std::string, ParamValue>;

struct EffectBlockSpec {
    // Stable per-rig block id ("amp", "cab", "dist", ...). Required on the
    // wire: the daemon addresses blocks by it, and the engine echoes it
    // back in diagnostics.
    std::string id;
    std::string type;
    // The binary asset this block plays through, if it needs one (a "nam"
    // block's model, an "ir" block's cabinet). Null/absent for blocks that
    // are pure DSP (gain, eq, delay, ...).
    std::optional<std::string> asset_id;
    bool enabled = true;
    ParamMap params;

    bool operator==(const EffectBlockSpec&) const = default;
};

enum class AssetKind { Nam, Ir };

std::string toString(AssetKind kind);
AssetKind assetKindFromString(const std::string& s);

struct Asset {
    std::string id;
    AssetKind kind = AssetKind::Nam;
    std::string filename;
    std::string stored_path;
    long long size_bytes = 0;
    std::optional<std::string> sha256;
    double uploaded_at = 0.0;
};

// Mirrors the daemon's ResolvedPreset: the flattened chain to play.
// `rig_id`/`rig_name` are carried for diagnostics/logging only -- the
// engine never resolves anything through them.
struct Preset {
    std::string id;
    std::string name;
    std::string rig_id;
    std::string rig_name;
    std::vector<EffectBlockSpec> blocks;  // signal chain order, amp/cab included
};

// nlohmann::json ADL hooks (to_json/from_json) -- these throw
// nlohmann::json::exception (or audio_engine::PresetParseError, for
// messages we want to add context to) on malformed input; callers should
// catch and translate to the control socket's typed error replies.
void to_json(nlohmann::json& j, const EffectBlockSpec& b);
void from_json(const nlohmann::json& j, EffectBlockSpec& b);

void to_json(nlohmann::json& j, const Asset& a);
void from_json(const nlohmann::json& j, Asset& a);

void to_json(nlohmann::json& j, const Preset& p);
void from_json(const nlohmann::json& j, Preset& p);

struct PresetParseError : std::runtime_error {
    explicit PresetParseError(const std::string& msg) : std::runtime_error(msg) {}
};

// Parses a JSON string shaped like a control-daemon Preset export. Throws
// PresetParseError with a human-readable message on any structural
// problem (missing required field, wrong type, etc.) rather than letting
// a raw nlohmann::json::exception escape with its less friendly message.
Preset parsePresetJson(const std::string& jsonText);
std::string serializePresetJson(const Preset& preset, int indent = -1);

}  // namespace audio_engine
