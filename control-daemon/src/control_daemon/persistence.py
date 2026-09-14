"""Atomic JSON persistence for DaemonState.

The store is a single plain JSON file (see models.py for the schema). Writes
are atomic: we write the full new state to a temp file in the same
directory, fsync it, then ``os.replace`` it over the real path. ``os.replace``
is a single filesystem rename, which POSIX guarantees is atomic -- a reader
(or a crash) can only ever see the fully-old file or the fully-new file,
never a half-written one. This matters because this daemon may run on a
Raspberry Pi where power can be pulled at any time.
"""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path

from .models import SCHEMA_VERSION, DaemonState


def load_state(path: Path) -> DaemonState:
    """Load daemon state from ``path``.

    Returns a fresh, empty default ``DaemonState`` if the file does not
    exist yet (first run on a new device).
    """
    if not path.exists():
        return DaemonState()

    raw = path.read_text(encoding="utf-8")
    data = json.loads(raw)

    on_disk_version = data.get("version")

    if on_disk_version == 1:
        data = _migrate_v1_to_v2(data)
        on_disk_version = data["version"]

    if on_disk_version != SCHEMA_VERSION:
        raise ValueError(
            f"Unsupported preset store schema version {on_disk_version!r} "
            f"(expected {SCHEMA_VERSION}); no migration is defined for it."
        )

    return DaemonState.model_validate(data)


# v1 footswitch actions that no longer exist under v2. Because every v1
# preset becomes its own v2 rig (see _migrate_v1_to_v2), stepping presets in
# v1 is the same gesture as stepping rigs in v2 -- so both of v1's
# preset-stepping and bank-stepping actions collapse onto the rig ones.
_V1_ACTION_RENAMES = {
    "next_bank": "next_rig",
    "prev_bank": "prev_rig",
    "next_preset": "next_rig",
    "prev_preset": "prev_rig",
}


def _migrate_v1_to_v2(data: dict) -> dict:
    """Migrate a v1 store (flat preset library + banks of slots) to v2
    (rigs, each holding its own chain and presets).

    In v1 every preset carried its own amp (``nam_asset_id``) and cab
    (``ir_asset_id``), so a v1 preset is really a whole backline: it maps
    onto a v2 *rig*, not a v2 preset. Each becomes a rig whose chain is the
    amp and cab as pinned blocks followed by that preset's effect blocks,
    plus a single "Default" preset that overrides nothing. That preserves
    every stored sound exactly rather than guessing at a grouping the user
    never expressed.

    Rig order follows bank order then slot order, so the sequence the rig
    switches step through matches the order presets appeared on the old
    banks; presets that were never placed in a bank are appended after.
    """
    v1_presets: dict = data.get("presets", {}) or {}
    v1_banks: list = data.get("banks", []) or []

    ordered_ids: list = []
    for bank in v1_banks:
        for preset_id in bank.get("slots", []) or []:
            if preset_id is not None and preset_id not in ordered_ids:
                ordered_ids.append(preset_id)
    for preset_id in v1_presets:
        if preset_id not in ordered_ids:
            ordered_ids.append(preset_id)

    rigs = []
    for preset_id in ordered_ids:
        preset = v1_presets.get(preset_id)
        if preset is None:
            continue

        chain = []
        if preset.get("nam_asset_id"):
            chain.append(
                {
                    "id": f"{preset_id}-amp",
                    "type": "nam",
                    "asset_id": preset["nam_asset_id"],
                    "pinned": True,
                    "enabled": True,
                    "params": {},
                }
            )
        if preset.get("ir_asset_id"):
            chain.append(
                {
                    "id": f"{preset_id}-cab",
                    "type": "ir",
                    "asset_id": preset["ir_asset_id"],
                    "pinned": True,
                    "enabled": True,
                    "params": {},
                }
            )
        for index, block in enumerate(preset.get("blocks", []) or []):
            chain.append(
                {
                    "id": f"{preset_id}-block-{index}",
                    "type": block.get("type", "unknown"),
                    "asset_id": None,
                    "pinned": False,
                    "enabled": block.get("enabled", True),
                    "params": block.get("params", {}) or {},
                }
            )

        rigs.append(
            {
                "id": preset_id,
                "name": preset.get("name", "Untitled"),
                "chain": chain,
                "presets": [
                    {
                        "id": f"{preset_id}-default",
                        "name": "Default",
                        "block_states": {},
                        "created_at": preset.get("created_at", 0.0),
                        "updated_at": preset.get("updated_at", 0.0),
                    }
                ],
            }
        )

    mapping = {}
    for switch_index, action in (data.get("footswitch_mapping", {}) or {}).items():
        action_type = action.get("type")
        if action_type == "select_slot":
            # A v1 slot addressed a preset inside a bank; under v2 those
            # presets are separate rigs, so there is no faithful target for
            # this to point at. Dropped rather than silently rebound.
            continue
        renamed = _V1_ACTION_RENAMES.get(action_type)
        mapping[switch_index] = {"type": renamed} if renamed else action

    active_rig_index = 0
    active_preset_id = data.get("active_preset_id")
    if active_preset_id in ordered_ids:
        active_rig_index = ordered_ids.index(active_preset_id)

    return {
        "version": 2,
        "rigs": rigs,
        "assets": data.get("assets", {}) or {},
        "footswitch_mapping": mapping,
        "active_rig_index": active_rig_index,
        "active_preset_index": 0,
        "bypass": data.get("bypass", False),
        "tempo_bpm": data.get("tempo_bpm"),
    }


def save_state(path: Path, state: DaemonState) -> None:
    """Atomically persist ``state`` as JSON to ``path``.

    Writes to a temp file in the same directory then renames over the
    target, so a crash mid-write can never leave a corrupt/partial store on
    disk -- readers only ever see the fully old file or the fully new one.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = state.model_dump_json(indent=2)

    fd, tmp_path_str = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent)
    )
    tmp_path = Path(tmp_path_str)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(payload)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_path, path)
    except BaseException:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise
