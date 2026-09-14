#include "audio_engine/preset_model.hpp"

#include <stdexcept>

namespace audio_engine {

using nlohmann::json;

std::string toString(AssetKind kind) {
    switch (kind) {
        case AssetKind::Nam:
            return "nam";
        case AssetKind::Ir:
            return "ir";
    }
    throw PresetParseError("unreachable: unknown AssetKind");
}

AssetKind assetKindFromString(const std::string& s) {
    if (s == "nam") return AssetKind::Nam;
    if (s == "ir") return AssetKind::Ir;
    throw PresetParseError("invalid asset kind '" + s + "' (expected 'nam' or 'ir')");
}

namespace {

json paramValueToJson(const ParamValue& v) {
    return std::visit([](auto&& val) -> json { return val; }, v);
}

ParamValue paramValueFromJson(const json& j) {
    if (j.is_boolean()) return ParamValue{j.get<bool>()};
    if (j.is_number()) return ParamValue{j.get<double>()};
    if (j.is_string()) return ParamValue{j.get<std::string>()};
    throw PresetParseError("effect block param values must be float/int/str/bool");
}

template <typename T>
T requireField(const json& j, const char* name) {
    auto it = j.find(name);
    if (it == j.end() || it->is_null()) {
        throw PresetParseError(std::string("missing required field '") + name + "'");
    }
    try {
        return it->get<T>();
    } catch (const json::exception& e) {
        throw PresetParseError(std::string("field '") + name + "' has wrong type: " + e.what());
    }
}

template <typename T>
T fieldOr(const json& j, const char* name, T fallback) {
    auto it = j.find(name);
    if (it == j.end() || it->is_null()) return fallback;
    try {
        return it->get<T>();
    } catch (const json::exception& e) {
        throw PresetParseError(std::string("field '") + name + "' has wrong type: " + e.what());
    }
}

// Nullable string field: JSON null and an absent key both mean "not set"
// (the daemon emits an explicit null, but tolerating absence keeps
// hand-written/partial payloads working); anything that is present and
// non-null must actually be a string, or it's a parse error rather than a
// silently dropped value.
std::optional<std::string> optionalStringField(const json& j, const char* name) {
    auto it = j.find(name);
    if (it == j.end() || it->is_null()) return std::nullopt;
    if (!it->is_string()) {
        throw PresetParseError(std::string("field '") + name + "' must be a string or null");
    }
    return it->get<std::string>();
}

}  // namespace

void to_json(json& j, const EffectBlockSpec& b) {
    json params = json::object();
    for (const auto& [k, v] : b.params) {
        params[k] = paramValueToJson(v);
    }
    j = json{{"id", b.id},
             {"type", b.type},
             {"asset_id", b.asset_id.has_value() ? json(*b.asset_id) : json(nullptr)},
             {"enabled", b.enabled},
             {"params", params}};
}

void from_json(const json& j, EffectBlockSpec& b) {
    if (!j.is_object()) throw PresetParseError("effect block must be a JSON object");
    b.id = requireField<std::string>(j, "id");
    b.type = requireField<std::string>(j, "type");
    b.asset_id = optionalStringField(j, "asset_id");
    b.enabled = fieldOr<bool>(j, "enabled", true);
    b.params.clear();
    auto it = j.find("params");
    if (it != j.end() && !it->is_null()) {
        if (!it->is_object()) throw PresetParseError("effect block 'params' must be an object");
        for (auto pit = it->begin(); pit != it->end(); ++pit) {
            b.params.emplace(pit.key(), paramValueFromJson(pit.value()));
        }
    }
}

void to_json(json& j, const Asset& a) {
    j = json{{"id", a.id},
             {"kind", toString(a.kind)},
             {"filename", a.filename},
             {"stored_path", a.stored_path},
             {"size_bytes", a.size_bytes},
             {"sha256", a.sha256.has_value() ? json(*a.sha256) : json(nullptr)},
             {"uploaded_at", a.uploaded_at}};
}

void from_json(const json& j, Asset& a) {
    if (!j.is_object()) throw PresetParseError("asset must be a JSON object");
    a.id = requireField<std::string>(j, "id");
    a.kind = assetKindFromString(requireField<std::string>(j, "kind"));
    a.filename = requireField<std::string>(j, "filename");
    a.stored_path = requireField<std::string>(j, "stored_path");
    a.size_bytes = fieldOr<long long>(j, "size_bytes", 0);
    auto shaIt = j.find("sha256");
    if (shaIt != j.end() && !shaIt->is_null()) {
        a.sha256 = shaIt->get<std::string>();
    } else {
        a.sha256.reset();
    }
    a.uploaded_at = fieldOr<double>(j, "uploaded_at", 0.0);
}

void to_json(json& j, const Preset& p) {
    j = json{{"id", p.id},
             {"name", p.name},
             {"rig_id", p.rig_id},
             {"rig_name", p.rig_name},
             {"blocks", p.blocks}};
}

void from_json(const json& j, Preset& p) {
    if (!j.is_object()) throw PresetParseError("preset must be a JSON object");
    p.id = requireField<std::string>(j, "id");
    p.name = requireField<std::string>(j, "name");
    // Required in the daemon's ResolvedPreset (models.py), so required
    // here too -- this file's whole point is that optionality matches.
    p.rig_id = requireField<std::string>(j, "rig_id");
    p.rig_name = requireField<std::string>(j, "rig_name");
    p.blocks.clear();
    auto blocksIt = j.find("blocks");
    if (blocksIt != j.end() && !blocksIt->is_null()) {
        if (!blocksIt->is_array()) throw PresetParseError("preset 'blocks' must be an array");
        // Order is the signal chain order (amp and cab included), so it is
        // preserved exactly as it arrives -- never sorted or regrouped.
        for (const auto& blockJson : *blocksIt) {
            p.blocks.push_back(blockJson.get<EffectBlockSpec>());
        }
    }
}

Preset parsePresetJson(const std::string& jsonText) {
    json j;
    try {
        j = json::parse(jsonText);
    } catch (const json::exception& e) {
        throw PresetParseError(std::string("invalid JSON: ") + e.what());
    }
    return j.get<Preset>();
}

std::string serializePresetJson(const Preset& preset, int indent) {
    json j = preset;
    return indent >= 0 ? j.dump(indent) : j.dump();
}

}  // namespace audio_engine
