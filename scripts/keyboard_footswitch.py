#!/usr/bin/env python3
"""Keyboard-as-footswitch dev tool -- for testing preset switching before a
real footswitch/GPIO relay exists (see ARCHITECTURE.md's "no hardware
required" testing philosophy, and control-daemon/README.md's "Two
footswitch-input ingress paths": this script is a stand-in for the second
one, a footswitch relay process sending already-logical presses over the
daemon's WS API -- it is not special-cased in the daemon at all).

Right arrow = next preset, left arrow = previous preset. Down arrow =
toggle bypass (no simulation at all -- dry signal only, useful for A/B
testing whatever's currently loaded). These map to switch indices 0, 1,
and 2, bound to the daemon's next_preset/prev_preset/toggle_bypass
footswitch actions (control-daemon/README.md); next_preset/prev_preset
step through every assigned preset slot across all banks, skipping empty
ones and wrapping around.

On startup this script briefly connects as role="app" to merge switches 0,
1, and 2 into whatever footswitch_mapping is already configured -- set_
footswitch_mapping is a full replace on the wire, so this reads the
current mapping first rather than clobbering any other switches you've
already set up -- then reconnects as role="footswitch" and listens for
arrow keys.

Usage:
    control-daemon/.venv/bin/python3 scripts/keyboard_footswitch.py [--host HOST] [--port PORT]

(Needs the `websockets` package -- control-daemon's own venv already has
it as a runtime dependency, hence running this with that interpreter
rather than requiring a separate install.)

Requires the terminal window running this script to have focus (keys are
read from stdin in cbreak mode) -- this is a dev/test stand-in, not a
permanent replacement for a physical footswitch.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
import termios
import tty

import websockets

NEXT_SWITCH = 0
PREV_SWITCH = 1
BYPASS_SWITCH = 2


async def ensure_mapping(uri: str) -> None:
    async with websockets.connect(uri) as ws:
        await ws.send(json.dumps({"type": "hello", "role": "app", "client_name": "keyboard-footswitch"}))
        snapshot = json.loads(await ws.recv())
        if snapshot["type"] != "state_snapshot":
            raise RuntimeError(f"expected state_snapshot, got {snapshot}")
        mapping = dict(snapshot["state"]["footswitch_mapping"])

        desired = {
            str(NEXT_SWITCH): {"type": "next_preset"},
            str(PREV_SWITCH): {"type": "prev_preset"},
            str(BYPASS_SWITCH): {"type": "toggle_bypass"},
        }
        if all(mapping.get(k) == v for k, v in desired.items()):
            return  # already configured -- nothing to send

        mapping.update(desired)
        await ws.send(json.dumps({"type": "set_footswitch_mapping", "mapping": mapping}))
        reply = json.loads(await ws.recv())
        if reply["type"] != "command_ok":
            raise RuntimeError(f"failed to configure footswitch mapping: {reply}")
        print(
            f"[keyboard-footswitch] configured switch {NEXT_SWITCH}=next_preset, "
            f"{PREV_SWITCH}=prev_preset, {BYPASS_SWITCH}=toggle_bypass"
        )


def _print_active(state: dict) -> None:
    bypass_suffix = " [BYPASSED -- dry signal only]" if state.get("bypass") else ""
    preset_id = state.get("active_preset_id")
    if preset_id is None:
        print(f"[keyboard-footswitch] active preset: (none){bypass_suffix}")
        return
    preset = state.get("presets", {}).get(preset_id)
    name = preset["name"] if preset else preset_id
    print(f"[keyboard-footswitch] active preset: {name}{bypass_suffix}")


async def run(uri: str) -> None:
    await ensure_mapping(uri)

    async with websockets.connect(uri) as ws:
        await ws.send(json.dumps({"type": "hello", "role": "footswitch", "client_name": "keyboard-footswitch"}))
        snapshot = json.loads(await ws.recv())
        _print_active(snapshot["state"])

        loop = asyncio.get_running_loop()
        key_queue: asyncio.Queue[str] = asyncio.Queue()
        loop.add_reader(sys.stdin.fileno(), lambda: key_queue.put_nowait(sys.stdin.read(1)))

        print(
            "[keyboard-footswitch] ready -- right arrow = next preset, left arrow = prev preset, "
            "down arrow = toggle bypass, q = quit"
        )

        async def read_broadcasts() -> None:
            async for raw in ws:
                msg = json.loads(raw)
                if msg["type"] == "state_changed":
                    _print_active(msg["state"])
                elif msg["type"] == "error":
                    print(f"[keyboard-footswitch] error: {msg['code']}: {msg['message']}")

        broadcast_task = asyncio.create_task(read_broadcasts())
        try:
            while True:
                ch = await key_queue.get()
                if ch == "q":
                    break
                if ch != "\x1b":
                    continue
                # Arrow keys arrive as the 3-byte escape sequence ESC [ C/D/B.
                ch2 = await key_queue.get()
                ch3 = await key_queue.get()
                if (ch2, ch3) == ("[", "C"):
                    await ws.send(json.dumps({"type": "footswitch_press", "switch_index": NEXT_SWITCH}))
                elif (ch2, ch3) == ("[", "D"):
                    await ws.send(json.dumps({"type": "footswitch_press", "switch_index": PREV_SWITCH}))
                elif (ch2, ch3) == ("[", "B"):
                    await ws.send(json.dumps({"type": "footswitch_press", "switch_index": BYPASS_SWITCH}))
        finally:
            loop.remove_reader(sys.stdin.fileno())
            broadcast_task.cancel()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    uri = f"ws://{args.host}:{args.port}/ws"

    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    try:
        tty.setcbreak(fd)
        asyncio.run(run(uri))
    except KeyboardInterrupt:
        pass
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)


if __name__ == "__main__":
    main()
