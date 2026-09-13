"""DaemonState manager: the daemon's single source of truth.

``DaemonStateManager`` owns the in-memory ``DaemonState``, persists it to
disk (atomically, via persistence.py) after every mutation, drives the
``AudioEngineClient`` on every change that should affect audio, and applies
footswitch actions according to the configured mapping.

This module is deliberately independent of asyncio/websockets: it exposes
plain, synchronous methods. The WS layer (app.py) is responsible for role
checks and for translating a successful mutation into a broadcast to
connected clients, via the ``on_change`` callback hook.
"""

from __future__ import annotations

import time
from pathlib import Path
from typing import Callable, Dict, List, Optional

from .audio_engine_client import AudioEngineClient
from .models import (
    Asset,
    AssetKind,
    Bank,
    DaemonState,
    EffectBlock,
    FootswitchAction,
    NextBankAction,
    Preset,
    PrevBankAction,
    SelectSlotAction,
    TapTempoAction,
    ToggleBypassAction,
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
        """A JSON-serializable snapshot of the full daemon state."""
        return self.state.model_dump(mode="json")

    # -- presets --------------------------------------------------------------

    def create_preset(
        self,
        name: str,
        blocks: Optional[List[EffectBlock]] = None,
        nam_asset_id: Optional[str] = None,
        ir_asset_id: Optional[str] = None,
    ) -> Preset:
        self._validate_asset_ref(nam_asset_id, AssetKind.NAM)
        self._validate_asset_ref(ir_asset_id, AssetKind.IR)
        preset = Preset(
            name=name,
            blocks=blocks or [],
            nam_asset_id=nam_asset_id,
            ir_asset_id=ir_asset_id,
        )
        self.state.presets[preset.id] = preset
        self._notify("create_preset")
        return preset

    def update_preset(
        self,
        preset_id: str,
        name: Optional[str] = None,
        blocks: Optional[List[EffectBlock]] = None,
        nam_asset_id: Optional[str] = None,
        ir_asset_id: Optional[str] = None,
    ) -> Preset:
        preset = self._get_preset(preset_id)
        if name is not None:
            preset.name = name
        if blocks is not None:
            preset.blocks = blocks
        if nam_asset_id is not None:
            self._validate_asset_ref(nam_asset_id, AssetKind.NAM)
            preset.nam_asset_id = nam_asset_id
        if ir_asset_id is not None:
            self._validate_asset_ref(ir_asset_id, AssetKind.IR)
            preset.ir_asset_id = ir_asset_id
        preset.updated_at = time.time()

        if preset_id == self.state.active_preset_id:
            # The currently-live preset changed shape -- push it to the
            # engine again so what's playing matches what's stored.
            self.audio_engine.load_preset(preset)

        self._notify("update_preset")
        return preset

    def delete_preset(self, preset_id: str) -> None:
        self._get_preset(preset_id)  # raises not_found if missing
        del self.state.presets[preset_id]

        for bank in self.state.banks:
            bank.slots = [None if pid == preset_id else pid for pid in bank.slots]

        if self.state.active_preset_id == preset_id:
            self.state.active_preset_id = None

        self._notify("delete_preset")

    def select_preset(
        self,
        preset_id: Optional[str] = None,
        bank_index: Optional[int] = None,
        slot: Optional[int] = None,
    ) -> None:
        if preset_id is not None:
            preset = self._get_preset(preset_id)
            self.state.active_preset_id = preset.id
            location = self._find_slot_for_preset(preset_id)
            if location is not None:
                self.state.active_bank_index, self.state.active_slot = location
            self.audio_engine.load_preset(preset)
            self._notify("select_preset")
            return

        if bank_index is not None and slot is not None:
            bank = self._get_bank_by_index(bank_index)
            if not (0 <= slot < len(bank.slots)):
                raise StateError(
                    "validation_error",
                    f"slot {slot} out of range for bank {bank_index!r} "
                    f"(has {len(bank.slots)} slots)",
                )
            self.state.active_bank_index = bank_index
            self.state.active_slot = slot
            selected_preset_id = bank.slots[slot]
            self.state.active_preset_id = selected_preset_id
            if selected_preset_id is not None:
                self.audio_engine.load_preset(self.state.presets[selected_preset_id])
            self._notify("select_preset")
            return

        raise StateError(
            "validation_error",
            "select_preset requires either preset_id or (bank_index and slot)",
        )

    # -- banks ------------------------------------------------------------

    def create_bank(self, name: str, num_slots: int = 4) -> Bank:
        if num_slots < 1:
            raise StateError("validation_error", "num_slots must be >= 1")
        bank = Bank(name=name, slots=[None] * num_slots)
        self.state.banks.append(bank)
        self._notify("create_bank")
        return bank

    def update_bank(
        self,
        bank_id: str,
        name: Optional[str] = None,
        slots: Optional[List[Optional[str]]] = None,
    ) -> Bank:
        bank = self._get_bank(bank_id)
        if name is not None:
            bank.name = name
        if slots is not None:
            for preset_id in slots:
                if preset_id is not None:
                    self._get_preset(preset_id)
            bank.slots = list(slots)
        self._notify("update_bank")
        return bank

    def reorder_banks(self, bank_ids: List[str]) -> None:
        existing_ids = {b.id for b in self.state.banks}
        if set(bank_ids) != existing_ids or len(bank_ids) != len(self.state.banks):
            raise StateError(
                "validation_error",
                "reorder_banks must list every existing bank id exactly once",
            )
        by_id = {b.id: b for b in self.state.banks}
        self.state.banks = [by_id[bank_id] for bank_id in bank_ids]
        self._notify("reorder_banks")

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

    def register_asset(
        self,
        kind: AssetKind,
        filename: str,
        stored_path: str,
        size_bytes: int = 0,
        sha256: Optional[str] = None,
    ) -> Asset:
        asset = Asset(
            kind=kind,
            filename=filename,
            stored_path=stored_path,
            size_bytes=size_bytes,
            sha256=sha256,
        )
        self.state.assets[asset.id] = asset
        self._notify("register_asset")
        return asset

    # -- footswitch actions ---------------------------------------------------

    def apply_footswitch_press(self, switch_index: int) -> None:
        """Apply whatever action is mapped to ``switch_index``.

        A press on an unmapped switch is a silent no-op (no state change,
        no broadcast) -- an unconfigured footswitch input is not an error.
        """
        action = self.state.footswitch_mapping.get(switch_index)
        if action is None:
            return

        if isinstance(action, SelectSlotAction):
            self.select_preset(bank_index=self.state.active_bank_index, slot=action.slot)
        elif isinstance(action, NextBankAction):
            self._change_bank(+1)
        elif isinstance(action, PrevBankAction):
            self._change_bank(-1)
        elif isinstance(action, ToggleBypassAction):
            self.set_bypass(not self.state.bypass)
        elif isinstance(action, TapTempoAction):
            self._tap_tempo()

    def _change_bank(self, delta: int) -> None:
        if not self.state.banks:
            return
        new_index = (self.state.active_bank_index + delta) % len(self.state.banks)
        self.state.active_bank_index = new_index
        bank = self.state.banks[new_index]
        slot = self.state.active_slot
        preset_id = bank.slots[slot] if 0 <= slot < len(bank.slots) else None
        self.state.active_preset_id = preset_id
        if preset_id is not None:
            self.audio_engine.load_preset(self.state.presets[preset_id])
        reason = "footswitch_next_bank" if delta > 0 else "footswitch_prev_bank"
        self._notify(reason)

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

    def _get_preset(self, preset_id: str) -> Preset:
        preset = self.state.presets.get(preset_id)
        if preset is None:
            raise StateError("not_found", f"no preset with id {preset_id!r}")
        return preset

    def _get_bank(self, bank_id: str) -> Bank:
        for bank in self.state.banks:
            if bank.id == bank_id:
                return bank
        raise StateError("not_found", f"no bank with id {bank_id!r}")

    def _get_bank_by_index(self, bank_index: int) -> Bank:
        if not (0 <= bank_index < len(self.state.banks)):
            raise StateError(
                "validation_error",
                f"bank_index {bank_index} out of range (have {len(self.state.banks)} banks)",
            )
        return self.state.banks[bank_index]

    def _find_slot_for_preset(self, preset_id: str) -> Optional[tuple]:
        for bank_index, bank in enumerate(self.state.banks):
            for slot_index, slot_preset_id in enumerate(bank.slots):
                if slot_preset_id == preset_id:
                    return (bank_index, slot_index)
        return None

    def _validate_asset_ref(self, asset_id: Optional[str], kind: AssetKind) -> None:
        if asset_id is None:
            return
        asset = self.state.assets.get(asset_id)
        if asset is None:
            raise StateError("not_found", f"no asset with id {asset_id!r}")
        if asset.kind != kind:
            raise StateError(
                "validation_error",
                f"asset {asset_id!r} is kind {asset.kind!r}, expected {kind!r}",
            )


def _default_engine() -> AudioEngineClient:
    from .audio_engine_client import NullAudioEngineClient

    return NullAudioEngineClient()
