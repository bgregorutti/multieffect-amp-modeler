"""DaemonState manager: the daemon's single source of truth.

``DaemonStateManager`` owns the in-memory ``DaemonState``, persists it to
disk (atomically, via persistence.py) after every mutation, drives the
``AudioEngineClient`` on every change that should affect audio, and applies
footswitch actions according to the configured mapping.

This module is deliberately independent of asyncio/websockets: it exposes
plain, synchronous methods. The WS layer (app.py) is responsible for role
checks and for translating a successful mutation into a broadcast to
connected clients, via the ``on_change`` callback hook.

The rig/preset split (see models.py) lands here as two navigation paths:
``_change_rig`` swaps the whole backline and reloads the engine's chain,
while ``_change_preset`` only flips which of the current rig's blocks are
enabled. Both end in the same place -- pushing a ``ResolvedPreset`` to the
engine -- but only the former implies reloading a NAM model or an IR.
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Callable, Dict, List, Optional

from .audio_engine_client import AudioEngineClient
from .models import (
    Asset,
    AssetKind,
    DaemonState,
    EffectBlock,
    FootswitchAction,
    NextPresetAction,
    NextRigAction,
    Preset,
    PresetBlockState,
    PrevPresetAction,
    PrevRigAction,
    ResolvedPreset,
    Rig,
    SelectPresetAction,
    TapTempoAction,
    ToggleBypassAction,
    default_rig_chain,
    resolve_preset,
)
from .persistence import load_state, save_state

OnChange = Callable[[str], None]


class StateError(Exception):
    """A well-typed error raised by a state mutation.

    ``code`` is a short machine-readable string (e.g. "not_found",
    "validation_error") that the WS layer forwards verbatim in its error
    message so clients can branch on it without string-matching English
    prose.
    """

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code
        self.message = message


class DaemonStateManager:
    def __init__(
        self,
        store_path: Path,
        audio_engine: Optional[AudioEngineClient] = None,
        on_change: Optional[OnChange] = None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self.store_path = store_path
        self.audio_engine: AudioEngineClient = audio_engine or _default_engine()
        self.on_change: Optional[OnChange] = on_change
        self._clock = clock
        self._tap_times: List[float] = []

        self.state: DaemonState = load_state(store_path)

    # -- persistence / notification plumbing --------------------------------

    def _persist(self) -> None:
        save_state(self.store_path, self.state)

    def _notify(self, reason: str, persist: bool = True) -> None:
        if persist:
            self._persist()
        if self.on_change is not None:
            self.on_change(reason)

    def state_view(self) -> dict:
        """A JSON-serializable snapshot of the full daemon state.

        ``active_rig_id``/``active_preset_id`` are derived rather than
        stored: clients (the app, the onboard display) want to highlight
        what is live without re-deriving it from two indices, but indices
        remain the authoritative position.
        """
        view = self.state.model_dump(mode="json")
        rig = self.active_rig()
        preset = self.active_preset()
        view["active_rig_id"] = rig.id if rig is not None else None
        view["active_preset_id"] = preset.id if preset is not None else None
        return view

    # -- active position ----------------------------------------------------

    def active_rig(self) -> Optional[Rig]:
        if not (0 <= self.state.active_rig_index < len(self.state.rigs)):
            return None
        return self.state.rigs[self.state.active_rig_index]

    def active_preset(self) -> Optional[Preset]:
        rig = self.active_rig()
        if rig is None:
            return None
        if not (0 <= self.state.active_preset_index < len(rig.presets)):
            return None
        return rig.presets[self.state.active_preset_index]

    def resolved_active(self) -> Optional[ResolvedPreset]:
        rig = self.active_rig()
        preset = self.active_preset()
        if rig is None or preset is None:
            return None
        return resolve_preset(rig, preset)

    def _load_active(self) -> None:
        """Push the currently-active resolved chain to the engine.

        A no-op when nothing is selected -- an empty rig list or a rig with
        no presets is a normal first-run state, not an error.
        """
        resolved = self.resolved_active()
        if resolved is not None:
            self.audio_engine.load_preset(resolved)

    def _clamp_active(self) -> None:
        """Keep the active indices inside the current rig/preset lists.

        Called after any structural edit (delete, reorder) so the active
        position can never dangle past the end of a shortened list.
        """
        if not self.state.rigs:
            self.state.active_rig_index = 0
            self.state.active_preset_index = 0
            return

        self.state.active_rig_index = max(
            0, min(self.state.active_rig_index, len(self.state.rigs) - 1)
        )
        rig = self.state.rigs[self.state.active_rig_index]
        if not rig.presets:
            self.state.active_preset_index = 0
        else:
            self.state.active_preset_index = max(
                0, min(self.state.active_preset_index, len(rig.presets) - 1)
            )

    # -- rigs ---------------------------------------------------------------

    def create_rig(
        self,
        name: str,
        chain: Optional[List[EffectBlock]] = None,
    ) -> Rig:
        """Create a rig. Omitting ``chain`` entirely (as opposed to passing
        an explicit ``[]``) seeds the standard gain -> amp -> cab -> tone
        stack -> volume skeleton (see ``models.default_rig_chain``) rather
        than an empty one, since that shape is what every rig with a NAM
        capture wants -- an explicit ``[]`` is respected as a deliberate
        choice (e.g. a rig with no amp at all), not overridden."""
        resolved_chain = default_rig_chain() if chain is None else list(chain)
        for block in resolved_chain:
            self._validate_asset_ref(block.asset_id)
        rig = Rig(name=name, chain=resolved_chain)
        self.state.rigs.append(rig)
        self._notify("create_rig")
        return rig

    def update_rig(
        self,
        rig_id: str,
        name: Optional[str] = None,
        chain: Optional[List[EffectBlock]] = None,
    ) -> Rig:
        rig = self._get_rig(rig_id)
        if name is not None:
            rig.name = name
        if chain is not None:
            for block in chain:
                self._validate_asset_ref(block.asset_id)
            rig.chain = list(chain)
            # Presets may reference blocks this edit removed. Drop those
            # entries rather than leaving them to accumulate as silent
            # cruft that resolves to nothing.
            live_block_ids = {b.id for b in rig.chain}
            for preset in rig.presets:
                preset.block_states = {
                    bid: st
                    for bid, st in preset.block_states.items()
                    if bid in live_block_ids
                }

        if rig is self.active_rig():
            self._load_active()

        self._notify("update_rig")
        return rig

    def delete_rig(self, rig_id: str) -> None:
        rig = self._get_rig(rig_id)
        was_active = rig is self.active_rig()
        self.state.rigs = [r for r in self.state.rigs if r.id != rig_id]
        self._clamp_active()
        if was_active:
            self._load_active()
        self._notify("delete_rig")

    def reorder_rigs(self, rig_ids: List[str]) -> None:
        existing_ids = {r.id for r in self.state.rigs}
        if set(rig_ids) != existing_ids or len(rig_ids) != len(self.state.rigs):
            raise StateError(
                "validation_error",
                "reorder_rigs must list every existing rig id exactly once",
            )
        # Follow the active rig to its new position rather than letting the
        # index point at whatever rig happens to land there.
        active = self.active_rig()
        by_id = {r.id: r for r in self.state.rigs}
        self.state.rigs = [by_id[rig_id] for rig_id in rig_ids]
        if active is not None:
            self.state.active_rig_index = rig_ids.index(active.id)
        self._clamp_active()
        self._notify("reorder_rigs")

    # -- presets (within a rig) ---------------------------------------------

    def create_preset(
        self,
        rig_id: str,
        name: str,
        block_states: Optional[Dict[str, PresetBlockState]] = None,
    ) -> Preset:
        rig = self._get_rig(rig_id)
        self._validate_block_states(rig, block_states)
        preset = Preset(name=name, block_states=dict(block_states or {}))
        rig.presets.append(preset)
        self._notify("create_preset")
        return preset

    def update_preset(
        self,
        rig_id: str,
        preset_id: str,
        name: Optional[str] = None,
        block_states: Optional[Dict[str, PresetBlockState]] = None,
    ) -> Preset:
        rig = self._get_rig(rig_id)
        preset = self._get_preset(rig, preset_id)

        if name is not None:
            preset.name = name
        if block_states is not None:
            self._validate_block_states(rig, block_states)
            preset.block_states = dict(block_states)
        preset.updated_at = time.time()

        if preset is self.active_preset():
            # The live preset changed shape -- push it again so what is
            # playing matches what is stored.
            self._load_active()

        self._notify("update_preset")
        return preset

    def delete_preset(self, rig_id: str, preset_id: str) -> None:
        rig = self._get_rig(rig_id)
        preset = self._get_preset(rig, preset_id)
        was_active = preset is self.active_preset()
        rig.presets = [p for p in rig.presets if p.id != preset_id]
        self._clamp_active()
        if was_active:
            self._load_active()
        self._notify("delete_preset")

    def select_preset(
        self,
        rig_index: Optional[int] = None,
        preset_index: Optional[int] = None,
    ) -> None:
        """Move the active position. Either index may be given alone;
        omitting ``rig_index`` selects within the current rig."""
        if rig_index is None and preset_index is None:
            raise StateError(
                "validation_error",
                "select_preset requires rig_index, preset_index, or both",
            )

        target_rig_index = (
            self.state.active_rig_index if rig_index is None else rig_index
        )
        if not (0 <= target_rig_index < len(self.state.rigs)):
            raise StateError(
                "validation_error",
                f"rig_index {target_rig_index} out of range "
                f"(have {len(self.state.rigs)} rigs)",
            )
        rig = self.state.rigs[target_rig_index]

        if rig.presets:
            target_preset_index = (
                self.state.active_preset_index if preset_index is None else preset_index
            )
            if not (0 <= target_preset_index < len(rig.presets)):
                raise StateError(
                    "validation_error",
                    f"preset_index {target_preset_index} out of range for rig "
                    f"{rig.name!r} (has {len(rig.presets)} presets)",
                )
        else:
            # A rig with no presets yet (e.g. freshly created) has nothing
            # to select -- not a client error, the same "no active preset"
            # state active_preset()/_load_active() already tolerate
            # gracefully for the footswitch rig-stepping path (_change_rig).
            # A specific preset_index request against an empty rig is
            # inherently unsatisfiable, so it's ignored here rather than
            # rejected -- this is what lets the app's "tap a rig to make it
            # active" (RigListScreen, always sends preset_index=0) work on a
            # rig that doesn't have a preset yet either.
            target_preset_index = 0

        self.state.active_rig_index = target_rig_index
        self.state.active_preset_index = target_preset_index
        self._load_active()
        self._notify("select_preset")

    # -- bypass / footswitch mapping ---------------------------------------

    def set_bypass(self, bypass: bool) -> None:
        self.state.bypass = bypass
        self.audio_engine.set_bypass(bypass)
        self._notify("set_bypass")

    def set_footswitch_mapping(self, mapping: Dict[int, FootswitchAction]) -> None:
        # A full replace, not a merge -- the app is expected to send its
        # complete desired mapping each time (it already holds the full
        # picture; the daemon is not in the business of diffing UI intent).
        self.state.footswitch_mapping = dict(mapping)
        self._notify("set_footswitch_mapping")

    # -- assets -------------------------------------------------------------

    def find_asset_by_sha256(self, sha256: Optional[str]) -> Optional[Asset]:
        """Return the already-registered asset with this checksum, if any.

        Content-addressed rather than filename-addressed on purpose: the same
        IR routinely arrives under different names, and two different IRs
        routinely arrive under the same name.
        """
        if sha256 is None:
            return None
        for asset in self.state.assets.values():
            if asset.sha256 == sha256:
                return asset
        return None

    def register_asset(
        self,
        kind: AssetKind,
        filename: str,
        stored_path: str,
        size_bytes: int = 0,
        sha256: Optional[str] = None,
        display_name: Optional[str] = None,
    ) -> Asset:
        existing = self.find_asset_by_sha256(sha256)
        if existing is not None:
            raise StateError(
                "duplicate_asset",
                f"asset with sha256 {sha256} already registered as "
                f"{existing.id!r} ({existing.filename!r})",
            )
        asset = Asset(
            kind=kind,
            filename=filename,
            stored_path=stored_path,
            size_bytes=size_bytes,
            sha256=sha256,
            # An omitted display_name defaults to the raw filename so the
            # field is never actually null in stored state -- only
            # optional on the wire (see RegisterAssetMessage) -- the
            # mobile picker UI always has *something* human-readable to
            # show, even before the player bothers to rename it.
            display_name=display_name if display_name is not None else filename,
        )
        # The engine resolves block asset ids off its own copy of this
        # registry (it never reads the daemon's state directly), so a
        # newly-registered asset must be pushed there before any chain
        # referencing it is loaded -- otherwise a real AudioEngineClient's
        # load_preset() fails with "unknown asset id" for every fresh
        # upload. See audio_engine_client.py. For a "vst3" asset, the
        # engine's reply also carries the plugin's own parameter schema
        # (a throwaway instantiation purely to introspect it) -- captured
        # onto the Asset record here so it's already in every
        # state_snapshot/state_changed broadcast, no second round-trip
        # needed once a rig actually uses the plugin.
        asset.parameters = self.audio_engine.register_asset(asset)
        self.state.assets[asset.id] = asset
        self._notify("register_asset")
        return asset

    def rename_asset(self, asset_id: str, display_name: str) -> Asset:
        """Change an already-registered asset's user-facing label.

        Purely a metadata edit -- the file on disk, its checksum and its
        engine registration are untouched, so this never talks to
        ``self.audio_engine``.
        """
        asset = self._get_asset(asset_id)
        asset.display_name = display_name
        self._notify("rename_asset")
        return asset

    def set_block_param(
        self,
        rig_id: str,
        preset_id: str,
        block_id: str,
        param_key: str,
        value: float,
    ) -> Preset:
        """Live, no-reload parameter tweak: persists into *this preset's*
        ``block_states[block_id].params`` override -- not the rig block's
        own default (that stays an ``update_rig`` concern) -- so the same
        plugin/block can sit at different settings per preset within one
        rig, the same way recalling a DAW plugin's per-scene state would.
        Forwarded to the engine only when this rig/preset is the one
        actually playing; never triggers a full ``load_preset``.
        """
        rig = self._get_rig(rig_id)
        preset = self._get_preset(rig, preset_id)
        if block_id not in {b.id for b in rig.chain}:
            raise StateError(
                "validation_error",
                f"no block with id {block_id!r} in rig {rig.id!r}",
            )

        block_state = preset.block_states.setdefault(block_id, PresetBlockState())
        block_state.params[param_key] = value
        preset.updated_at = time.time()

        if preset is self.active_preset():
            self.audio_engine.set_block_param(block_id, param_key, value)

        self._notify("set_block_param")
        return preset

    def list_block_types(self) -> List[dict]:
        """Static per-engine-build parameter schema for every native block
        type -- a pure query, proxied straight to the engine (see
        audio_engine_client.py); never mutates state or notifies."""
        return self.audio_engine.list_block_types()

    # -- footswitch actions ---------------------------------------------------

    def apply_footswitch_press(self, switch_index: int) -> None:
        """Apply whatever action is mapped to ``switch_index``.

        A press on an unmapped switch is a silent no-op (no state change,
        no broadcast) -- an unconfigured footswitch input is not an error.
        """
        action = self.state.footswitch_mapping.get(switch_index)
        if action is None:
            return

        if isinstance(action, NextRigAction):
            self._change_rig(+1)
        elif isinstance(action, PrevRigAction):
            self._change_rig(-1)
        elif isinstance(action, NextPresetAction):
            self._change_preset(+1)
        elif isinstance(action, PrevPresetAction):
            self._change_preset(-1)
        elif isinstance(action, SelectPresetAction):
            self.select_preset(preset_index=action.index)
        elif isinstance(action, ToggleBypassAction):
            self.set_bypass(not self.state.bypass)
        elif isinstance(action, TapTempoAction):
            self._tap_tempo()

    def _change_rig(self, delta: int) -> None:
        """Step to the next/previous rig, wrapping around.

        This is the expensive path: a different rig means a different amp
        and cab, so the engine reloads its NAM model and IR.
        """
        if not self.state.rigs:
            return
        self.state.active_rig_index = (
            self.state.active_rig_index + delta
        ) % len(self.state.rigs)
        # Landing on a rig with fewer presets must not leave the preset
        # index dangling past the end.
        self._clamp_active()
        self._load_active()
        self._notify("footswitch_next_rig" if delta > 0 else "footswitch_prev_rig")

    def _change_preset(self, delta: int) -> None:
        """Step within the current rig's presets, wrapping around.

        Never crosses into another rig: preset block ids are only meaningful
        against their own rig's chain, and a switch that silently swapped
        the amp mid-song would be a bug.
        """
        rig = self.active_rig()
        if rig is None or not rig.presets:
            return
        self.state.active_preset_index = (
            self.state.active_preset_index + delta
        ) % len(rig.presets)
        self._load_active()
        self._notify(
            "footswitch_next_preset" if delta > 0 else "footswitch_prev_preset"
        )

    def _tap_tempo(self) -> None:
        now = self._clock()
        # Taps more than 3s apart start a fresh tap sequence rather than
        # being averaged in with a stale one.
        if self._tap_times and (now - self._tap_times[-1]) > 3.0:
            self._tap_times.clear()
        self._tap_times.append(now)
        self._tap_times = self._tap_times[-4:]

        if len(self._tap_times) >= 2:
            intervals = [
                b - a for a, b in zip(self._tap_times, self._tap_times[1:])
            ]
            avg_interval = sum(intervals) / len(intervals)
            if avg_interval > 0:
                bpm = round(60.0 / avg_interval, 1)
                self.state.tempo_bpm = bpm
                self.audio_engine.set_tempo(bpm)

        # Tap-tempo taps are transient performance state, not durable
        # configuration -- broadcast so the display/app can show the live
        # BPM, but don't wear out the storage medium persisting every tap.
        self._notify("tap_tempo", persist=False)

    # -- lookups / validation -------------------------------------------------

    def _get_rig(self, rig_id: str) -> Rig:
        for rig in self.state.rigs:
            if rig.id == rig_id:
                return rig
        raise StateError("not_found", f"no rig with id {rig_id!r}")

    def _get_preset(self, rig: Rig, preset_id: str) -> Preset:
        for preset in rig.presets:
            if preset.id == preset_id:
                return preset
        raise StateError(
            "not_found", f"no preset with id {preset_id!r} in rig {rig.id!r}"
        )

    def _get_asset(self, asset_id: str) -> Asset:
        asset = self.state.assets.get(asset_id)
        if asset is None:
            raise StateError("not_found", f"no asset with id {asset_id!r}")
        return asset

    def _validate_block_states(
        self, rig: Rig, block_states: Optional[Dict[str, PresetBlockState]]
    ) -> None:
        if not block_states:
            return
        chain_ids = {b.id for b in rig.chain}
        unknown = set(block_states) - chain_ids
        if unknown:
            raise StateError(
                "validation_error",
                f"block_states references blocks not in rig {rig.id!r}: "
                f"{sorted(unknown)}",
            )

    def _validate_asset_ref(self, asset_id: Optional[str]) -> None:
        """Assets must exist, but a block's ``type`` is not checked against
        the asset's ``kind``: block types are opaque engine-defined strings
        (see models.py) and this daemon deliberately keeps no catalog of
        which types imply which asset kind. Existence is the invariant that
        actually matters -- the engine hard-errors on an unknown asset id.
        """
        if asset_id is None:
            return
        if asset_id not in self.state.assets:
            raise StateError("not_found", f"no asset with id {asset_id!r}")


def _default_engine() -> AudioEngineClient:
    from .audio_engine_client import NullAudioEngineClient

    return NullAudioEngineClient()
