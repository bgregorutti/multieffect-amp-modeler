#!/usr/bin/env python3
"""Dev tool: build a test preset against a running control-daemon, without
the mobile app -- its OS file-picker is currently stubbed (see mobile-app/
README.md "What's stubbed"), so it can't upload a real file yet. This does
exactly what the app would do over the wire instead: optionally upload a
.nam/IR file via HTTP, register it, then create + assign-to-bank + select a
preset referencing it. See control-daemon/README.md "HTTP: uploading a
.nam/IR binary" for the exact protocol this mirrors.

Real vs. stubbed, so you know what you're actually testing:
  - gain/delay blocks (built in below): REAL DSP, audible immediately, no
    asset files needed at all.
  - --ir (a cabinet impulse response .wav): REAL time-domain convolution
    (see audio-engine/README.md "Deviations" #3) -- you WILL hear this
    change the tone.
  - --nam (a .nam model file): metadata parsing is always real. Actual
    WaveNet/LSTM inference is real too, IF the audio-engine process you're
    talking to was built with -DAUDIO_ENGINE_WITH_REAL_NAM=ON (see
    audio-engine/README.md "Real NAM inference") -- otherwise it falls
    back to a fixed identity pass-through, and this only exercises the
    upload/register/load plumbing, not real amp tone. Either way this
    script can't tell which engine build it's talking to, so it can't warn
    you if you forgot the flag.

Defaults to +6dB gain and a short slapback delay so a first run with no
extra flags already proves audio is flowing through the engine.

Usage:
    control-daemon/.venv/bin/python3 scripts/load_test_preset.py --name "Slap Delay"
    control-daemon/.venv/bin/python3 scripts/load_test_preset.py --name "With Cab" --ir path/to/cab_ir.wav
    control-daemon/.venv/bin/python3 scripts/load_test_preset.py --name "Clean" --gain-db 0 --no-delay

Presets created this way all land in the same bank (--bank, default "Test
Bank"), one per slot, in creation order -- run
scripts/keyboard_footswitch.py afterwards to switch between them.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import websockets


def upload_asset(base_url: str, kind: str, path: Path) -> dict:
    """Upload one asset, or report that the daemon already has these bytes.

    Returns upload metadata, or ``{"existing_asset_id": ...}`` when the
    daemon refuses the upload as a duplicate. The daemon deduplicates by
    content checksum rather than filename, so re-running this script (or
    pointing two rigs at the same cab IR) is expected, not an error --
    hence reusing the registered asset instead of failing the run.
    """
    data = path.read_bytes()
    # Real NAM/IR packs routinely have spaces (and other reserved
    # characters) in their filenames -- urlencode, don't just interpolate.
    query = urllib.parse.urlencode({"kind": kind, "filename": path.name})
    url = f"{base_url}/assets/upload?{query}"
    req = urllib.request.Request(url, data=data, method="POST")
    try:
        with urllib.request.urlopen(req) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as exc:
        if exc.code != 409:
            raise
        body = json.loads(exc.read())
        return {"existing_asset_id": body["existing_asset_id"], "filename": path.name}


async def register(ws: websockets.WebSocketClientProtocol, kind: str, meta: dict) -> str:
    # Already in the daemon's library (same bytes, any filename) -- nothing
    # to register, just point at what is already there.
    if "existing_asset_id" in meta:
        return meta["existing_asset_id"]

    await ws.send(
        json.dumps(
            {
                "type": "register_asset",
                "kind": kind,
                "filename": meta["filename"],
                "stored_path": meta["stored_path"],
                "size_bytes": meta["size_bytes"],
                "sha256": meta["sha256"],
            }
        )
    )
    reply = json.loads(await ws.recv())
    if reply["type"] != "command_ok":
        raise RuntimeError(f"register_asset failed: {reply}")
    await ws.recv()  # matching state_changed broadcast
    return reply["result"]["asset"]["id"]


async def main_async(args: argparse.Namespace) -> None:
    base_url = f"http://{args.host}:{args.port}"
    ws_uri = f"ws://{args.host}:{args.port}/ws"

    # Every successful command is followed by a state_changed broadcast on
    # this same connection. Keeping that broadcast's state (rather than
    # discarding it) means the script never has to guess where the daemon
    # put things -- ids and list positions are read back, not assumed.
    latest_state: dict = {}

    async def command(ws, payload: dict) -> dict:
        nonlocal latest_state
        await ws.send(json.dumps(payload))
        reply = json.loads(await ws.recv())
        if reply["type"] != "command_ok":
            raise RuntimeError(f"{payload['type']} failed: {reply}")
        broadcast = json.loads(await ws.recv())
        if broadcast.get("type") == "state_changed":
            latest_state = broadcast["state"]
        return reply["result"]

    async with websockets.connect(ws_uri) as ws:
        await ws.send(json.dumps({"type": "hello", "role": "app", "client_name": "load-test-preset"}))
        snapshot = json.loads(await ws.recv())
        if snapshot["type"] != "state_snapshot":
            raise RuntimeError(f"expected state_snapshot, got {snapshot}")
        state = snapshot["state"]

        # Find or create the rig. Everything this invocation adds goes into
        # that one rig's chain, so calling this script twice -- once with a
        # --nam, once with an --ir -- builds a single head+cab rig rather
        # than two unrelated ones.
        rigs = state["rigs"]
        rig = next((r for r in rigs if r["name"] == args.rig), None)
        if rig is None:
            rig = (await command(ws, {"type": "create_rig", "name": args.rig, "chain": []}))["rig"]
            print(f"[load-test-preset] created rig {rig['id']} ({args.rig!r})")

        chain = list(rig["chain"])

        def add_block(block_id: str, block_type: str, *, pinned: bool, asset_id=None, params=None):
            # Replace an existing block of the same id rather than stacking
            # duplicates, so re-running the script is idempotent.
            block = {
                "id": block_id,
                "type": block_type,
                "asset_id": asset_id,
                "pinned": pinned,
                "enabled": True,
                "params": params or {},
            }
            for i, existing in enumerate(chain):
                if existing["id"] == block_id:
                    chain[i] = block
                    return
            chain.append(block)

        if args.nam:
            meta = upload_asset(base_url, "nam", Path(args.nam))
            asset_id = await register(ws, "nam", meta)
            verb = "reused" if "existing_asset_id" in meta else "registered"
            print(f"[load-test-preset] {verb} NAM asset {asset_id} ({meta['filename']})")
            # Amp and cab are pinned: part of the rig's fixed backline, on in
            # every preset, never switchable by a footswitch.
            add_block("amp", "nam", pinned=True, asset_id=asset_id)

        if args.ir:
            meta = upload_asset(base_url, "ir", Path(args.ir))
            asset_id = await register(ws, "ir", meta)
            verb = "reused" if "existing_asset_id" in meta else "registered"
            print(f"[load-test-preset] {verb} IR asset {asset_id} ({meta['filename']})")
            add_block("cab", "ir", pinned=True, asset_id=asset_id)

        effect_ids = []
        if args.gain_db != 0.0:
            add_block("gain", "gain", pinned=False, params={"gain_db": args.gain_db})
            effect_ids.append("gain")
        if args.delay:
            add_block(
                "delay",
                "delay",
                pinned=False,
                params={
                    "delay_ms": args.delay_ms,
                    "feedback": args.delay_feedback,
                    "mix": args.delay_mix,
                },
            )
            effect_ids.append("delay")

        rig = (await command(ws, {"type": "update_rig", "rig_id": rig["id"], "chain": chain}))["rig"]
        pinned = " + ".join(b["type"] for b in rig["chain"] if b["pinned"]) or "none"
        print(f"[load-test-preset] rig {args.rig!r} chain: {len(rig['chain'])} block(s), pinned: {pinned}")

        # One preset turning on whatever effects this invocation added.
        preset = next((p for p in rig["presets"] if p["name"] == args.name), None)
        block_states = {bid: {"enabled": True, "params": {}} for bid in effect_ids}
        if preset is None:
            preset = (
                await command(
                    ws,
                    {
                        "type": "create_preset",
                        "rig_id": rig["id"],
                        "name": args.name,
                        "block_states": block_states,
                    },
                )
            )["preset"]
            print(f"[load-test-preset] created preset {preset['id']} ({preset['name']!r}) in rig {args.rig!r}")
        else:
            preset = (
                await command(
                    ws,
                    {
                        "type": "update_preset",
                        "rig_id": rig["id"],
                        "preset_id": preset["id"],
                        "block_states": block_states,
                    },
                )
            )["preset"]
            print(f"[load-test-preset] updated preset {preset['name']!r} in rig {args.rig!r}")

        if args.select:
            # select_preset addresses by position, not by id: presets live
            # inside rigs, and two rigs may both have a preset called "Solo".
            rig_index = next(
                i for i, r in enumerate(latest_state["rigs"]) if r["id"] == rig["id"]
            )
            preset_index = next(
                i
                for i, p in enumerate(latest_state["rigs"][rig_index]["presets"])
                if p["id"] == preset["id"]
            )
            await command(
                ws,
                {"type": "select_preset", "rig_index": rig_index, "preset_index": preset_index},
            )
            print(f"[load-test-preset] selected rig {args.rig!r} / preset {preset['name']!r}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--rig", default="Test Rig", help="rig to add these blocks to (created if missing)")
    parser.add_argument("--nam", help="path to a .nam file to upload+register (metadata only -- see module docstring)")
    parser.add_argument("--ir", help="path to a cabinet IR .wav file to upload+register (real convolution)")
    parser.add_argument("--gain-db", type=float, default=6.0)
    parser.add_argument("--no-delay", dest="delay", action="store_false", default=True)
    parser.add_argument("--delay-ms", type=float, default=300.0)
    parser.add_argument("--delay-feedback", type=float, default=0.3)
    parser.add_argument("--delay-mix", type=float, default=0.25)
    parser.add_argument("--no-select", dest="select", action="store_false", default=True)
    args = parser.parse_args()

    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()

