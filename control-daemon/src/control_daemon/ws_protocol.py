"""WebSocket message type definitions for the control daemon's client<->server
protocol.

See the top-level README for the full protocol writeup with example JSON
payloads. In short:

* Every connection's first message must be ``hello`` declaring a role:
  ``"app"`` (mobile app -- full read/write), ``"footswitch"`` (sends only
  ``footswitch_press`` events), or ``"display"`` (read-only, sends nothing
  but receives broadcasts).
* App-only *commands* mutate state (create/update/delete preset, banks,
  bypass, footswitch mapping, asset registration). Sending one from a
  non-"app" role gets a typed ``error`` with code ``"role_forbidden"``.
* ``footswitch_press`` is footswitch-only.
* Every successful command gets a direct ``command_ok`` reply to the sender,
  and every state mutation is separately broadcast to *all* connected
  clients as ``state_changed`` (including mutations triggered by a
  footswitch press), so every UI stays in sync.
"""

from __future__ import annotations

from typing import Dict, List, Literal, Optional

from pydantic import BaseModel, Field

from .models import EffectBlock, FootswitchAction

ClientRole = Literal["app", "footswitch", "display"]


# --------------------------------------------------------------------------
# Client -> server
# --------------------------------------------------------------------------


class HelloMessage(BaseModel):
    """Must be the first message sent on every connection."""

    type: Literal["hello"] = "hello"
    role: ClientRole
    client_name: Optional[str] = None


class CreatePresetMessage(BaseModel):
    type: Literal["create_preset"] = "create_preset"
    name: str
    blocks: List[EffectBlock] = Field(default_factory=list)
    nam_asset_id: Optional[str] = None
    ir_asset_id: Optional[str] = None


class UpdatePresetMessage(BaseModel):
    type: Literal["update_preset"] = "update_preset"
    preset_id: str
    name: Optional[str] = None
    blocks: Optional[List[EffectBlock]] = None
    nam_asset_id: Optional[str] = None
    ir_asset_id: Optional[str] = None


class DeletePresetMessage(BaseModel):
    type: Literal["delete_preset"] = "delete_preset"
    preset_id: str


class SelectPresetMessage(BaseModel):
    """Select by ``preset_id`` OR by ``(bank_index, slot)`` -- not both."""

    type: Literal["select_preset"] = "select_preset"
    preset_id: Optional[str] = None
    bank_index: Optional[int] = None
    slot: Optional[int] = None


class CreateBankMessage(BaseModel):
    type: Literal["create_bank"] = "create_bank"
    name: str
    num_slots: int = 4


class UpdateBankMessage(BaseModel):
    type: Literal["update_bank"] = "update_bank"
    bank_id: str
    name: Optional[str] = None
    slots: Optional[List[Optional[str]]] = None


class ReorderBanksMessage(BaseModel):
    type: Literal["reorder_banks"] = "reorder_banks"
    bank_ids: List[str]


class SetBypassMessage(BaseModel):
    type: Literal["set_bypass"] = "set_bypass"
    bypass: bool


class SetFootswitchMappingMessage(BaseModel):
    """Replaces the *entire* footswitch mapping (not a merge)."""

    type: Literal["set_footswitch_mapping"] = "set_footswitch_mapping"
    mapping: Dict[int, FootswitchAction]


class RegisterAssetMessage(BaseModel):
    """Registers metadata for a .nam/IR file already uploaded via the plain
    HTTP ``POST /assets/upload`` endpoint (see app.py / README for why
    binary upload deliberately does not go over this WS protocol)."""

    type: Literal["register_asset"] = "register_asset"
    kind: Literal["nam", "ir"]
    filename: str
    stored_path: str
    size_bytes: int = 0
    sha256: Optional[str] = None


class FootswitchPressMessage(BaseModel):
    """Sent by a footswitch-role client for a single logical (already
    debounced, if it came through the in-process GPIO path) button press,
    identified by switch index."""

    type: Literal["footswitch_press"] = "footswitch_press"
    switch_index: int


# type string -> pydantic model class, for parsing incoming messages.
APP_ONLY_MESSAGES = {
    "create_preset": CreatePresetMessage,
    "update_preset": UpdatePresetMessage,
    "delete_preset": DeletePresetMessage,
    "select_preset": SelectPresetMessage,
    "create_bank": CreateBankMessage,
    "update_bank": UpdateBankMessage,
    "reorder_banks": ReorderBanksMessage,
    "set_bypass": SetBypassMessage,
    "set_footswitch_mapping": SetFootswitchMappingMessage,
    "register_asset": RegisterAssetMessage,
}

FOOTSWITCH_ONLY_MESSAGES = {
    "footswitch_press": FootswitchPressMessage,
}

MESSAGE_MODELS = {**APP_ONLY_MESSAGES, **FOOTSWITCH_ONLY_MESSAGES}


# --------------------------------------------------------------------------
# Server -> client
# --------------------------------------------------------------------------


class StateSnapshotMessage(BaseModel):
    """Sent once, right after a successful ``hello``."""

    type: Literal["state_snapshot"] = "state_snapshot"
    state: dict


class StateChangedMessage(BaseModel):
    """Broadcast to *every* connected client (app, footswitch, display)
    whenever the daemon's state changes, for any reason -- including a
    footswitch press or another app instance's edit."""

    type: Literal["state_changed"] = "state_changed"
    state: dict
    reason: str


class CommandOkMessage(BaseModel):
    """Direct reply to the sender of a successfully-applied command."""

    type: Literal["command_ok"] = "command_ok"
    command: str
    result: dict = Field(default_factory=dict)


class ErrorMessage(BaseModel):
    type: Literal["error"] = "error"
    code: str
    message: str
    in_reply_to: Optional[str] = None
