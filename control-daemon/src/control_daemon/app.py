"""FastAPI application wiring: the WebSocket API and the HTTP asset-upload
endpoint, wired to a DaemonStateManager.

Resource footprint
------------------
This daemon is meant to run as a lightweight background process on a
Raspberry Pi *alongside* the real-time JUCE audio engine, which is the thing
that actually needs the machine's RAM/CPU budget. Concretely that means:

* Dependencies are kept to FastAPI + uvicorn + pydantic + websockets. No
  pandas/numpy/ML libraries, no in-memory databases, no heavy ORMs -- the
  JSON file written by persistence.py *is* the persistence layer.
* The HTTP upload endpoint takes a raw streamed request body (not a
  multipart form -- see ``upload_asset`` below) and writes it to disk in
  fixed-size chunks, read directly off ``Request.stream()``, rather than
  ever materializing the whole file as a single Python ``bytes`` object. A
  20-something MB IR/NAM file must not cost the daemon 20-something MB of
  RSS growth. ``tests/test_upload_memory.py`` benchmarks this with the
  stdlib ``resource`` module (see the README for measured numbers).
* The daemon never keeps decoded/parsed asset bytes in memory at all --
  only ``Asset`` metadata (id, filename, path, size, checksum). Actual
  model/IR bytes are the audio engine's concern, not the daemon's.
"""

from __future__ import annotations

import asyncio
import hashlib
import logging
import threading
import uuid
from pathlib import Path
from typing import List, Optional

from contextlib import asynccontextmanager

from fastapi import FastAPI, WebSocket
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic import ValidationError
from starlette.requests import Request
from starlette.websockets import WebSocketDisconnect

from .audio_engine_client import AudioEngineClient
from .connection_manager import ConnectionManager
from .state import DaemonStateManager, StateError
from .ws_protocol import (
    APP_ONLY_MESSAGES,
    FOOTSWITCH_ONLY_MESSAGES,
    MESSAGE_MODELS,
    CreatePresetMessage,
    CreateRigMessage,
    DeletePresetMessage,
    DeleteRigMessage,
    ErrorMessage,
    FootswitchPressMessage,
    HelloMessage,
    RegisterAssetMessage,
    ReorderRigsMessage,
    SelectPresetMessage,
    SetBypassMessage,
    SetFootswitchMappingMessage,
    StateChangedMessage,
    StateSnapshotMessage,
    UpdatePresetMessage,
    UpdateRigMessage,
)

logger = logging.getLogger("control_daemon.app")

# Chunk size for streaming the upload to disk. Small enough to keep peak
# memory use for the read/write buffer itself negligible, large enough to
# not be silly about syscall overhead for tens-of-MB files.
UPLOAD_CHUNK_SIZE = 1 << 20  # 1 MiB


def create_app(
    store_path: Path,
    audio_engine: Optional[AudioEngineClient] = None,
) -> FastAPI:
    """Build a fully-wired FastAPI app around a fresh DaemonStateManager.

    A factory (rather than a single module-level app) so tests can spin up
    independent, isolated daemon instances against their own temp store
    paths.
    """
    @asynccontextmanager
    async def _lifespan(fastapi_app: FastAPI):
        fastapi_app.state.main_loop = asyncio.get_running_loop()
        fastapi_app.state.main_thread_id = threading.get_ident()
        yield

    app = FastAPI(title="control-daemon", lifespan=_lifespan)
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_methods=["*"],
        allow_headers=["*"],
    )

    connection_manager = ConnectionManager()
    asset_dir = store_path.parent / "assets"

    # Reasons queued by a state mutation that happened synchronously on the
    # event loop's own thread (the overwhelmingly common case: a mutation
    # made directly inside a WS message handler). `_dispatch` drains this
    # right after sending its direct reply to the command's sender, so a
    # client always sees its own `command_ok` before the resulting
    # `state_changed` broadcast -- even though that broadcast also reaches
    # the sender itself, per "broadcast to ALL connected clients".
    pending_broadcasts: List[str] = []

    def _on_state_changed(reason: str) -> None:
        if threading.get_ident() == app.state.main_thread_id:
            pending_broadcasts.append(reason)
            return
        # Called from a different OS thread than the event loop's own (e.g.
        # a real GPIO interrupt callback driving FootswitchInputController
        # in-process) -- there's no in-flight WS dispatch to sequence with,
        # so schedule the broadcast directly, thread-safely.
        loop: Optional[asyncio.AbstractEventLoop] = app.state.main_loop
        if loop is None:
            return
        message = StateChangedMessage(
            state=state_manager.state_view(), reason=reason
        ).model_dump()
        asyncio.run_coroutine_threadsafe(connection_manager.broadcast(message), loop)

    state_manager = DaemonStateManager(
        store_path=store_path,
        audio_engine=audio_engine,
        on_change=_on_state_changed,
    )

    app.state.connection_manager = connection_manager
    app.state.state_manager = state_manager
    app.state.main_loop = None
    app.state.main_thread_id = None

    # ---------------------------------------------------------------- HTTP --

    @app.post("/assets/upload")
    async def upload_asset(request: Request) -> JSONResponse:
        """Stream a raw .nam/IR binary to disk. Deliberately plain HTTP, not
        part of the WS JSON protocol -- see README "deliberate deviations".

        Expects a ``kind`` query/header is *not* required; instead this is a
        raw streaming POST: ``?kind=nam&filename=amp.nam``. Keeping it a raw
        body (not multipart) means the request can be streamed straight
        through to disk with no intermediate form-parsing buffering at all.
        """
        kind = request.query_params.get("kind")
        filename = request.query_params.get("filename", "asset.bin")
        if kind not in ("nam", "ir"):
            return JSONResponse(
                {"error": "kind query param must be 'nam' or 'ir'"}, status_code=400
            )

        asset_dir.mkdir(parents=True, exist_ok=True)
        token = uuid.uuid4().hex[:16]
        suffix = Path(filename).suffix
        dest = asset_dir / f"{token}{suffix}"

        hasher = hashlib.sha256()
        size = 0
        # Stream the request body straight to disk in bounded chunks --
        # never buffer the whole upload in memory (see module docstring).
        with dest.open("wb") as f:
            async for chunk in request.stream():
                if not chunk:
                    continue
                f.write(chunk)
                hasher.update(chunk)
                size += len(chunk)

        digest = hasher.hexdigest()

        # Dedup by content, not filename. The checksum is only known once the
        # whole body has streamed, so the file is already on disk by now --
        # unlink it rather than leaving an orphan no asset id will ever
        # reference.
        existing = state_manager.find_asset_by_sha256(digest)
        if existing is not None:
            dest.unlink(missing_ok=True)
            return JSONResponse(
                {
                    "error": "duplicate_asset",
                    "message": (
                        f"identical content already registered as asset "
                        f"{existing.id} ({existing.filename})"
                    ),
                    "existing_asset_id": existing.id,
                },
                status_code=409,
            )

        return JSONResponse(
            {
                "kind": kind,
                "filename": filename,
                "stored_path": str(dest),
                "size_bytes": size,
                "sha256": digest,
            }
        )

    # ----------------------------------------------------------------- WS --

    @app.websocket("/ws")
    async def ws_endpoint(websocket: WebSocket) -> None:
        await connection_manager.connect(websocket)
        role: Optional[str] = None
        try:
            first_raw = await websocket.receive_json()
            if not isinstance(first_raw, dict) or first_raw.get("type") != "hello":
                await websocket.send_json(
                    ErrorMessage(
                        code="hello_required",
                        message="first message must be {type: 'hello', role: ...}",
                    ).model_dump()
                )
                await websocket.close()
                return

            try:
                hello = HelloMessage.model_validate(first_raw)
            except ValidationError as exc:
                await websocket.send_json(
                    ErrorMessage(code="validation_error", message=str(exc)).model_dump()
                )
                await websocket.close()
                return

            role = hello.role
            await connection_manager.set_role(websocket, role)
            await websocket.send_json(
                StateSnapshotMessage(state=state_manager.state_view()).model_dump()
            )

            while True:
                raw = await websocket.receive_json()
                await _dispatch(
                    websocket, role, raw, state_manager, connection_manager, pending_broadcasts
                )
        except WebSocketDisconnect:
            pass
        finally:
            await connection_manager.disconnect(websocket)

    return app


async def _dispatch(
    websocket: WebSocket,
    role: str,
    raw: dict,
    state_manager: DaemonStateManager,
    connection_manager: ConnectionManager,
    pending_broadcasts: List[str],
) -> None:
    if not isinstance(raw, dict) or "type" not in raw:
        await websocket.send_json(
            ErrorMessage(code="validation_error", message="message must have a 'type'").model_dump()
        )
        return

    msg_type = raw["type"]
    model_cls = MESSAGE_MODELS.get(msg_type)
    if model_cls is None:
        await websocket.send_json(
            ErrorMessage(
                code="unknown_message_type", message=f"unknown message type {msg_type!r}"
            ).model_dump()
        )
        return

    if msg_type in APP_ONLY_MESSAGES and role != "app":
        await websocket.send_json(
            ErrorMessage(
                code="role_forbidden",
                message=f"role {role!r} may not send {msg_type!r}; this is an app-only command",
            ).model_dump()
        )
        return

    if msg_type in FOOTSWITCH_ONLY_MESSAGES and role != "footswitch":
        await websocket.send_json(
            ErrorMessage(
                code="role_forbidden",
                message=f"role {role!r} may not send {msg_type!r}; this is a footswitch-only event",
            ).model_dump()
        )
        return

    try:
        message = model_cls.model_validate(raw)
    except ValidationError as exc:
        await websocket.send_json(
            ErrorMessage(code="validation_error", message=str(exc)).model_dump()
        )
        return

    try:
        result = _apply(state_manager, message)
    except StateError as exc:
        await websocket.send_json(
            ErrorMessage(code=exc.code, message=exc.message).model_dump()
        )
        return
    except Exception as exc:  # pragma: no cover - defensive catch-all
        logger.exception("unhandled error applying %s", msg_type)
        await websocket.send_json(
            ErrorMessage(code="internal_error", message=str(exc)).model_dump()
        )
        return

    if result is not None:
        await websocket.send_json(
            {"type": "command_ok", "command": msg_type, "result": result}
        )

    # Flush any broadcast(s) the mutation above queued -- always *after* the
    # direct reply above, so the sender's own command_ok is never preceded
    # by the broadcast copy of the same change (see _on_state_changed).
    while pending_broadcasts:
        reason = pending_broadcasts.pop(0)
        await connection_manager.broadcast(
            StateChangedMessage(state=state_manager.state_view(), reason=reason).model_dump()
        )


def _apply(state_manager: DaemonStateManager, message) -> Optional[dict]:
    """Apply one validated, role-checked client message. Returns a JSON-able
    dict to send back to the sender as `command_ok.result`, or None to send
    nothing beyond the (already-happening) broadcast."""

    if isinstance(message, CreateRigMessage):
        rig = state_manager.create_rig(name=message.name, chain=message.chain)
        return {"rig": rig.model_dump(mode="json")}

    if isinstance(message, UpdateRigMessage):
        rig = state_manager.update_rig(
            rig_id=message.rig_id, name=message.name, chain=message.chain
        )
        return {"rig": rig.model_dump(mode="json")}

    if isinstance(message, DeleteRigMessage):
        state_manager.delete_rig(message.rig_id)
        return {"rig_id": message.rig_id}

    if isinstance(message, ReorderRigsMessage):
        state_manager.reorder_rigs(message.rig_ids)
        return {"rig_ids": message.rig_ids}

    if isinstance(message, CreatePresetMessage):
        preset = state_manager.create_preset(
            rig_id=message.rig_id,
            name=message.name,
            block_states=message.block_states,
        )
        return {"preset": preset.model_dump(mode="json")}

    if isinstance(message, UpdatePresetMessage):
        preset = state_manager.update_preset(
            rig_id=message.rig_id,
            preset_id=message.preset_id,
            name=message.name,
            block_states=message.block_states,
        )
        return {"preset": preset.model_dump(mode="json")}

    if isinstance(message, DeletePresetMessage):
        state_manager.delete_preset(message.rig_id, message.preset_id)
        return {"preset_id": message.preset_id}

    if isinstance(message, SelectPresetMessage):
        state_manager.select_preset(
            rig_index=message.rig_index,
            preset_index=message.preset_index,
        )
        view = state_manager.state_view()
        return {
            "active_rig_id": view["active_rig_id"],
            "active_preset_id": view["active_preset_id"],
        }

    if isinstance(message, SetBypassMessage):
        state_manager.set_bypass(message.bypass)
        return {"bypass": state_manager.state.bypass}

    if isinstance(message, SetFootswitchMappingMessage):
        state_manager.set_footswitch_mapping(message.mapping)
        return {"footswitch_mapping": state_manager.state_view()["footswitch_mapping"]}

    if isinstance(message, RegisterAssetMessage):
        asset = state_manager.register_asset(
            kind=message.kind,
            filename=message.filename,
            stored_path=message.stored_path,
            size_bytes=message.size_bytes,
            sha256=message.sha256,
        )
        return {"asset": asset.model_dump(mode="json")}

    if isinstance(message, FootswitchPressMessage):
        state_manager.apply_footswitch_press(message.switch_index)
        return None

    raise AssertionError(f"no handler wired for message type {type(message)!r}")
