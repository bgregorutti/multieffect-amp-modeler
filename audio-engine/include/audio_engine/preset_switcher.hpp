// Glitch-free preset switching (product spec 4.1: "no audible glitch" when
// switching presets, via crossfade and/or background preloading).
//
// Design:
//  * A "chain" is just a callable that processes a buffer in place
//    (`ChainFn` -- matches EffectBlock::process's in-place signature, so a
//    real chain of EffectBlocks can be adapted to this with a one-line
//    lambda). PresetSwitcher knows nothing about NAM models, IRs or effect
//    blocks -- it only ever crossfades between two already-prepared chains.
//  * "Preloading": by construction, both the old and the next chain passed
//    to beginCrossfade() must already be fully prepared (NAM/IR loaded,
//    filters primed) *before* the call -- PresetSwitcher never loads
//    anything itself, so process() never performs blocking file I/O. The
//    caller (ResourceManager, see resource_manager.hpp) is responsible for
//    finishing that loading first; test_preset_switcher.cpp asserts this
//    with a call-counting mock loader.
//  * Curve: equal-power (constant perceived loudness across the fade),
//    not linear. A linear crossfade (gainOld=1-t, gainNew=t) dips in
//    perceived loudness for two uncorrelated signals (the realistic case:
//    old and new presets are generally different amp models/IRs/effects,
//    not phase-aligned copies of each other) because power doesn't sum
//    linearly. Equal-power (gainOld=cos(t*pi/2), gainNew=sin(t*pi/2))
//    keeps gainOld^2 + gainNew^2 == 1 throughout, which is the standard
//    audio-engineering choice for this exact "switch between two
//    unrelated sources" scenario. It is still perfectly monotonic and
//    smooth for two constant/correlated signals too (see
//    test_preset_switcher.cpp), which is what matters for the glitch-free
//    requirement.
#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace audio_engine {

using ChainFn = std::function<void(float* buffer, std::size_t numSamples)>;

class PresetSwitcher {
public:
    // durationSamples: length of the crossfade window, in samples at the
    // engine's current sample rate (e.g. 50ms @ 48kHz = 2400).
    void beginCrossfade(ChainFn oldChain, ChainFn newChain, std::size_t durationSamples);

    // True from beginCrossfade() until the window's samples have all been
    // produced by process().
    bool isCrossfading() const { return crossfading_; }

    // How many samples of the current crossfade have been produced so far.
    std::size_t samplesProcessed() const { return position_; }

    // Renders `numSamples` of output into `output` (resized as needed).
    // `input` is passed to both chains as their (identical) starting
    // buffer content -- each chain gets its own private copy, since a
    // ChainFn processes in place. While crossfading this blends both
    // chains' outputs; once the window completes it degenerates to
    // running only the new chain (and forgets the old one).
    void process(const std::vector<float>& input, std::vector<float>& output);

private:
    ChainFn oldChain_;
    ChainFn newChain_;
    std::size_t duration_ = 0;
    std::size_t position_ = 0;
    bool crossfading_ = false;

    std::vector<float> oldScratch_;
    std::vector<float> newScratch_;
};

}  // namespace audio_engine
