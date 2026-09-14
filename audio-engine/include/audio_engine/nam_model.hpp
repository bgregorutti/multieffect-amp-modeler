// Neural Amp Modeler (.nam) file support.
//
// METADATA PARSING IS REAL, and deliberately lightweight/architecture-
// agnostic. A `.nam` file is plain JSON with a top-level "architecture"
// name (e.g. "WaveNet", "LSTM", "SlimmableContainer" -- see below), an
// architecture-specific "config" object, usually a "weights" array, and
// usually "sample_rate"/"metadata". parseNamModelMetadata validates just
// the generic shape (architecture + config present) for display/bookkeeping
// purposes; it does NOT validate architecture-specific structure (e.g. it
// does not require "weights" to be non-empty, or look inside "config" at
// all) -- that's real inference's job now (see RealNamModel below), not a
// second, competing implementation of the same validation here. This
// matters concretely: newer NAM exports (e.g. "SlimmableContainer", used
// by multi-gain-stage models) put the real per-submodel weights nested
// under config.submodels[...], leaving the top-level "weights" array
// empty -- a real, valid file shape this parser must not reject.
//
// REAL INFERENCE: see real_nam_model.hpp. Built only when the CMake option
// AUDIO_ENGINE_WITH_REAL_NAM is on (default OFF), which vendors
// NeuralAmpModelerCore (MIT-licensed,
// github.com/sdatkinson/NeuralAmpModelerCore) via CMake FetchContent --
// see audio-engine/README.md "Real NAM inference". `StubNamModel` below
// remains the default when that flag is off: a fixed identity/gain
// pass-through, same "narrow interface + swappable backend" pattern used
// everywhere else in this project.
#pragma once

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "audio_engine/effect_block.hpp"

namespace audio_engine {

struct NamParseError : std::runtime_error {
    explicit NamParseError(const std::string& msg) : std::runtime_error(msg) {}
};

struct NamModelMetadata {
    std::string version;       // e.g. "0.5.3"; optional in some exports
    std::string architecture;  // e.g. "WaveNet", "LSTM", "SlimmableContainer" -- required
    nlohmann::json config;     // architecture-specific config -- required, must be an object
    std::size_t numWeights = 0;  // length of the top-level "weights" array, if any -- informational
                                  // only; 0 for container architectures whose real weights are
                                  // nested (see the file comment above)
    double sampleRate = 48000.0;  // "sample_rate" if present, else a documented default
    std::string name;           // "metadata.name" if present
    std::string modeledBy;      // "metadata.modeled_by" if present
};

// Parses and validates the generic .nam file shape. Throws NamParseError
// with a human-readable message for: invalid JSON, missing/wrong-typed
// "architecture", missing/non-object "config", or a "weights" field that's
// present but not an array of numbers (an empty "weights" array is valid
// -- see the file comment above).
NamModelMetadata parseNamModelMetadata(const std::string& jsonText);
NamModelMetadata parseNamModelFile(const std::string& path);

// Amp/preamp model inference interface. Modeled as an EffectBlock (it sits
// in the signal chain the same way any other block does) so callers can
// treat "the NAM slot" uniformly with the rest of the effect chain.
class INamModel : public EffectBlock {
public:
    virtual const NamModelMetadata& metadata() const = 0;
};

// Stub inference: does NOT run the WaveNet/LSTM described by `metadata`.
// It applies a fixed identity pass-through (optionally scaled by a
// caller-supplied makeup gain, default unity). Default when
// AUDIO_ENGINE_WITH_REAL_NAM is off -- keeps the signal chain / preset-
// switching / IPC plumbing buildable and testable with no external
// dependency, and remains the intentional fallback for a build that
// doesn't want to vendor NeuralAmpModelerCore at all (see real_nam_model.hpp).
class StubNamModel : public INamModel {
public:
    using EffectBlock::process;  // bring the std::vector<float>& convenience overload back into scope

    explicit StubNamModel(NamModelMetadata metadata, float makeupGainLinear = 1.0f);

    const NamModelMetadata& metadata() const override { return metadata_; }

    void prepare(double sampleRate) override;
    void process(float* buffer, std::size_t numSamples) override;

private:
    NamModelMetadata metadata_;
    float makeupGainLinear_;
};

}  // namespace audio_engine
