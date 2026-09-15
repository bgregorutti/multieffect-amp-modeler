#!/usr/bin/env python3
"""Dev tool: build one rig the way the real UX intends it -- a fixed
backline (head + cab + input gain + output volume, pinned, unaffected by
preset switching) plus a small palette of *native* switchable effect
blocks that presets pick combinations of. Two explicit phases, run against
a live control-daemon:

  1. upload -- register the head .nam and cab IR (the only real files
     involved; gain/delay are parametrized blocks, no asset needed).
  2. chaining -- build the rig's chain once (pinned backline + switchable
     effects), then create every preset with a *complete*, explicit
     enabled/disabled state for every switchable block.

Why not just call scripts/load_test_preset.py four times, once per preset?
That script is additive and per-invocation: a preset it creates only ever
lists the effect ids *this call* turned on, and control_daemon.models.
resolve_preset falls back to the block's own rig-level `enabled` (which
add_block always leaves at True) for any id a preset's block_states
doesn't mention. So a "Clean" preset built by a call that added no
effects wouldn't actually silence effects a *different* call already
added to the shared chain -- it would inherit them as on. Explicit
per-preset state for every switchable block (this script) sidesteps that
instead of relying on call order.

No VST3 here on purpose: the plugins currently on hand either don't
process audio headlessly (PACE/iLok-protected, see audio-engine session
notes) or aren't the right architecture for this Mac. This uses the
engine's real, unconditionally-working native blocks (gain boost, delay)
as the switchable "effects" instead -- swap in vst3 blocks later once a
working plugin is confirmed, same chain-building shape either way.

Usage:
    control-daemon/.venv/bin/python3 scripts/build_guitar_rig.py
    control-daemon/.venv/bin/python3 scripts/build_guitar_rig.py --rig "Guitar Rig" --nam path/to/head.nam --ir path/to/cab.wav
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
    """Upload one asset, or report that the daemon already has these bytes
    (content-addressed dedup -- see control-daemon/README.md). Reusing the
    same cab IR (or head .nam) across rigs/runs is expected, not an error."""
    data = path.read_bytes()
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
    if "existing_asset_id" in meta:
        return meta["existing_asset_id"]
    await ws.send(json.dumps({
        "type": "register_asset",
        "kind": kind,
        "filename": meta["filename"],
        "stored_path": meta["stored_path"],
        "size_bytes": meta["size_bytes"],
        "sha256": meta["sha256"],
    }))
    reply = json.loads(await ws.recv())
    if reply["type"] != "command_ok":
        raise RuntimeError(f"register_asset failed: {reply}")
    await ws.recv()  # matching state_changed broadcast
    return reply["result"]["asset"]["id"]


async def main_async(args: argparse.Namespace) -> None:
    base_url = f"http://{args.host}:{args.port}"
    ws_uri = f"ws://{args.host}:{args.port}/ws"

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
        await ws.send(json.dumps({"type": "hello", "role": "app", "client_name": "build-guitar-rig"}))
        snapshot = json.loads(await ws.recv())
        if snapshot["type"] != "state_snapshot":
            raise RuntimeError(f"expected state_snapshot, got {snapshot}")
        latest_state = snapshot["state"]

        # ---- Phase 1: upload ----------------------------------------------
        nam_meta = upload_asset(base_url, "nam", Path(args.nam))
        nam_asset_id = await register(ws, "nam", nam_meta)
        print(f"[build-guitar-rig] head asset {nam_asset_id} ({nam_meta['filename']})")

        ir_meta = upload_asset(base_url, "ir", Path(args.ir))
        ir_asset_id = await register(ws, "ir", ir_meta)
        print(f"[build-guitar-rig] cab asset {ir_asset_id} ({ir_meta['filename']})")

        # ---- Phase 2: chaining ---------------------------------------------
        rigs = latest_state["rigs"]
        rig = next((r for r in rigs if r["name"] == args.rig), None)
        if rig is None:
            # Omitting "chain" seeds the standard pinned skeleton:
            # input_trim(gain) -> amp(nam) -> cab(ir) -> tone_stack -> output_volume
            # (control_daemon.models.default_rig_chain).
            rig = (await command(ws, {"type": "create_rig", "name": args.rig}))["rig"]
            print(f"[build-guitar-rig] created rig {rig['id']} ({args.rig!r})")

        chain = list(rig["chain"])

        def replace_block(block_id: str, **updates) -> None:
            """Pinned blocks (input_trim/amp/cab/tone_stack/output_volume)
            already exist from the default skeleton -- just patch fields."""
            for i, block in enumerate(chain):
                if block["id"] == block_id:
                    chain[i] = {**block, **updates}
                    return
            raise KeyError(f"rig {args.rig!r} has no block id {block_id!r} to update")

        def upsert_switchable(block_id: str, block_type: str, params: dict) -> None:
            """Insert a non-pinned, preset-switchable block right before the
            cab (so it lands amp -> ...switchable effects... -> cab, per the
            required order) if it's not already there; otherwise just
            refresh its rig-level default params, keeping its position and
            leaving pinned/enabled alone."""
            for i, block in enumerate(chain):
                if block["id"] == block_id:
                    chain[i] = {**block, "params": params}
                    return
            cab_index = next((i for i, b in enumerate(chain) if b["id"] == "cab"), len(chain))
            chain.insert(cab_index, {
                "id": block_id,
                "type": block_type,
                "asset_id": None,
                "pinned": False,
                "enabled": True,
                "params": params,
            })

        replace_block("amp", asset_id=nam_asset_id)
        replace_block("cab", asset_id=ir_asset_id)
        upsert_switchable("boost", "gain", {"gain_db": args.boost_db})
        upsert_switchable("delay", "delay", {
            "delay_ms": args.delay_ms,
            "feedback": args.delay_feedback,
            "mix": args.delay_mix,
        })

        rig = (await command(ws, {"type": "update_rig", "rig_id": rig["id"], "chain": chain}))["rig"]
        pinned = " + ".join(b["type"] for b in rig["chain"] if b["pinned"])
        switchable = " + ".join(b["id"] for b in rig["chain"] if not b["pinned"])
        print(f"[build-guitar-rig] rig {args.rig!r} chain: pinned [{pinned}], switchable [{switchable}]")

        # Every preset states BOTH switchable blocks explicitly -- never
        # relies on an id being absent to mean "off" (see module docstring).
        preset_specs = [
            ("Clean", {"boost": False, "delay": False}),
            ("Boost", {"boost": True, "delay": False}),
            ("Slap Delay", {"boost": False, "delay": True}),
            ("Boost + Delay", {"boost": True, "delay": True}),
        ]

        first_preset_index = None
        for name, combo in preset_specs:
            block_states = {bid: {"enabled": enabled, "params": {}} for bid, enabled in combo.items()}
            existing = next((p for p in rig["presets"] if p["name"] == name), None)
            if existing is None:
                preset = (await command(ws, {
                    "type": "create_preset", "rig_id": rig["id"], "name": name, "block_states": block_states,
                }))["preset"]
                print(f"[build-guitar-rig] created preset {preset['name']!r}: {combo}")
            else:
                preset = (await command(ws, {
                    "type": "update_preset", "rig_id": rig["id"], "preset_id": existing["id"],
                    "block_states": block_states,
                }))["preset"]
                print(f"[build-guitar-rig] updated preset {preset['name']!r}: {combo}")

            rig_index = next(i for i, r in enumerate(latest_state["rigs"]) if r["id"] == rig["id"])
            preset_index = next(
                i for i, p in enumerate(latest_state["rigs"][rig_index]["presets"]) if p["id"] == preset["id"]
            )
            if name == "Clean":
                first_preset_index = (rig_index, preset_index)

        if args.select and first_preset_index is not None:
            rig_index, preset_index = first_preset_index
            await command(ws, {"type": "select_preset", "rig_index": rig_index, "preset_index": preset_index})
            print(f"[build-guitar-rig] selected rig {args.rig!r} / preset 'Clean'")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--rig", default="Guitar Rig")
    parser.add_argument("--nam", required=True, help="path to the head's .nam file (pinned amp, rig-level)")
    parser.add_argument("--ir", required=True, help="path to the cab IR .wav file (pinned cab, rig-level)")
    parser.add_argument("--boost-db", type=float, default=8.0, help="gain_db for the 'Boost' switchable block")
    parser.add_argument("--delay-ms", type=float, default=300.0)
    parser.add_argument("--delay-feedback", type=float, default=0.3)
    parser.add_argument("--delay-mix", type=float, default=0.25)
    parser.add_argument("--no-select", dest="select", action="store_false", default=True)
    args = parser.parse_args()

    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
