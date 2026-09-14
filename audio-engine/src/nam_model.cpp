#include "audio_engine/nam_model.hpp"

#include <fstream>
#include <sstream>

namespace audio_engine {

using nlohmann::json;

NamModelMetadata parseNamModelMetadata(const std::string& jsonText) {
    json j;
    try {
        j = json::parse(jsonText);
    } catch (const json::exception& e) {
        throw NamParseError(std::string("invalid JSON: ") + e.what());
    }

    if (!j.is_object()) throw NamParseError(".nam file root must be a JSON object");

    NamModelMetadata meta;

    auto archIt = j.find("architecture");
    if (archIt == j.end() || !archIt->is_string() || archIt->get<std::string>().empty()) {
        throw NamParseError("missing or invalid required field 'architecture' (expected non-empty string)");
    }
    meta.architecture = archIt->get<std::string>();

    auto configIt = j.find("config");
    if (configIt == j.end() || !configIt->is_object()) {
        throw NamParseError("missing or invalid required field 'config' (expected a JSON object)");
    }
    meta.config = *configIt;

    // "weights" is informational only, not required to be present or
    // non-empty: container architectures (e.g. "SlimmableContainer") nest
    // their real weights under config.submodels[...] instead -- see the
    // file comment in nam_model.hpp. Still reject a clearly malformed
    // "weights" (present but not an array of numbers), since that's a
    // real structural problem regardless of architecture.
    auto weightsIt = j.find("weights");
    if (weightsIt != j.end()) {
        if (!weightsIt->is_array()) {
            throw NamParseError("'weights' field, if present, must be an array");
        }
        for (const auto& w : *weightsIt) {
            if (!w.is_number()) {
                throw NamParseError("'weights' array must contain only numbers");
            }
        }
        meta.numWeights = weightsIt->size();
    }

    auto versionIt = j.find("version");
    if (versionIt != j.end() && versionIt->is_string()) meta.version = versionIt->get<std::string>();

    auto srIt = j.find("sample_rate");
    if (srIt != j.end() && srIt->is_number()) meta.sampleRate = srIt->get<double>();

    auto metaIt = j.find("metadata");
    if (metaIt != j.end() && metaIt->is_object()) {
        auto nameIt = metaIt->find("name");
        if (nameIt != metaIt->end() && nameIt->is_string()) meta.name = nameIt->get<std::string>();
        auto modeledByIt = metaIt->find("modeled_by");
        if (modeledByIt != metaIt->end() && modeledByIt->is_string()) meta.modeledBy = modeledByIt->get<std::string>();
    }

    return meta;
}

NamModelMetadata parseNamModelFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw NamParseError("could not open file: " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return parseNamModelMetadata(ss.str());
}

StubNamModel::StubNamModel(NamModelMetadata metadata, float makeupGainLinear)
    : metadata_(std::move(metadata)), makeupGainLinear_(makeupGainLinear) {}

void StubNamModel::prepare(double /*sampleRate*/) {
    // Real inference would need to be re-primed for a new sample rate;
    // the stub has no internal state to prepare.
}

void StubNamModel::process(float* buffer, std::size_t numSamples) {
    if (makeupGainLinear_ == 1.0f) return;  // pure identity fast path
    for (std::size_t i = 0; i < numSamples; ++i) buffer[i] *= makeupGainLinear_;
}

}  // namespace audio_engine
