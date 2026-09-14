"""Pydantic models for the control daemon's state.

These describe the *entire* persisted + in-memory state of the daemon: the
rig library, the presets nested inside each rig, registered .nam/IR asset
metadata, and the footswitch-to-action mapping.

Rigs and presets
----------------
A **rig** is a backline: one ordered signal chain whose amp and cab blocks
are ``pinned`` (always on, not preset-switchable), plus the effects sitting
around them. A **preset** belongs to one rig and says only *which of that
rig's non-pinned blocks are on* (and, eventually, what their parameter
values are). The physical switches map onto exactly those two levels: rig
up/down swaps the whole backline, preset next/prev flips effects underneath
an unchanged amp and cab.

That split is a real-time property, not just a modelling nicety. Changing
preset within a rig never reloads a NAM model or re-partitions an IR --
it only flips per-block enable flags, so it is fast and click-free on the
audio thread. The expensive reloads are confined to rig changes, which
happen between songs rather than mid-riff.

The engine is never told about this structure. It receives a
``ResolvedPreset``: the rig's chain with the active preset's overrides
already applied, i.e. the exact chain to play. All rig/preset resolution
stays here in the daemon.

Versioned schema
-----------------
The on-disk JSON store format is a deliberately simple, versioned schema
(``DaemonState.version``). The product spec flags "preset serialization
format" as an open question; we pick plain JSON (via pydantic) because it is
human-inspectable, diffable, trivial to back up/restore, and easy for the
mobile app to also parse if it ever wants to read a preset bundle directly.
When the shape of this schema changes in a backwards incompatible way, bump
``SCHEMA_VERSION`` and add a migration step in ``persistence.load_state``.

Asset bytes vs. metadata
------------------------
Only *metadata* about uploaded .nam/IR files lives here (id, filename, path,
size, checksum). The daemon deliberately never holds decoded/parsed asset
bytes in memory -- the real audio engine (JUCE host) owns loading and
decoding those files from disk. This keeps the daemon a lightweight
background process suitable for running alongside the real-time audio
engine on a resource constrained Raspberry Pi.

Assets are deduplicated by content checksum, not filename: the same IR
routinely arrives under several names, and unrelated IRs routinely share
one. See ``DaemonStateManager.register_asset``.
"""

from __future__ import annotations

import time
import uuid
from enum import Enum
from typing import Annotated, Dict, List, Literal, Optional, Union

from pydantic import BaseModel, Field

SCHEMA_VERSION = 2

ParamValue = Union[float, int, str, bool]


def _new_id() -> str:
    return uuid.uuid4().hex[:12]


def _now() -> float:
    return time.time()


class EffectBlock(BaseModel):
    """One block in a rig's signal chain.

    The daemon treats ``type``/``params`` as opaque, engine-defined data --
    it stores and forwards them but has no knowledge of individual effect
    DSP internals. The audio engine (JUCE plugin host) owns interpreting
    them. There is deliberately no hardcoded catalog of known effect types
    anywhere in this codebase.

    ``asset_id`` is how a block references the library: an amp block points
    at a ``.nam`` asset, a cab block at an IR. Keeping the reference *on the
    block* (rather than as a pair of special fields on the preset) is what
    makes chain order explicit, allows more than one IR in a chain, and
    leaves room for a future VST3 block without a third special case.

    ``pinned`` marks a block as part of the rig's fixed backline: always on,
    never toggled by a preset. Amp and cab are the usual pinned blocks.
    """

    id: str = Field(default_factory=_new_id)
    type: str
    asset_id: Optional[str] = None
    pinned: bool = False
    enabled: bool = True
    params: Dict[str, ParamValue] = Field(default_factory=dict)


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


class PresetBlockState(BaseModel):
    """One preset's override of one of its rig's blocks.

    ``params`` is an override map, not a full copy: keys absent here fall
    back to the block's own values when resolved. Storing per-preset
    parameter values is supported by the schema but not yet driven by the
    UI -- gain staging and preamp values are deliberately out of scope for
    now.
    """

    enabled: bool = True
    params: Dict[str, ParamValue] = Field(default_factory=dict)


class Preset(BaseModel):
    """A set of on/off (and eventually parameter) overrides within one rig.

    ``block_states`` is keyed by ``EffectBlock.id``. A block with no entry
    keeps its own default ``enabled``/``params``; pinned blocks ignore any
    entry entirely and are always on.
    """

    id: str = Field(default_factory=_new_id)
    name: str
    block_states: Dict[str, PresetBlockState] = Field(default_factory=dict)
    created_at: float = Field(default_factory=_now)
    updated_at: float = Field(default_factory=_now)


class Rig(BaseModel):
    """One backline: an ordered chain plus the presets that toggle it.

    Rig order in ``DaemonState.rigs`` is the user-visible order that the
    rig up/down switches step through (see ``reorder_rigs``).
    """

    id: str = Field(default_factory=_new_id)
    name: str
    chain: List[EffectBlock] = Field(default_factory=list)
    presets: List[Preset] = Field(default_factory=list)


class ResolvedBlock(BaseModel):
    """A chain block with a preset's overrides already applied."""

    id: str
    type: str
    asset_id: Optional[str] = None
    enabled: bool = True
    params: Dict[str, ParamValue] = Field(default_factory=dict)


class ResolvedPreset(BaseModel):
    """What actually gets sent to the audio engine: the exact chain to play.

    Flattened on purpose -- the engine has no concept of rigs, presets or
    overrides, only an ordered list of blocks with resolved parameters.
    """

    id: str
    name: str
    rig_id: str
    rig_name: str
    blocks: List[ResolvedBlock] = Field(default_factory=list)


def resolve_preset(rig: Rig, preset: Preset) -> ResolvedPreset:
    """Flatten ``rig``'s chain under ``preset``'s overrides.

    Pinned blocks are forced on regardless of what the preset says: losing
    your amp or cab to a mis-saved preset is never a useful outcome.
    """
    blocks: List[ResolvedBlock] = []
    for block in rig.chain:
        override = preset.block_states.get(block.id)
        if block.pinned:
            enabled = True
        elif override is not None:
            enabled = override.enabled
        else:
            enabled = block.enabled

        params = dict(block.params)
        if override is not None:
            params.update(override.params)

        blocks.append(
            ResolvedBlock(
                id=block.id,
                type=block.type,
                asset_id=block.asset_id,
                enabled=enabled,
                params=params,
            )
        )

    return ResolvedPreset(
        id=preset.id,
        name=preset.name,
        rig_id=rig.id,
        rig_name=rig.name,
        blocks=blocks,
    )


# --------------------------------------------------------------------------
# Footswitch mapping: what a physical footswitch press *does*.
# --------------------------------------------------------------------------


class NextRigAction(BaseModel):
    """Step to the next rig, wrapping around. Swaps the whole backline, so
    this is the expensive path (a NAM/IR reload) -- a between-songs move."""

    type: Literal["next_rig"] = "next_rig"


class PrevRigAction(BaseModel):
    type: Literal["prev_rig"] = "prev_rig"


class NextPresetAction(BaseModel):
    """Step to the next preset *within the current rig*, wrapping around.

    Deliberately does not cross rigs: a preset's block ids only mean
    anything against its own rig's chain, and a footswitch that silently
    swapped your amp mid-song would be a bug, not a feature.
    """

    type: Literal["next_preset"] = "next_preset"


class PrevPresetAction(BaseModel):
    type: Literal["prev_preset"] = "prev_preset"


class SelectPresetAction(BaseModel):
    """Jump straight to the Nth preset of the current rig -- for boards with
    enough switches to address presets directly rather than stepping."""

    type: Literal["select_preset"] = "select_preset"
    index: int


class ToggleBypassAction(BaseModel):
    type: Literal["toggle_bypass"] = "toggle_bypass"


class TapTempoAction(BaseModel):
    """Placeholder action: the spec calls for tap-tempo support, but tempo
    is not yet wired to any real effect. The daemon still tracks tap
    intervals and computes a BPM estimate so the display/app can show
    something, and so the AudioEngineClient hook is exercised end to end."""

    type: Literal["tap_tempo"] = "tap_tempo"


FootswitchAction = Annotated[
    Union[
        NextRigAction,
        PrevRigAction,
        NextPresetAction,
        PrevPresetAction,
        SelectPresetAction,
        ToggleBypassAction,
        TapTempoAction,
    ],
    Field(discriminator="type"),
]


class DaemonState(BaseModel):
    """The full persisted + in-memory state of the daemon.

    ``version`` is the on-disk schema version -- see the module docstring.

    The active position is held as (rig index, preset index) rather than a
    bare preset id because presets live inside rigs and two rigs may well
    both have a preset called "Solo".
    """

    version: int = SCHEMA_VERSION

    rigs: List[Rig] = Field(default_factory=list)
    assets: Dict[str, Asset] = Field(default_factory=dict)
    footswitch_mapping: Dict[int, FootswitchAction] = Field(default_factory=dict)

    active_rig_index: int = 0
    active_preset_index: int = 0
    bypass: bool = False
    tempo_bpm: Optional[float] = None
