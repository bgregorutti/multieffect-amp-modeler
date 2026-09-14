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
import urllib.parse
import urllib.request
from pathlib import Path

import websockets


def upload_asset(base_url: str, kind: str, path: Path) -> dict:
    data = path.read_bytes()
    # Real NAM/IR packs routinely have spaces (and other reserved
    # characters) in their filenames -- urlencode, don't just interpolate.
    query = urllib.parse.urlencode({"kind": kind, "filename": path.name})
    url = f"{base_url}/assets/upload?{query}"
    req = urllib.request.Request(url, data=data, method="POST")
    with urllib.request.urlopen(req) as resp:
        return json.loads(resp.read())


async def register(ws: websockets.WebSocketClientProtocol, kind: str, meta: dict) -> str:
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

    async with websockets.connect(ws_uri) as ws:
        await ws.send(json.dumps({"type": "hello", "role": "app", "client_name": "load-test-preset"}))
        snapshot = json.loads(await ws.recv())
        if snapshot["type"] != "state_snapshot":
            raise RuntimeError(f"expected state_snapshot, got {snapshot}")
        state = snapshot["state"]

        nam_asset_id = None
        if args.nam:
            meta = upload_asset(base_url, "nam", Path(args.nam))
            nam_asset_id = await register(ws, "nam", meta)
            print(f"[load-test-preset] registered NAM asset {nam_asset_id} ({meta['filename']})")

        ir_asset_id = None
        if args.ir:
            meta = upload_asset(base_url, "ir", Path(args.ir))
            ir_asset_id = await register(ws, "ir", meta)
            print(f"[load-test-preset] registered IR asset {ir_asset_id} ({meta['filename']})")

        blocks = []
        if args.gain_db != 0.0:
            blocks.append({"type": "gain", "enabled": True, "params": {"gain_db": args.gain_db}})
        if args.delay:
            blocks.append(
                {
                    "type": "delay",
                    "enabled": True,
                    "params": {
                        "delay_ms": args.delay_ms,
                        "feedback": args.delay_feedback,
                        "mix": args.delay_mix,
                    },
                }
            )

        await ws.send(
            json.dumps(
                {
                    "type": "create_preset",
                    "name": args.name,
                    "blocks": blocks,
                    "nam_asset_id": nam_asset_id,
                    "ir_asset_id": ir_asset_id,
                }
            )
        )
        reply = json.loads(await ws.recv())
        if reply["type"] != "command_ok":
            raise RuntimeError(f"create_preset failed: {reply}")
        await ws.recv()
        preset = reply["result"]["preset"]
        print(f"[load-test-preset] created preset {preset['id']} ({preset['name']!r}) with {len(blocks)} block(s)")

        bank = next((b for b in state["banks"] if b["name"] == args.bank), None)
        if bank is None:
            await ws.send(json.dumps({"type": "create_bank", "name": args.bank, "num_slots": 4}))
            reply = json.loads(await ws.recv())
            if reply["type"] != "command_ok":
                raise RuntimeError(f"create_bank failed: {reply}")
            await ws.recv()
            bank = reply["result"]["bank"]
            print(f"[load-test-preset] created bank {bank['id']} ({args.bank!r})")

        slot = next((i for i, pid in enumerate(bank["slots"]) if pid is None), None)
        if slot is None:
            raise RuntimeError(f"bank {args.bank!r} has no free slots -- pass --bank to use/create another one")

        new_slots = list(bank["slots"])
        new_slots[slot] = preset["id"]
        await ws.send(json.dumps({"type": "update_bank", "bank_id": bank["id"], "slots": new_slots}))
        reply = json.loads(await ws.recv())
        if reply["type"] != "command_ok":
            raise RuntimeError(f"update_bank failed: {reply}")
        await ws.recv()
        print(f"[load-test-preset] assigned {preset['name']!r} to bank {args.bank!r} slot {slot}")

        if args.select:
            await ws.send(json.dumps({"type": "select_preset", "preset_id": preset["id"]}))
            reply = json.loads(await ws.recv())
            if reply["type"] != "command_ok":
                raise RuntimeError(f"select_preset failed: {reply}")
            await ws.recv()
            print(f"[load-test-preset] selected {preset['name']!r} as the active preset")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--name", required=True)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--bank", default="Test Bank")
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

