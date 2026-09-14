#!/usr/bin/env python3
"""Keyboard-as-footswitch dev tool -- for testing preset switching before a
real footswitch/GPIO relay exists (see ARCHITECTURE.md's "no hardware
required" testing philosophy, and control-daemon/README.md's "Two
footswitch-input ingress paths": this script is a stand-in for the second
one, a footswitch relay process sending already-logical presses over the
daemon's WS API -- it is not special-cased in the daemon at all).

This mirrors the four-switch layout the real pedal is planned around, one
switch per arrow key:

    up / down arrow     next / prev **rig**    -- swaps the whole backline
                                                  (amp + cab), so this is
                                                  the expensive path: the
                                                  engine reloads its NAM
                                                  model and IR.
    right / left arrow  next / prev **preset** -- flips which effects are on
                                                  underneath an unchanged
                                                  amp and cab. No asset
                                                  reload, so it is the fast,
                                                  click-free path.
    b                   toggle bypass          -- dry signal only, useful
                                                  for A/B testing whatever
                                                  is currently loaded.

Bypass is on `b` rather than an arrow because all four arrows are now
spoken for; it is a dev convenience anyway, since the final hardware drops
the bypass switch entirely.

These map to switch indices 0-4, bound to the daemon's
next_preset/prev_preset/next_rig/prev_rig/toggle_bypass footswitch actions
(control-daemon/README.md). Preset stepping deliberately stays inside the
current rig and never crosses into another one.

On startup this script briefly connects as role="app" to merge those
switches into whatever footswitch_mapping is already configured -- set_
footswitch_mapping is a full replace on the wire, so this reads the
current mapping first rather than clobbering any other switches you've
already set up -- then reconnects as role="footswitch" and listens for
keys.

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
import os
import sys
import termios
import tty

import websockets

NEXT_PRESET_SWITCH = 0
PREV_PRESET_SWITCH = 1
NEXT_RIG_SWITCH = 2
PREV_RIG_SWITCH = 3
BYPASS_SWITCH = 4


async def ensure_mapping(uri: str) -> None:
    async with websockets.connect(uri) as ws:
        await ws.send(json.dumps({"type": "hello", "role": "app", "client_name": "keyboard-footswitch"}))
        snapshot = json.loads(await ws.recv())
        if snapshot["type"] != "state_snapshot":
            raise RuntimeError(f"expected state_snapshot, got {snapshot}")
        mapping = dict(snapshot["state"]["footswitch_mapping"])

        desired = {
            str(NEXT_PRESET_SWITCH): {"type": "next_preset"},
            str(PREV_PRESET_SWITCH): {"type": "prev_preset"},
            str(NEXT_RIG_SWITCH): {"type": "next_rig"},
            str(PREV_RIG_SWITCH): {"type": "prev_rig"},
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
            f"[keyboard-footswitch] configured switch "
            f"{NEXT_PRESET_SWITCH}=next_preset, {PREV_PRESET_SWITCH}=prev_preset, "
            f"{NEXT_RIG_SWITCH}=next_rig, {PREV_RIG_SWITCH}=prev_rig, "
            f"{BYPASS_SWITCH}=toggle_bypass"
        )


async def _press(ws, switch_index: int) -> None:
    await ws.send(json.dumps({"type": "footswitch_press", "switch_index": switch_index}))


def _print_active(state: dict) -> None:
    bypass_suffix = " [BYPASSED -- dry signal only]" if state.get("bypass") else ""
    rigs = state.get("rigs", [])
    rig_index = state.get("active_rig_index", 0)
    preset_index = state.get("active_preset_index", 0)

    if not (0 <= rig_index < len(rigs)):
        print(f"[keyboard-footswitch] no rig selected{bypass_suffix}")
        return

    rig = rigs[rig_index]
    presets = rig.get("presets", [])
    preset_name = (
        presets[preset_index]["name"]
        if 0 <= preset_index < len(presets)
        else "(no preset)"
    )
    print(
        f"[keyboard-footswitch] rig: {rig.get('name')} "
        f"[{rig_index + 1}/{len(rigs)}]  |  preset: {preset_name} "
        f"[{preset_index + 1}/{len(presets)}]{bypass_suffix}"
    )


async def run(uri: str) -> None:
    await ensure_mapping(uri)

    async with websockets.connect(uri) as ws:
        await ws.send(json.dumps({"type": "hello", "role": "footswitch", "client_name": "keyboard-footswitch"}))
        snapshot = json.loads(await ws.recv())
        _print_active(snapshot["state"])

        loop = asyncio.get_running_loop()
        key_queue: asyncio.Queue[str] = asyncio.Queue()
        stdin_fd = sys.stdin.fileno()
        # os.read, not sys.stdin.read: the latter is a buffered
        # TextIOWrapper that can silently read ahead past the 1 byte we
        # asked for whenever multiple bytes are already waiting (e.g. a
        # fast burst of keypresses, or a full 3-byte arrow-key escape
        # sequence arriving in one chunk). Those extra bytes then sit in
        # Python's own buffer, invisible to the kernel -- so add_reader's
        # callback (which only fires on the fd's kernel-level readiness)
        # stops firing even though unread bytes remain, until the next
        # physical keypress arrives and "unsticks" a stale buffered byte
        # instead of the new one. That desyncs which action a keypress
        # maps to under fast presses. os.read() is a raw, unbuffered
        # syscall wrapper -- it takes only what's asked for from the
        # kernel, so there's never a hidden backlog for add_reader to lose
        # track of.
        loop.add_reader(stdin_fd, lambda: key_queue.put_nowait(os.read(stdin_fd, 1).decode(errors="replace")))

        print(
            "[keyboard-footswitch] ready -- up/down arrow = next/prev rig "
            "(swaps amp + cab), right/left arrow = next/prev preset "
            "(flips effects under the same amp), b = toggle bypass, q = quit"
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
                if ch == "b":
                    # Bypass has no arrow key left now that up/down drive the
                    # rig switches. It is a dev-tool convenience anyway --
                    # the final hardware drops the bypass switch entirely.
                    await _press(ws, BYPASS_SWITCH)
                    continue
                if ch != "\x1b":
                    continue
                # Arrow keys arrive as the 3-byte escape sequence ESC [ C/D/B.
                ch2 = await key_queue.get()
                ch3 = await key_queue.get()
                if (ch2, ch3) == ("[", "C"):
                    await _press(ws, NEXT_PRESET_SWITCH)
                elif (ch2, ch3) == ("[", "D"):
                    await _press(ws, PREV_PRESET_SWITCH)
                elif (ch2, ch3) == ("[", "A"):
                    await _press(ws, NEXT_RIG_SWITCH)
                elif (ch2, ch3) == ("[", "B"):
                    await _press(ws, PREV_RIG_SWITCH)
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
