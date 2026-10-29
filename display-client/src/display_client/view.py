"""Daemon state -> what the live screen shows.

Reads the same ``state_view()`` dict the daemon sends every client
(``state_snapshot`` / ``state_changed``), and resolves block enablement the
same way the daemon's ``resolve_preset`` does, so the chain strip shows
what the engine is actually playing.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional

# Short chip labels for the signal-chain strip. Unknown types fall back to
# their first letters, so a new engine block still shows up.
BLOCK_LABELS = {
    "gain": "GAIN",
    "nam": "AMP",
    "ir": "CAB",
    "tone_stack": "TONE",
    "eq": "EQ",
    "volume": "VOL",
    "delay": "DLY",
    "big_muff": "MUFF",
    "tube_screamer": "TS",
    "noise_gate": "GATE",
    "passthrough": "THRU",
    "vst3": "VST",
}


@dataclass(frozen=True)
class Chip:
    label: str
    enabled: bool


@dataclass(frozen=True)
class LiveView:
    rig_name: str = ""
    rig_number: int = 0  # 1-based, 0 when there are no rigs
    rig_count: int = 0
    preset_name: str = ""
    preset_number: int = 0
    preset_count: int = 0
    chain: List[Chip] = field(default_factory=list)
    bypass: bool = False
    engine_error: Optional[str] = None

    @property
    def empty(self) -> bool:
        return self.rig_count == 0


def block_label(block_type: str) -> str:
    return BLOCK_LABELS.get(block_type, block_type[:4].upper() or "?")


def live_view(state: dict) -> LiveView:
    rigs = state.get("rigs") or []
    bypass = bool(state.get("bypass"))
    engine_error = state.get("engine_error") or None
    rig_index = state.get("active_rig_index", 0)
    if not (0 <= rig_index < len(rigs)):
        return LiveView(rig_count=len(rigs), bypass=bypass, engine_error=engine_error)

    rig = rigs[rig_index]
    presets = rig.get("presets") or []
    preset_index = state.get("active_preset_index", 0)
    preset = presets[preset_index] if 0 <= preset_index < len(presets) else None
    overrides = (preset or {}).get("block_states") or {}

    chain = []
    for block in rig.get("chain") or []:
        override = overrides.get(block.get("id"))
        if block.get("pinned"):
            enabled = True
        elif override is not None:
            enabled = bool(override.get("enabled", True))
        else:
            enabled = bool(block.get("enabled", True))
        chain.append(Chip(block_label(block.get("type", "")), enabled))

    return LiveView(
        rig_name=rig.get("name", ""),
        rig_number=rig_index + 1,
        rig_count=len(rigs),
        preset_name=preset.get("name", "") if preset else "",
        preset_number=preset_index + 1 if preset else 0,
        preset_count=len(presets),
        chain=chain,
        bypass=bypass,
        engine_error=engine_error,
    )
