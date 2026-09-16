#!/usr/bin/env python3
"""Render a WAV file through a chain of pedal models.

Its main use is A/B-ing against reference plugins: render the same DI take
through both, then compare by ear and by spectrum.

    # one pedal
    python tools/render.py di.wav out.wav -p big_muff:sustain=0.8,tone=0.35

    # a chain, in order
    python tools/render.py di.wav out.wav \\
        -p tube_screamer:drive=0.7,tone=0.5 \\
        -p noise_gate:threshold_db=-45

    # smart gate: gate the distorted signal, but detect from the clean input
    python tools/render.py di.wav out.wav \\
        -p big_muff:sustain=0.9 \\
        -p noise_gate:threshold_db=-40,sidechain=input

Processing runs in blocks (``--block-size``) rather than one shot, so this
exercises exactly the path the C++ engine will use.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np
from scipy.io import wavfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from pedals import PEDALS, NoiseGate  # noqa: E402


def parse_value(text: str) -> float | bool:
    lowered = text.strip().lower()
    if lowered in ("true", "on", "yes"):
        return True
    if lowered in ("false", "off", "no"):
        return False
    return float(text)


def parse_pedal(spec: str) -> tuple[str, dict, bool]:
    """Parse ``name:key=value,key=value`` into a name, params and a flag.

    The flag reports whether ``sidechain=input`` was requested -- a pseudo
    parameter rather than a real one, since it selects a signal routing rather
    than setting anything on the pedal.
    """
    name, _, param_text = spec.partition(":")
    name = name.strip()
    if name not in PEDALS:
        raise SystemExit(
            f"unknown pedal {name!r}; available: {', '.join(sorted(PEDALS))}"
        )

    params: dict[str, float | bool] = {}
    sidechain_from_input = False
    for pair in filter(None, (p.strip() for p in param_text.split(","))):
        key, _, value = pair.partition("=")
        key = key.strip()
        if not value:
            raise SystemExit(f"parameter {key!r} in {spec!r} needs a value")
        if key == "sidechain":
            if value.strip() != "input":
                raise SystemExit("sidechain only accepts 'input'")
            sidechain_from_input = True
            continue
        params[key] = parse_value(value)
    return name, params, sidechain_from_input


def read_mono(path: Path) -> tuple[int, np.ndarray]:
    """Read a WAV as float64 in -1..1, taking the left channel if stereo."""
    sample_rate, data = wavfile.read(path)
    if data.ndim > 1:
        data = data[:, 0]
    if np.issubdtype(data.dtype, np.integer):
        data = data.astype(np.float64) / float(abs(np.iinfo(data.dtype).min))
    return sample_rate, data.astype(np.float64)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("input", type=Path, help="input WAV (a dry DI take)")
    parser.add_argument("output", type=Path, help="output WAV to write")
    parser.add_argument(
        "-p", "--pedal", action="append", default=[], metavar="NAME:K=V,...",
        help="a pedal to add to the chain; repeat for more, applied in order",
    )
    parser.add_argument("--block-size", type=int, default=256)
    parser.add_argument("--oversample", type=int, default=4)
    args = parser.parse_args(argv)

    if not args.pedal:
        raise SystemExit(f"no pedals given; available: {', '.join(sorted(PEDALS))}")

    sample_rate, dry = read_mono(args.input)

    chain = []
    for spec in args.pedal:
        name, params, keyed = parse_pedal(spec)
        cls = PEDALS[name]
        # The gate has no nonlinearity, so it takes no oversample argument.
        pedal = (
            cls(sample_rate=sample_rate)
            if cls is NoiseGate
            else cls(sample_rate=sample_rate, oversample=args.oversample)
        )
        try:
            pedal.set_params(**params)
        except (ValueError, TypeError) as exc:
            raise SystemExit(f"{name}: {exc}") from exc
        if keyed and not isinstance(pedal, NoiseGate):
            raise SystemExit(f"{name} does not take a sidechain")
        chain.append((pedal, keyed))

    out = np.empty_like(dry)
    for start in range(0, len(dry), args.block_size):
        block = dry[start:start + args.block_size]
        key = block
        signal = block
        for pedal, keyed in chain:
            if keyed:
                signal = pedal.process(signal, sidechain=key)
            else:
                signal = pedal.process(signal)
        out[start:start + len(signal)] = signal

    peak = float(np.max(np.abs(out))) if out.size else 0.0
    if peak > 1.0:
        print(f"warning: output peaked at {peak:.2f}, clamping", file=sys.stderr)
        out = np.clip(out, -1.0, 1.0)

    wavfile.write(args.output, sample_rate, out.astype(np.float32))
    latency = sum(pedal.latency_samples for pedal, _ in chain)
    print(
        f"{args.input.name} -> {args.output.name}  "
        f"{len(dry) / sample_rate:.2f}s @ {sample_rate} Hz  |  "
        f"{' -> '.join(p.TYPE for p, _ in chain)}  |  "
        f"peak {peak:.3f}, latency {latency:.0f} samples"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
