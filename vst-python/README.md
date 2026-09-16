# vst-python — DSP pedal models in Python

Hand-written DSP models of guitar pedals, kept **deliberately separate from the
C++ in `audio-engine/`**. This is a reference implementation and a testing
bench, not what runs on the Pi: the Python model is where a pedal's behaviour
gets designed, measured and argued about, and the C++ block is then ported from
it and validated against it.

Nothing here is a VST plugin, despite the directory name — these are plain
Python classes plus a WAV renderer. Loading them *into* a DAW would mean a
plugin wrapper, which is a separate question (see "Open questions").

## Why DSP instead of a NAM capture

A `.nam` file bakes in the knob positions it was captured at, and most of what
a pedal's knobs do is not something a neural network should be spent on:

| Control | What it is in the circuit | Cost to model directly |
|---|---|---|
| **Volume / Level** | Post-gain output level | A single multiply |
| **Tone** | Passive filter network — entirely **linear** | One or two one-poles |
| **Sustain / Drive** | Pre-gain into the clipping stages | Genuinely reshapes the nonlinearity |

Only the gain control touches the nonlinear part. Capturing the others
neurally spends an inference budget to reproduce a multiply and a filter — and
still leaves the knobs frozen where they were captured, which the mobile app's
per-block parameter editing assumes it can move.

NAM remains the right tool for the **amp**. It is the wrong tool for pedals,
and for gates, delay, reverb and modulation it cannot work at all — those need
either time-varying gain or memory far longer than its receptive field.

## Layout

```
src/pedals/dsp.py            One-pole filters, soft clipper, stateful oversampler
src/pedals/bigmuff.py        Big Muff Pi fuzz
src/pedals/tubescreamer.py   TS808 overdrive
src/pedals/noisegate.py      Noise gate / downward expander
tools/render.py              Render a WAV through a chain (the A/B workflow)
tests/                       103 tests
```

## Setup

```bash
cd vst-python
uv venv --python 3.12 .venv
uv pip install --python .venv/bin/python -e ".[dev]"
.venv/bin/python -m pytest -q          # 103 passed
```

All three models share one interface:

```python
from pedals import BigMuff, TubeScreamer, NoiseGate

pedal = TubeScreamer(sample_rate=48000)
pedal.set_params(drive=0.7, tone=0.6, level=0.5)
wet = pedal.process(dry_block)          # any block size
```

---

## Big Muff

```
in → pad → input HPF → Sustain (pre-gain)
   → [4x oversampled]  clip stage 1 → clip stage 2
   → DC block → tone stack → Volume → out
```

Each clipping stage is a coupling highpass, a fixed gain, an asymmetric `tanh`
saturator (diodes in the transistor's feedback loop — soft and compressive,
unlike a Rat's hard diodes-to-ground clip) and a lowpass modelling the cap
across those diodes. That last filter is what naive distortion models skip, and
most of why they sound like fizzing wasps.

Controls: `sustain`, `tone`, `volume`, `pad_15db`, `bypass`.

| Property | Measured |
|---|---|
| Tone-stack mid scoop at noon | **−9.2 dB at 949 Hz** (matches the published circuit response) |
| Aliasing energy, 4x vs none | 1.49% → 0.022%, **18.3 dB better** |
| Saturation at full Sustain | 4x input → **1.00x** output — fully squared off |
| Added latency | 16 samples = **0.33 ms** |
| Cost, 256-sample block | **0.113 ms = 2.1% of a 5.33 ms budget** |

## Tube Screamer

```
in → input HPF
   → [4x oversampled]  dry + clip(highpass(dry) × Drive)
   → DC block → fixed LPF → Tone → Level → out
```

Structurally this is **not** a milder Big Muff, and modelling it as one is the
usual way to get it wrong. Two things define it:

- **The dry signal never leaves.** The diodes sit in the feedback loop of a
  *non-inverting* stage, so the output is the input *plus* the clipped path.
  That is why a Tube Screamer sounds like a boost with hair on it rather than a
  fuzz, and why it cleans up when you roll the guitar's volume back.
- **Bass never reaches the clipper.** The feedback leg is 4.7k in series with
  0.047 µF, so stage gain falls to unity below ~720 Hz. That single highpass is
  the entire reason for the midrange focus — it is a *gain* shape, before the
  clipping, not a tone control after it.

Most constants are derived from TS808 component values rather than fitted by
ear: the 720 Hz corner, and the 21.5–41.4 dB drive range from
`1 + (51k + pot)/4.7k` with a 500k pot.

Controls: `drive`, `tone`, `level`, `bypass`.

| Property | Measured |
|---|---|
| Midrange hump | **+14.1 dB at 1.5 kHz** relative to 82 Hz |
| Dry survival, 4x input | **1.42x** output, against the Big Muff's 1.00x |
| Bass stays cleaner than treble | growth 1.67x @ 82 Hz → 1.31x @ 2 kHz |
| Added latency | 16 samples = **0.33 ms** |
| Cost, 256-sample block | **0.088 ms = 1.7% of budget** |

The Drive knob's *level* range is narrow — peak output stops moving past about
0.75 — which is authentic for a famously one-sound pedal. Harmonic content
keeps rising across the full sweep.

## Noise gate

**What it can and cannot do.** A gate removes noise *in the gaps*: it shuts the
path when you are not playing, so an upstream pedal's hiss never reaches the
amp. It cannot remove noise riding underneath a note you are actually playing —
while open it passes the signal through untouched, hiss included. Doing that
needs spectral subtraction against a noise profile, which costs an FFT of
latency and smears transients. Every guitar noise gate ever built — ISP
Decimator, Boss NS-2 — works the way this one does, because in practice the
hiss is only objectionable in the silences.

Set `threshold_db` just above the noise floor and just below your quietest
intended note. The rest have working defaults.

Three details separate a gate that works from one that chatters:

- **Hysteresis** — opens at the threshold, closes a few dB *below* it.
- **Hold** — stays open a minimum time, so notes decay instead of being chopped.
- **Range** — the closed state ducks by a set amount rather than going silent.
  A gate slamming to digital zero is *more* noticeable, because the noise floor
  vanishing entirely is itself an audible event.

Controls (in real units, matching the C++ block registry): `threshold_db`,
`range_db`, `attack_ms`, `hold_ms`, `release_ms`, `hysteresis_db`, `bypass`.

| Property | Measured |
|---|---|
| Noise floor in the gaps | −56.5 dB → **−116.5 dB** (−60 dB, the `range_db` setting) |
| Signal while playing | −17.8 dB → **−17.8 dB (−0.0)** — untouched |
| Chatter on a decaying note | 10 open/close flips without hysteresis, **2** with |
| Added latency | **0** — detection is causal, no lookahead |
| Cost, 256-sample block | **0.059 ms = 1% of budget** (229 ns/sample) |

### Side-chaining (smart gate)

`process` takes an optional `sidechain` to detect from, while the gain is still
applied to the main input. Feeding it the clean guitar while gating the
distorted output is how an ISP Decimator G-String works: detection keys off
your playing dynamics rather than off a compressed distortion signal whose
level barely moves between a held note and silence.

```python
gate.process(distorted, sidechain=clean_guitar)
```

---

## Rendering and A/B

```bash
# one pedal
.venv/bin/python tools/render.py di.wav out.wav -p big_muff:sustain=0.8,tone=0.35

# a chain, applied in order
.venv/bin/python tools/render.py di.wav out.wav \
    -p tube_screamer:drive=0.7,tone=0.5 \
    -p noise_gate:threshold_db=-45

# smart gate: gate the fuzz, detect from the clean input
.venv/bin/python tools/render.py di.wav out.wav \
    -p big_muff:sustain=0.9 \
    -p noise_gate:threshold_db=-40,sidechain=input
```

`--block-size` is honoured, so this exercises the same block-based path the C++
engine will use rather than a one-shot offline render.

## Parity with the C++ port

These models are the reference for `audio-engine`'s `big_muff`,
`tube_screamer` and `noise_gate` blocks, and that is enforced rather than
asserted. `tools/export_golden.py` freezes a render from each model into
`audio-engine/tests/golden/pedal_parity.json`; `test_python_parity.cpp`
replays the identical input through the C++ block and compares to **1e-6**
(the C++ keeps every intermediate in `double` and rounds once into its `float`
buffer, so that is a single quantization, not a fudge factor).

**After any deliberate change to a model here, regenerate the fixture** --
otherwise the C++ suite fails, which is the point:

```bash
.venv/bin/python tools/export_golden.py \
    ../audio-engine/tests/golden/pedal_parity.json
```

It has already earned its keep: the first parity run failed on the gate alone,
and the bug was *here*, not in the port -- `NoiseGate.reset()` runs from
`__init__` before `set_params`, so the gate started at the default floor
instead of the configured one and ramped between them at the start of every
render. The closed gain is now resolved lazily on the first sample.

## Tuning against a reference

The circuit constants at the top of each model are the tuning surface — stage
gains, clipping bias, coupling and feedback corners, tone branches. Render the
same DI take through this and through a reference plugin, then compare by ear
and by spectrum.

## What the tests are actually for

Two patterns carry most of the weight:

- **`test_is_block_size_invariant`** (all three models) — processing in blocks
  of 1, 32, 64, 441 or 1024 gives bit-identical output to one whole-signal
  call. This is what makes the models a valid reference for a real-time engine,
  and what catches mishandled filter state.
- **`test_oversampling_suppresses_aliasing`** — proves the oversampler earns
  its cost. The test tone is 3137 Hz specifically because it is not a rational
  fraction of the sample rate; with a tidier fundamental the aliases would fold
  back exactly onto the harmonic grid and hide inside the bins the test
  excludes.

The rest assert circuit behaviour rather than implementation details: the Muff
scoops hardest at noon, the Tube Screamer keeps its dry signal where the Muff
saturates flat, bass stays cleaner than treble, the gate crushes the floor
without touching the note.

A note on how these were arrived at: several first drafts failed, and in every
case the *metric* was wrong rather than the model — comparing harmonic counts
across frequencies the pedal's own rolloff treats differently, or asserting
that a digital one-pole highpass reaches unity gain. Each was diagnosed by
measurement before the test was rewritten. The one real bug the process caught
was in the gate's envelope follower, whose decay was fast enough to sag more
than the hysteresis window on low notes.

## Known gaps

- **No parameter smoothing.** Parameters are applied per block, so moving a
  knob mid-stream will click. Deliberately omitted: smoothing across a block
  would break block-size invariance, which is the property these models exist
  to guarantee. The C++ port *does* smooth, and reconciles the two by snapping
  on construction/prepare/reset and ramping only on a live change -- see
  "Parity with the C++ port" below.
- **Mono only.** All three reject anything but a 1-D block.
- **The gate runs a per-sample Python loop.** Its envelope and state machine
  are inherently sequential, so unlike the others it is not vectorized. It is
  still only 1% of budget, and trivially cheap in C++.
- **The Big Muff tone stack is a blend approximation** — two independent
  one-poles blended by the pot, not the interacting RC network with the pot's
  loading. The measured notch lands on the published figure, so it holds where
  it matters.
- **Only the Big Muff has been checked against a real plugin** (NA Big Stuff,
  in a DAW). Every Tube Screamer and gate number above is self-measured.

## Open questions

- **Does this want a plugin wrapper?** Loading these into a DAW for A/B would
  need one. Not built, because the C++ engine is what ships — but it would make
  the tuning loop much tighter.
- **Which pedal next?** The same filter → clip → filter skeleton covers a Rat
  (hard clip to ground, single-knob filter) and a Klon-style transparent drive
  (a TS variant with a much higher dry/wet ratio).
