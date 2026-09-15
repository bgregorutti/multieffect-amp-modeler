#!/usr/bin/env python3
"""Dev tool: build a rig where the amp *channel* itself is preset-switchable
-- several .nam captures of the same physical head (e.g. Clean/Crunch/OD1/
OD2), each its own unpinned "nam" chain block, with each preset enabling
exactly one of them. Cab/input gain/output volume/tone stack stay pinned
as usual (see scripts/build_guitar_rig.py for that baseline shape).

This only works because audio-engine's resource_manager.cpp treats "nam"
as an ordinary chain position gated by `enabled` (same mechanism used for
gain/delay/eq), not a rig-wide singleton -- see audio-engine's
EngineChain::namModel and ResourceManager::loadPreset. A rig-side guard
there now rejects a preset with more than one *enabled* "nam" block at
load time (a real use-after-free otherwise: namModel is a single
shared_ptr field and processOrder holds a raw pointer into it, so a
second enabled "nam" block would silently free the first one's object out
from under an already-pushed pointer) -- this script always builds
mutually-exclusive presets, one enabled channel each, so that guard should
never actually fire in normal use; it exists as a safety net, not
something this script works around.

Two phases, same shape as build_guitar_rig.py:
  1. upload -- register the cab IR and every channel's .nam file.
  2. chaining -- build the rig's chain once (pinned backline + one
     unpinned "nam" block per channel), then create one preset per
     channel, each with an explicit enabled/disabled state for every
     channel block (never relies on omission meaning "off").

Usage:
    control-daemon/.venv/bin/python3 scripts/build_amp_channels_rig.py \\
      --rig "Guitar Rig Channels" \\
      --ir "/path/to/cab.wav" \\
      --channel "Clean=/path/to/Clean_Amp.nam" \\
      --channel "Crunch=/path/to/CRUNCH_Amp.nam" \\
      --channel "OD1=/path/to/OD1_Amp.nam" \\
      --channel "OD2=/path/to/OD2_Amp.nam"
"""

from __future__ import annotations

import argparse
import asyncio
import json
import re
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import websockets


def upload_asset(base_url: str, kind: str, path: Path) -> dict:
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


def slugify(name: str) -> str:
    return "channel-" + re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


class ChannelArg(argparse.Action):
    """Parses repeated --channel NAME=PATH into an ordered list of (name, path)."""

    def __call__(self, parser, namespace, values, option_string=None):
        if "=" not in values:
            raise argparse.ArgumentError(self, f"expected NAME=PATH, got {values!r}")
        name, path = values.split("=", 1)
        items = getattr(namespace, self.dest) or []
        items.append((name, path))
        setattr(namespace, self.dest, items)


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
        await ws.send(json.dumps({"type": "hello", "role": "app", "client_name": "build-amp-channels-rig"}))
        snapshot = json.loads(await ws.recv())
        if snapshot["type"] != "state_snapshot":
            raise RuntimeError(f"expected state_snapshot, got {snapshot}")
        latest_state = snapshot["state"]

        # ---- Phase 1: upload ----------------------------------------------
        ir_meta = upload_asset(base_url, "ir", Path(args.ir))
        ir_asset_id = await register(ws, "ir", ir_meta)
        print(f"[build-amp-channels-rig] cab asset {ir_asset_id} ({ir_meta['filename']})")

        channels = []  # (display_name, block_id, asset_id)
        for name, nam_path in args.channel:
            meta = upload_asset(base_url, "nam", Path(nam_path))
            asset_id = await register(ws, "nam", meta)
            block_id = slugify(name)
            channels.append((name, block_id, asset_id))
            print(f"[build-amp-channels-rig] channel {name!r} asset {asset_id} ({meta['filename']})")

        # ---- Phase 2: chaining ---------------------------------------------
        rigs = latest_state["rigs"]
        rig = next((r for r in rigs if r["name"] == args.rig), None)
        if rig is None:
            rig = (await command(ws, {"type": "create_rig", "name": args.rig}))["rig"]
            print(f"[build-amp-channels-rig] created rig {rig['id']} ({args.rig!r})")

        chain = list(rig["chain"])

        def replace_block(block_id: str, **updates) -> None:
            for i, block in enumerate(chain):
                if block["id"] == block_id:
                    chain[i] = {**block, **updates}
                    return
            raise KeyError(f"rig {args.rig!r} has no block id {block_id!r} to update")

        replace_block("cab", asset_id=ir_asset_id)

        # The default skeleton's single pinned "amp" placeholder doesn't fit
        # a multi-channel rig -- drop it and put one unpinned "nam" block
        # per channel in its place instead, same chain position (right
        # after input gain).
        amp_index = next((i for i, b in enumerate(chain) if b["id"] == "amp"), None)
        existing_channel_ids = {block_id for _, block_id, _ in channels}
        chain = [b for b in chain if b["id"] != "amp" and not (b["id"] in existing_channel_ids)]
        insert_at = amp_index if amp_index is not None else 0
        for offset, (name, block_id, asset_id) in enumerate(channels):
            chain.insert(insert_at + offset, {
                "id": block_id,
                "type": "nam",
                "asset_id": asset_id,
                "pinned": False,
                "enabled": True,
                "params": {},
            })

        rig = (await command(ws, {"type": "update_rig", "rig_id": rig["id"], "chain": chain}))["rig"]
        channel_ids = " / ".join(block_id for _, block_id, _ in channels)
        print(f"[build-amp-channels-rig] rig {args.rig!r} channels: [{channel_ids}]")

        first_preset_index = None
        for name, this_block_id, _ in channels:
            # Every preset states every channel block explicitly (exactly
            # one True) -- never relies on an id being absent to mean "off"
            # (see scripts/build_guitar_rig.py's module docstring for why).
            block_states = {
                block_id: {"enabled": block_id == this_block_id, "params": {}}
                for _, block_id, _ in channels
            }
            existing = next((p for p in rig["presets"] if p["name"] == name), None)
            if existing is None:
                preset = (await command(ws, {
                    "type": "create_preset", "rig_id": rig["id"], "name": name, "block_states": block_states,
                }))["preset"]
                print(f"[build-amp-channels-rig] created preset {preset['name']!r} (channel {this_block_id!r} on)")
            else:
                preset = (await command(ws, {
                    "type": "update_preset", "rig_id": rig["id"], "preset_id": existing["id"],
                    "block_states": block_states,
                }))["preset"]
                print(f"[build-amp-channels-rig] updated preset {preset['name']!r} (channel {this_block_id!r} on)")

            rig_index = next(i for i, r in enumerate(latest_state["rigs"]) if r["id"] == rig["id"])
            preset_index = next(
                i for i, p in enumerate(latest_state["rigs"][rig_index]["presets"]) if p["id"] == preset["id"]
            )
            if first_preset_index is None:
                first_preset_index = (rig_index, preset_index)

        if args.select and first_preset_index is not None:
            rig_index, preset_index = first_preset_index
            await command(ws, {"type": "select_preset", "rig_index": rig_index, "preset_index": preset_index})
            print(f"[build-amp-channels-rig] selected rig {args.rig!r} / preset {channels[0][0]!r}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--rig", default="Guitar Rig Channels")
    parser.add_argument("--ir", required=True, help="path to the cab IR .wav file (pinned cab, rig-level)")
    parser.add_argument(
        "--channel", action=ChannelArg, dest="channel", default=[], metavar="NAME=PATH", required=True,
        help="one amp channel: a display name and its .nam file, e.g. Crunch=/path/to/CRUNCH_Amp.nam "
             "(repeat for each channel; order sets preset order)",
    )
    parser.add_argument("--no-select", dest="select", action="store_false", default=True)
    args = parser.parse_args()

    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
