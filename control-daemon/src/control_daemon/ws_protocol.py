"""WebSocket message type definitions for the control daemon's client<->server
protocol.

See the top-level README for the full protocol writeup with example JSON
payloads. In short:

* Every connection's first message must be ``hello`` declaring a role:
  ``"app"`` (mobile app -- full read/write), ``"footswitch"`` (sends only
  ``footswitch_press`` events), or ``"display"`` (read-only, sends nothing
  but receives broadcasts).
* App-only *commands* mutate state (create/update/delete rigs and the
  presets inside them, bypass, footswitch mapping, asset registration and
  renaming). Sending one from a non-"app" role gets a typed ``error`` with
  code ``"role_forbidden"``.
* ``footswitch_press`` is footswitch-only.
* Every successful command gets a direct ``command_ok`` reply to the sender,
  and every state mutation is separately broadcast to *all* connected
  clients as ``state_changed`` (including mutations triggered by a
  footswitch press), so every UI stays in sync.
"""

from __future__ import annotations

from typing import Dict, List, Literal, Optional

from pydantic import BaseModel, Field

from .models import EffectBlock, FootswitchAction, PresetBlockState

ClientRole = Literal["app", "footswitch", "display"]


# --------------------------------------------------------------------------
# Client -> server
# --------------------------------------------------------------------------


class HelloMessage(BaseModel):
    """Must be the first message sent on every connection."""

    type: Literal["hello"] = "hello"
    role: ClientRole
    client_name: Optional[str] = None


class CreateRigMessage(BaseModel):
    """Omitting ``chain`` seeds the standard gain/amp/cab/tone-stack/volume
    skeleton (see ``models.default_rig_chain``); sending an explicit ``[]``
    is honored as a deliberate empty rig instead."""

    type: Literal["create_rig"] = "create_rig"
    name: str
    chain: Optional[List[EffectBlock]] = None


class UpdateRigMessage(BaseModel):
    """Any field left unset is left unchanged. Sending ``chain`` replaces the
    whole chain, so the app is expected to send the full desired order."""

    type: Literal["update_rig"] = "update_rig"
    rig_id: str
    name: Optional[str] = None
    chain: Optional[List[EffectBlock]] = None


class DeleteRigMessage(BaseModel):
    type: Literal["delete_rig"] = "delete_rig"
    rig_id: str


class ReorderRigsMessage(BaseModel):
    type: Literal["reorder_rigs"] = "reorder_rigs"
    rig_ids: List[str]


class CreatePresetMessage(BaseModel):
    type: Literal["create_preset"] = "create_preset"
    rig_id: str
    name: str
    block_states: Dict[str, PresetBlockState] = Field(default_factory=dict)


class UpdatePresetMessage(BaseModel):
    type: Literal["update_preset"] = "update_preset"
    rig_id: str
    preset_id: str
    name: Optional[str] = None
    block_states: Optional[Dict[str, PresetBlockState]] = None


class DeletePresetMessage(BaseModel):
    type: Literal["delete_preset"] = "delete_preset"
    rig_id: str
    preset_id: str


class SelectPresetMessage(BaseModel):
    """Move the active position. Either index may be sent alone; omitting
    ``rig_index`` selects within the current rig."""

    type: Literal["select_preset"] = "select_preset"
    rig_index: Optional[int] = None
    preset_index: Optional[int] = None


class SetBypassMessage(BaseModel):
    type: Literal["set_bypass"] = "set_bypass"
    bypass: bool


class SetFootswitchMappingMessage(BaseModel):
    """Replaces the *entire* footswitch mapping (not a merge)."""

    type: Literal["set_footswitch_mapping"] = "set_footswitch_mapping"
    mapping: Dict[int, FootswitchAction]


class RegisterAssetMessage(BaseModel):
    """Registers metadata for a .nam/IR file already uploaded via the plain
    HTTP ``POST /assets/upload`` endpoint, or a ``.vst3`` plugin bundle
    already installed out of band (see app.py / README for why binary
    upload deliberately does not go over this WS protocol, and why a
    ``.vst3`` bundle -- a directory -- doesn't fit that upload endpoint at
    all)."""

    type: Literal["register_asset"] = "register_asset"
    kind: Literal["nam", "ir", "vst3"]
    filename: str
    stored_path: str
    size_bytes: int = 0
    sha256: Optional[str] = None
    # Optional user-facing label (e.g. "Crunch lampes vintage") for the
    # mobile picker UI. Omitting it defaults the stored asset's
    # ``display_name`` to ``filename`` (see
    # ``DaemonStateManager.register_asset``) -- it is never left ``None``
    # in stored state.
    display_name: Optional[str] = None


class RenameAssetMessage(BaseModel):
    """Change an already-registered asset's user-facing ``display_name``
    (e.g. after the initial ``register_asset`` default of ``filename``
    isn't the label the player actually wants). Mirrors the shape of other
    simple single-field mutations like ``set_bypass``."""

    type: Literal["rename_asset"] = "rename_asset"
    asset_id: str
    display_name: str


class SetBlockParamMessage(BaseModel):
    """Live, no-reload parameter tweak on one block within one preset.

    Persists into that preset's ``block_states[block_id].params`` override
    (see ``PresetBlockState``) -- not the rig block's own default -- and is
    forwarded to the engine only when this rig/preset is the one actually
    playing (see ``DaemonStateManager.set_block_param``).
    """

    type: Literal["set_block_param"] = "set_block_param"
    rig_id: str
    preset_id: str
    block_id: str
    param_key: str
    value: float


class ListBlockTypesMessage(BaseModel):
    """Static parameter schema for every native block type this engine
    build recognizes. Static per engine build, not per-rig state -- a
    client fetches this once (e.g. on connect), not per preset."""

    type: Literal["list_block_types"] = "list_block_types"


class FootswitchPressMessage(BaseModel):
    """Sent by a footswitch-role client for a single logical (already
    debounced, if it came through the in-process GPIO path) button press,
    identified by switch index."""

    type: Literal["footswitch_press"] = "footswitch_press"
    switch_index: int


# type string -> pydantic model class, for parsing incoming messages.
APP_ONLY_MESSAGES = {
    "create_rig": CreateRigMessage,
    "update_rig": UpdateRigMessage,
    "delete_rig": DeleteRigMessage,
    "reorder_rigs": ReorderRigsMessage,
    "create_preset": CreatePresetMessage,
    "update_preset": UpdatePresetMessage,
    "delete_preset": DeletePresetMessage,
    "select_preset": SelectPresetMessage,
    "set_bypass": SetBypassMessage,
    "set_footswitch_mapping": SetFootswitchMappingMessage,
    "register_asset": RegisterAssetMessage,
    "rename_asset": RenameAssetMessage,
    "set_block_param": SetBlockParamMessage,
    "list_block_types": ListBlockTypesMessage,
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
