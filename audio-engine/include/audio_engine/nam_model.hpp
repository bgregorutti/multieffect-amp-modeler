// Neural Amp Modeler (.nam) file support.
//
// METADATA PARSING IS REAL. A `.nam` file is plain JSON with (per the
// format used by github.com/sdatkinson/NeuralAmpModelerCore, the reference
// C++ implementation): a top-level "architecture" name (e.g. "WaveNet",
// "LSTM"), an architecture-specific "config" object, a flat "weights"
// array, and usually "sample_rate"/"metadata". parseNamModelMetadata below
// really parses and validates that structure and rejects malformed files.
//
// ACTUAL INFERENCE IS STUBBED -- documented deviation, see
// audio-engine/README.md "NAM inference stub" section. Running the real
// WaveNet/LSTM forward pass encoded by "weights" requires vendoring
// NeuralAmpModelerCore (MIT-licensed, github.com/sdatkinson/NeuralAmpModelerCore).
// That repository can only be fetched via `git clone`/GitHub archive
// download, both of which this sandbox's network proxy blocks (plain
// `https://raw.githubusercontent.com` GETs work, but `codeload.github.com`
// -- which git clone and tarball downloads both resolve to -- returns 403;
// see the README for the confirmed test). `INamModel` is the seam: the
// stub can be swapped for a real implementation backed by that library
// with no change to any caller, once it can be vendored (e.g. as a git
// submodule from a machine with full GitHub access).
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
    std::string architecture;  // e.g. "WaveNet", "LSTM" -- required
    nlohmann::json config;     // architecture-specific config -- required, must be an object
    std::size_t numWeights = 0;  // length of the "weights" array -- required, must be non-empty
    double sampleRate = 48000.0;  // "sample_rate" if present, else a documented default
    std::string name;           // "metadata.name" if present
    std::string modeledBy;      // "metadata.modeled_by" if present
};

// Parses and validates .nam file JSON. Throws NamParseError with a
// human-readable message for any of: invalid JSON, missing/wrong-typed
// "architecture", missing/non-object "config", missing/empty/non-numeric
// "weights".
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
// caller-supplied makeup gain, default unity) so the rest of the signal
// chain / preset-switching / IPC plumbing can be built and tested end to
// end before real inference exists. See the class comment above for why.
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
