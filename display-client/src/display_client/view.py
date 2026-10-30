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

# Component image (``icons/<name>.png``) per block type. Types without one
# keep the text chip.
BLOCK_ICONS = {
    "nam": "amp",
    "ir": "cab",
    "tone_stack": "eq",
    "eq": "eq",
    "delay": "dly",
    "tube_screamer": "drv",
    "big_muff": "fuzz",
    "noise_gate": "nr"
}

# Left out of the strip: their value is shown as a number instead, which
# says more on a small screen than one more picture.
INPUT_GAIN_TYPE = "gain"
VOLUME_TYPE = "volume"
HIDDEN_IN_CHAIN = {INPUT_GAIN_TYPE, VOLUME_TYPE}
# Both are a plain gain stage; the engine's default when the param is unset.
LEVEL_PARAM = "gain_db"
LEVEL_DEFAULT_DB = 0.0


@dataclass(frozen=True)
class Chip:
    label: str
    enabled: bool
    icon: Optional[str] = None


@dataclass(frozen=True)
class LiveView:
    rig_name: str = ""
    rig_number: int = 0  # 1-based, 0 when there are no rigs
    rig_count: int = 0
    preset_name: str = ""
    preset_number: int = 0
    preset_count: int = 0
    chain: List[Chip] = field(default_factory=list)
    amp_name: Optional[str] = None
    cab_name: Optional[str] = None
    input_gain_db: Optional[float] = None
    volume_db: Optional[float] = None
    bypass: bool = False
    engine_error: Optional[str] = None

    @property
    def empty(self) -> bool:
        return self.rig_count == 0


def block_label(block_type: str) -> str:
    return BLOCK_LABELS.get(block_type, block_type[:4].upper() or "?")


def asset_name(assets: dict, asset_id: Optional[str]) -> str:
    asset = assets.get(asset_id) if asset_id else None
    if not asset:
        return "–"
    return asset.get("display_name") or asset.get("filename") or "?"


def level_db(params: dict) -> float:
    try:
        return float(params.get(LEVEL_PARAM, LEVEL_DEFAULT_DB))
    except (TypeError, ValueError):
        return LEVEL_DEFAULT_DB


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

    assets = state.get("assets") or {}

    chain = []
    amp_name = cab_name = None
    input_gain_db = volume_db = None
    for block in rig.get("chain") or []:
        block_type = block.get("type", "")
        override = overrides.get(block.get("id"))
        if block.get("pinned"):
            enabled = True
        elif override is not None:
            enabled = bool(override.get("enabled", True))
        else:
            enabled = bool(block.get("enabled", True))
        params = dict(block.get("params") or {})
        params.update((override or {}).get("params") or {})

        # First of each wins, which is the one the strip shows first too.
        if block_type == "nam" and amp_name is None:
            amp_name = asset_name(assets, block.get("asset_id"))
        elif block_type == "ir" and cab_name is None:
            cab_name = asset_name(assets, block.get("asset_id"))
        elif block_type == INPUT_GAIN_TYPE and input_gain_db is None:
            input_gain_db = level_db(params)
        elif block_type == VOLUME_TYPE and volume_db is None:
            volume_db = level_db(params)

        if block_type not in HIDDEN_IN_CHAIN:
            chain.append(Chip(block_label(block_type), enabled, BLOCK_ICONS.get(block_type)))

    return LiveView(
        rig_name=rig.get("name", ""),
        rig_number=rig_index + 1,
        rig_count=len(rigs),
        preset_name=preset.get("name", "") if preset else "",
        preset_number=preset_index + 1 if preset else 0,
        preset_count=len(presets),
        chain=chain,
        amp_name=amp_name,
        cab_name=cab_name,
        input_gain_db=input_gain_db,
        volume_db=volume_db,
        bypass=bypass,
        engine_error=engine_error,
    )
