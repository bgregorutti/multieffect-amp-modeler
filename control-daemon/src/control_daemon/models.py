"""Pydantic models for the control daemon's state.

These describe the *entire* persisted + in-memory state of the daemon: the
preset library, banks (ordered groupings of presets into footswitch-sized
pages), registered .nam/IR asset metadata, and the footswitch-to-action
mapping.

Versioned schema
-----------------
The on-disk JSON store format is a deliberately simple, versioned schema
(``DaemonState.version``). The product spec flags "preset serialization
format" as an open question; we pick plain JSON (via pydantic) because it is
human-inspectable, diffable, trivial to back up/restore, and easy for the
(not-yet-built) mobile app to also parse if it ever wants to read a preset
bundle directly. When the shape of this schema changes in a backwards
incompatible way, bump ``SCHEMA_VERSION`` and add a migration step in
``persistence.load_state``.

Asset bytes vs. metadata
------------------------
Only *metadata* about uploaded .nam/IR files lives here (id, filename, path,
size, checksum). The daemon deliberately never holds decoded/parsed asset
bytes in memory -- the real audio engine (JUCE host) owns loading and
decoding those files from disk. This keeps the daemon a lightweight
background process suitable for running alongside the real-time audio
engine on a resource constrained Raspberry Pi.
"""

from __future__ import annotations

import time
import uuid
from enum import Enum
from typing import Annotated, Dict, List, Literal, Optional, Union

from pydantic import BaseModel, Field

SCHEMA_VERSION = 1


def _new_id() -> str:
    return uuid.uuid4().hex[:12]


def _now() -> float:
    return time.time()


class EffectBlock(BaseModel):
    """One block in a preset's signal chain.

    The daemon treats a block's ``params`` as opaque, engine-defined data --
    it stores and forwards them but has no knowledge of individual effect
    DSP internals. The audio engine (JUCE plugin host) owns interpreting
    ``type``/``params``.
    """

    type: str
    enabled: bool = True
    params: Dict[str, Union[float, int, str, bool]] = Field(default_factory=dict)


class AssetKind(str, Enum):
    NAM = "nam"
    IR = "ir"


class Asset(BaseModel):
    """Registry metadata for one uploaded ``.nam`` model or IR file.

    The binary itself is written to disk by the HTTP upload endpoint
    (streamed, never buffered whole in memory -- see app.py); this model
    only ever holds metadata about it.
    """

    id: str = Field(default_factory=_new_id)
    kind: AssetKind
    filename: str
    stored_path: str
    size_bytes: int = 0
    sha256: Optional[str] = None
    uploaded_at: float = Field(default_factory=_now)


class Preset(BaseModel):
    id: str = Field(default_factory=_new_id)
    name: str
    blocks: List[EffectBlock] = Field(default_factory=list)
    nam_asset_id: Optional[str] = None
    ir_asset_id: Optional[str] = None
    created_at: float = Field(default_factory=_now)
    updated_at: float = Field(default_factory=_now)


class Bank(BaseModel):
    """An ordered grouping of presets into footswitch-sized pages.

    ``slots`` is an ordered, fixed-length-ish list; each entry is either a
    preset id or ``None`` for an empty slot. Bank order in
    ``DaemonState.banks`` is itself the user-visible bank order (see
    ``reorder_banks``).
    """

    id: str = Field(default_factory=_new_id)
    name: str
    slots: List[Optional[str]] = Field(default_factory=lambda: [None] * 4)


# --------------------------------------------------------------------------
# Footswitch mapping: what a physical footswitch press *does*.
# --------------------------------------------------------------------------


class SelectSlotAction(BaseModel):
    type: Literal["select_slot"] = "select_slot"
    slot: int


class NextBankAction(BaseModel):
    type: Literal["next_bank"] = "next_bank"


class PrevBankAction(BaseModel):
    type: Literal["prev_bank"] = "prev_bank"


class ToggleBypassAction(BaseModel):
    type: Literal["toggle_bypass"] = "toggle_bypass"


class NextPresetAction(BaseModel):
    """Steps to the next non-empty slot, scanning across banks in order
    (bank order, then slot order within a bank) and wrapping around. Unlike
    ``NextBankAction`` (which keeps the same slot index and can land on an
    empty slot), this always lands on an assigned preset if one exists
    anywhere -- useful for a minimal two-switch footswitch that just wants
    to browse the whole preset list without per-slot buttons."""

    type: Literal["next_preset"] = "next_preset"


class PrevPresetAction(BaseModel):
    type: Literal["prev_preset"] = "prev_preset"


class TapTempoAction(BaseModel):
    """Placeholder action: the spec calls for tap-tempo support, but tempo
    is not yet wired to any real effect. The daemon still tracks tap
    intervals and computes a BPM estimate so the display/app can show
    something, and so the AudioEngineClient hook is exercised end to end."""

    type: Literal["tap_tempo"] = "tap_tempo"


FootswitchAction = Annotated[
    Union[
        SelectSlotAction,
        NextBankAction,
        PrevBankAction,
        ToggleBypassAction,
        NextPresetAction,
        PrevPresetAction,
        TapTempoAction,
    ],
    Field(discriminator="type"),
]


class DaemonState(BaseModel):
    """The full persisted + in-memory state of the daemon.

    ``version`` is the on-disk schema version -- see the module docstring.
    """

    version: int = SCHEMA_VERSION

    presets: Dict[str, Preset] = Field(default_factory=dict)
    banks: List[Bank] = Field(default_factory=list)
    assets: Dict[str, Asset] = Field(default_factory=dict)
    footswitch_mapping: Dict[int, FootswitchAction] = Field(default_factory=dict)

    active_bank_index: int = 0
    active_slot: int = 0
    active_preset_id: Optional[str] = None
    bypass: bool = False
    tempo_bpm: Optional[float] = None
