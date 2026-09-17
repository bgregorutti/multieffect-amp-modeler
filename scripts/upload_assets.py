#!/usr/bin/env python3
"""Dev tool: upload/register .nam/.wav/.vst3 files against a running
control-daemon and stop there -- no rig or preset created. Use the mobile
(or web-build) app afterwards to build rigs/presets from whatever this
registers; see mobile-app/README.md's "Live parameter controls" and
RigChainEditorScreen's asset picker.

kind is decided per-file:
  --nam / --ir  : a single .nam / cabinet-IR .wav file, uploaded via HTTP
                  POST /assets/upload (streamed, content-deduped by
                  sha256 -- re-running this script, or pointing it at a
                  file you've already registered, is expected, not an
                  error) then registered over WS.
  --vst3        : a .vst3 *bundle directory* already sitting on this
                  machine's disk -- no HTTP upload at all (a directory
                  doesn't fit that single-file endpoint), registered
                  directly by path instead. See audio-engine/README.md
                  "VST3 plugin hosting: Discovery is a filesystem scan,
                  not a phone upload". Deduped here by stored_path
                  (checked against already-registered assets) since a
                  bundle carries no single content checksum to hash.
  --dir         : recursively scans a folder and classifies by extension
                  (*.nam -> nam, *.wav -> ir, *.vst3/ -> vst3, not
                  descended into). Convenience for a whole NAM/IR pack at
                  once -- every .wav under --dir is assumed to be a
                  cabinet IR, so point it at a folder that's actually
                  that (not a mixed folder of unrelated .wav files).

Usage:
    control-daemon/.venv/bin/python3 scripts/upload_assets.py \\
      --nam "/path/to/Head.nam" --ir "/path/to/cab.wav"

    control-daemon/.venv/bin/python3 scripts/upload_assets.py \\
      --vst3 "/path/to/Some Plugin.vst3"

    control-daemon/.venv/bin/python3 scripts/upload_assets.py \\
      --dir "/Users/you/Documents/Musique/VST-NAM"
"""

from __future__ import annotations

import argparse
import asyncio
import platform
import subprocess
import json
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import websockets


def upload_file_asset(base_url: str, kind: str, path: Path) -> dict:
    """nam/ir only -- streams the file to the daemon's upload endpoint.

    Returns upload metadata, or ``{"existing_asset_id": ...}`` when the
    daemon already has these exact bytes (content-addressed dedup)."""
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


async def register_file_asset(ws: websockets.WebSocketClientProtocol, kind: str, meta: dict) -> tuple[str, bool]:
    """Returns (asset_id, was_newly_registered)."""
    if "existing_asset_id" in meta:
        return meta["existing_asset_id"], False
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
        raise AssetRegistrationFailed(
            Path(meta["filename"]), str(reply.get("message", reply))
        )
    await ws.recv()  # matching state_changed broadcast
    return reply["result"]["asset"]["id"], True


class AssetRegistrationFailed(RuntimeError):
    """One asset could not be registered.

    Raised per job rather than aborting the run: a folder of plugins where
    one is unloadable should still register the rest.
    """

    def __init__(self, path: Path, detail: str, hint: str | None = None) -> None:
        super().__init__(detail)
        self.path = path
        self.detail = detail
        self.hint = hint


def architecture_hint(bundle_path: Path, message: str) -> str | None:
    """Turn dlopen's "incompatible architecture" wall of text into one line.

    A VST3 is a native binary, so it has to match the *engine's* architecture
    exactly -- there is no translation layer inside an already-running arm64
    process. An Intel-only plugin on an Apple Silicon machine simply cannot be
    loaded, which is a property of the plugin, not a bug to fix here.
    """
    if "incompatible architecture" not in message:
        return None
    host = platform.machine()
    binaries = sorted((bundle_path / "Contents" / "MacOS").glob("*"))
    archs = "unknown"
    if binaries:
        try:
            archs = subprocess.run(
                ["lipo", "-archs", str(binaries[0])],
                capture_output=True, text=True, timeout=5,
            ).stdout.strip() or "unknown"
        except (OSError, subprocess.SubprocessError):
            pass
    return (
        f"plugin is {archs}, this machine and the audio engine are {host}. "
        f"A VST3 is loaded into the engine's own process, so the architectures "
        f"must match -- Rosetta cannot help here. Nothing to fix: use a "
        f"universal or {host} build of this plugin, or leave it out."
    )


async def register_vst3_asset(
    ws: websockets.WebSocketClientProtocol, bundle_path: Path, already_registered_by_path: dict
) -> tuple[str, bool]:
    """No HTTP upload -- a .vst3 bundle is registered by its own on-disk
    path (see module docstring). Deduped here by stored_path rather than
    content hash."""
    resolved = str(bundle_path.resolve())
    if resolved in already_registered_by_path:
        return already_registered_by_path[resolved], False
    await ws.send(json.dumps({
        "type": "register_asset",
        "kind": "vst3",
        "filename": bundle_path.name,
        "stored_path": resolved,
    }))
    reply = json.loads(await ws.recv())
    if reply["type"] != "command_ok":
        message = str(reply.get("message", reply))
        raise AssetRegistrationFailed(
            bundle_path, message, architecture_hint(bundle_path, message)
        )
    await ws.recv()  # matching state_changed broadcast
    asset_id = reply["result"]["asset"]["id"]
    params = reply["result"]["asset"].get("parameters") or []
    if params:
        print(f"    parameters: {', '.join(p['label'] for p in params)}")
    return asset_id, True


def collect_from_dir(root: Path) -> list[tuple[str, Path]]:
    """Recursively classifies files under root; does not descend into a
    *.vst3 bundle looking for more assets inside it."""
    found: list[tuple[str, Path]] = []
    for path in sorted(root.rglob("*")):
        # Skip anything already inside a .vst3 bundle we've already listed.
        if any(parent.suffix.lower() == ".vst3" for parent in path.parents):
            continue
        if path.is_dir() and path.suffix.lower() == ".vst3":
            found.append(("vst3", path))
        elif path.is_file() and path.suffix.lower() == ".nam":
            found.append(("nam", path))
        elif path.is_file() and path.suffix.lower() == ".wav":
            found.append(("ir", path))
    return found


async def main_async(args: argparse.Namespace) -> None:
    base_url = f"http://{args.host}:{args.port}"
    ws_uri = f"ws://{args.host}:{args.port}/ws"

    jobs: list[tuple[str, Path]] = []
    jobs += [("nam", Path(p)) for p in args.nam]
    jobs += [("ir", Path(p)) for p in args.ir]
    jobs += [("vst3", Path(p)) for p in args.vst3]
    for d in args.dir:
        jobs += collect_from_dir(Path(d))

    if not jobs:
        raise SystemExit("nothing to upload -- pass --nam/--ir/--vst3/--dir")

    async with websockets.connect(ws_uri) as ws:
        await ws.send(json.dumps({"type": "hello", "role": "app", "client_name": "upload-assets"}))
        snapshot = json.loads(await ws.recv())
        if snapshot["type"] != "state_snapshot":
            raise RuntimeError(f"expected state_snapshot, got {snapshot}")
        already_registered_by_path = {
            a["stored_path"]: a["id"] for a in snapshot["state"]["assets"].values() if a["kind"] == "vst3"
        }

        results = []
        failures: list[AssetRegistrationFailed] = []
        for kind, path in jobs:
            print(f"[upload-assets] {kind:5} {path}")
            try:
                if kind == "vst3":
                    asset_id, is_new = await register_vst3_asset(ws, path, already_registered_by_path)
                    if is_new:
                        already_registered_by_path[str(path.resolve())] = asset_id
                else:
                    meta = upload_file_asset(base_url, kind, path)
                    asset_id, is_new = await register_file_asset(ws, kind, meta)
            except AssetRegistrationFailed as failure:
                # One unusable asset must not cost you the rest of the folder.
                print(f"    -> SKIPPED: {failure.hint or failure.detail}")
                failures.append(failure)
                continue
            status = "registered" if is_new else "reused (already known)"
            print(f"    -> asset {asset_id} ({status})")
            results.append((kind, path.name, asset_id, status))

        print()
        print(f"[upload-assets] done -- {len(results)} asset(s) available to the app:")
        for kind, filename, asset_id, status in results:
            print(f"  {kind:5} {asset_id}  {filename}  [{status}]")

        if failures:
            print()
            print(f"[upload-assets] {len(failures)} asset(s) skipped:")
            for failure in failures:
                print(f"  {failure.path.name}")
                print(f"      {failure.hint or failure.detail}")
            raise SystemExit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--nam", action="append", default=[], metavar="PATH", help="a .nam file (repeatable)")
    parser.add_argument("--ir", action="append", default=[], metavar="PATH", help="a cabinet IR .wav file (repeatable)")
    parser.add_argument("--vst3", action="append", default=[], metavar="PATH", help="a .vst3 bundle directory (repeatable)")
    parser.add_argument("--dir", action="append", default=[], metavar="PATH",
                         help="recursively scan a folder for .nam/.wav/.vst3 (repeatable)")
    args = parser.parse_args()

    asyncio.run(main_async(args))


if __name__ == "__main__":
    main()
