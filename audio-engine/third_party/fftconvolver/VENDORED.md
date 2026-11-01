# FFTConvolver (vendored)

- Source: https://github.com/Darkglass-Electronics/FFTConvolver (branch
  `non-uniform`, commit 3048535d8fa9e12f9eb9141770cb1be3392d6833),
  a fork of https://github.com/HiFi-LoFi/FFTConvolver.
- License: MIT, see `COPYING.txt` (must stay with these files).
- Unmodified copy of the library sources. Only `FFTConvolver` (uniform
  partitions) is used by `ConvolutionEngine`; `TwoStageFFTConvolver` is
  kept for benchmarking (`tools/conv_bench.cpp`).
