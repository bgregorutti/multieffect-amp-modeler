// Sample-rate conversion for loaded audio (currently: cabinet IRs -- see
// convolution.hpp's loadImpulseResponseFile). The engine standardizes on
// one fixed internal sample rate (48kHz -- see ResourceManager's default
// and README.md "Sample rate policy"); any asset loaded at a different
// native rate must be converted to that rate before use, or a cabinet IR
// captured at e.g. 44.1kHz plays back time-compressed/detuned against the
// engine's 48kHz processing (confirmed with a real IR pack during manual
// testing -- see README.md).
#pragma once

#include <vector>

namespace audio_engine {

// Linear-interpolation resampling from `fromRate` to `toRate`. Naive (no
// anti-aliasing lowpass before downsampling) but correct and simple --
// the same engineering tradeoff this codebase already makes for cabinet-IR
// convolution itself (see convolution.hpp: naive O(n*m), not FFT). A
// proper windowed-sinc resampler is a real follow-up for higher fidelity,
// not attempted here. Returns `samples` unchanged (no copy-avoidance
// beyond that) if the rates already match within a small epsilon, or if
// either rate is non-positive.
std::vector<float> resampleLinear(const std::vector<float>& samples, double fromRate, double toRate);

}  // namespace audio_engine
