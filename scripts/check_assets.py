#!/usr/bin/env python3
"""Flag NAM models and IRs that cost the audio engine a lot of CPU -- the
usual cause of crackling or "jerky" sound on a Pi. Standard library only, so
it runs on the pedal as is.

    # what the pedal has (uses the daemon's state.json for the real names)
    python3 scripts/check_assets.py --state /var/lib/multieffect-amp-modeler/state.json
    # a folder of files, before uploading them
    python3 scripts/check_assets.py ~/Music/NAM ~/Music/IR
    # also write trimmed copies of long IRs (keeps the first 100 ms, fades out)
    python3 scripts/check_assets.py ~/Music/IR --trim-ms 100 --out ~/Music/IR-trimmed

What the engine does with them (audio-engine/src):

* IRs use partitioned FFT convolution: cheap even when long (about 1% of a
  64-sample block at the cap on x86). Anything longer than 16384 samples at
  48 kHz (341 ms) is cut to 16384. Cabinet IRs rarely need more than
  50-100 ms; the rest is usually room tail or silence.
* NAM: the cost is roughly proportional to the model's weight count. A
  "SlimmableContainer" file (newer TONE3000 exports) holds several
  sub-models; the engine runs the last, largest one.
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

ENGINE_SR = 48000
IR_CAP_SAMPLES = 16384  # kMaxRealtimeIrSamples, convolution.hpp
IR_COMFORTABLE_MS = 100
NAM_HEAVY_WEIGHTS = 10000


# -- NAM ---------------------------------------------------------------------

def nam_report(path: str) -> str:
    with open(path) as fh:
        model = json.load(fh)
    arch = model.get("architecture")
    run = model
    note = ""
    if arch == "SlimmableContainer":
        subs = model["config"]["submodels"]
        run = subs[-1]["model"]
        note = " (container: runs the largest of %s)" % "/".join(
            str(len(s["model"].get("weights", []))) for s in subs)
    weights = len(run.get("weights", []))
    sr = model.get("sample_rate") or run.get("sample_rate")
    flags = []
    if weights > NAM_HEAVY_WEIGHTS:
        flags.append("heavy")
    if sr and sr != ENGINE_SR:
        flags.append(f"trained at {sr} Hz")
    return f"{run.get('architecture')} {weights} weights{note}" + (
        "  <-- " + ", ".join(flags) if flags else "")


# -- IR ----------------------------------------------------------------------

def read_wav(path: str):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    i, fmt, fmt_chunk, body = 12, None, b"", None
    while i + 8 <= len(data):
        cid, size = data[i:i + 4], struct.unpack("<I", data[i + 4:i + 8])[0]
        chunk = data[i + 8:i + 8 + size]
        if cid == b"fmt ":
            fmt, fmt_chunk = struct.unpack("<HHIIHH", chunk[:16]), chunk
        elif cid == b"data":
            body = chunk
        i += 8 + size + (size & 1)
    tag, channels, sr, _, align, bits = fmt
    if tag == 0xFFFE:  # WAVE_FORMAT_EXTENSIBLE: real format in the subformat GUID
        tag = struct.unpack("<H", fmt_chunk[24:26])[0]
    samples = []
    for k in range(len(body) // align):
        o = k * align  # first channel only, as the engine does
        if bits == 16:
            v = struct.unpack_from("<h", body, o)[0] / 32768
        elif bits == 24:
            v = int.from_bytes(body[o:o + 3], "little", signed=True) / 8388608
        elif bits == 32 and tag == 3:
            v = struct.unpack_from("<f", body, o)[0]
        elif bits == 32:
            v = struct.unpack_from("<i", body, o)[0] / 2147483648
        else:
            raise ValueError(f"{bits}-bit samples not handled")
        samples.append(v)
    return sr, channels, bits, samples


def energy_ms(samples, sr, fraction):
    total = sum(v * v for v in samples) or 1.0
    acc = 0.0
    for k, v in enumerate(samples):
        acc += v * v
        if acc >= fraction * total:
            return k / sr * 1000
    return len(samples) / sr * 1000


def ir_report(path: str):
    sr, channels, bits, x = read_wav(path)
    length_ms = len(x) / sr * 1000
    taps = min(round(len(x) * ENGINE_SR / sr), IR_CAP_SAMPLES)
    flags = []
    if taps >= IR_CAP_SAMPLES:
        flags.append(f"long: cut to {IR_CAP_SAMPLES * 1000 // ENGINE_SR} ms")
    elif length_ms > IR_COMFORTABLE_MS:
        flags.append(f"over {IR_COMFORTABLE_MS} ms")
    if channels > 1:
        flags.append("stereo (only the first channel is used)")
    e999 = energy_ms(x, sr, 0.999)
    line = (f"{length_ms:6.0f} ms, {sr} Hz, {bits}-bit, {channels}ch -> {taps} taps; "
            f"99.9% of energy in {e999:.0f} ms")
    return line + ("  <-- " + ", ".join(flags) if flags else ""), (sr, x)


def write_trimmed(path_out: str, sr: int, x, ms: float) -> None:
    n = min(len(x), int(sr * ms / 1000))
    fade = max(1, int(sr * 0.005))
    y = [v * (min(1.0, (n - k) / fade)) for k, v in enumerate(x[:n])]
    payload = struct.pack(f"<{n}f", *y)
    fmt = struct.pack("<HHIIHH", 3, 1, sr, sr * 4, 4, 32)
    with open(path_out, "wb") as fh:
        fh.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(payload)) + b"WAVE")
        fh.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        fh.write(b"data" + struct.pack("<I", len(payload)) + payload)


# -- main --------------------------------------------------------------------

def files_from_state(state_path: str):
    with open(state_path) as fh:
        state = json.load(fh)
    for asset in state.get("assets", {}).values():
        if asset.get("kind") in ("nam", "ir"):
            name = asset.get("display_name") or asset.get("filename")
            yield asset["stored_path"], name, asset["kind"]


def files_from_dirs(dirs):
    for root in dirs:
        for d, _, names in sorted(os.walk(root)):
            for f in sorted(names):
                ext = f.lower().rsplit(".", 1)[-1]
                if ext in ("nam", "wav"):
                    p = os.path.join(d, f)
                    yield p, os.path.relpath(p, root), "nam" if ext == "nam" else "ir"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dirs", nargs="*", help="folders of .nam / .wav files")
    ap.add_argument("--state", help="the daemon's state.json (checks what is uploaded)")
    ap.add_argument("--trim-ms", type=float, help="write IRs cut to this length (with a 5 ms fade)")
    ap.add_argument("--out", help="folder for the trimmed IRs")
    args = ap.parse_args()
    if not args.dirs and not args.state:
        ap.error("give folders, or --state")
    if args.trim_ms and not args.out:
        ap.error("--trim-ms needs --out")

    entries = list(files_from_state(args.state)) if args.state else list(files_from_dirs(args.dirs))
    flagged = 0
    for kind, title in (("nam", "NAM models"), ("ir", "Impulse responses")):
        rows = [e for e in entries if e[2] == kind]
        print(f"== {title} ({len(rows)})")
        for path, name, _ in rows:
            try:
                if kind == "nam":
                    line = nam_report(path)
                else:
                    line, (sr, x) = ir_report(path)
                    if args.trim_ms and len(x) / sr * 1000 > args.trim_ms:
                        os.makedirs(args.out, exist_ok=True)
                        out = os.path.join(args.out, os.path.basename(name))
                        write_trimmed(out, sr, x, args.trim_ms)
                        line += f"  [trimmed copy: {out}]"
            except Exception as exc:  # one bad file must not hide the others
                line = f"UNREADABLE: {exc}"
            flagged += "<--" in line or "UNREADABLE" in line
            print(f"  {name}\n      {line}")
    print(f"\n{flagged} file(s) flagged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
