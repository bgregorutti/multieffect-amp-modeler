"""End-to-end WebSocket API tests, driving the whole app through FastAPI's
TestClient (which also exercises the HTTP asset upload endpoint)."""

from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from control_daemon.app import create_app


@pytest.fixture()
def client(tmp_path: Path):
    app = create_app(store_path=tmp_path / "state.json")
    with TestClient(app) as c:
        yield c


def _hello(ws, role: str, name: str = None):
    ws.send_json({"type": "hello", "role": role, "client_name": name})
    return ws.receive_json()  # state_snapshot


def test_hello_must_be_first_message(client: TestClient):
    with client.websocket_connect("/ws") as ws:
        ws.send_json({"type": "set_bypass", "bypass": True})
        reply = ws.receive_json()
        assert reply["type"] == "error"
        assert reply["code"] == "hello_required"


def test_hello_gives_initial_state_snapshot(client: TestClient):
    with client.websocket_connect("/ws") as ws:
        snapshot = _hello(ws, "app")
        assert snapshot["type"] == "state_snapshot"
        assert snapshot["state"]["version"] == 2
        assert snapshot["state"]["rigs"] == []


def test_app_create_rig_and_select_preset_broadcasts_to_display(client: TestClient):
    with client.websocket_connect("/ws") as app_ws, client.websocket_connect(
        "/ws"
    ) as display_ws:
        _hello(app_ws, "app")
        _hello(display_ws, "display")

        app_ws.send_json(
            {
                "type": "create_rig",
                "name": "Ampeg SVT",
                "chain": [
                    {"id": "amp", "type": "nam", "pinned": True},
                    {"id": "dist", "type": "distortion", "enabled": False},
                ],
            }
        )
        create_ack = app_ws.receive_json()
        assert create_ack["type"] == "command_ok"
        assert create_ack["command"] == "create_rig"
        rig_id = create_ack["result"]["rig"]["id"]

        # Broadcasts go to ALL connected clients, including the sender
        # itself -- the direct command_ok above is always followed by the
        # same state_changed broadcast every other client gets.
        app_self_broadcast = app_ws.receive_json()
        assert app_self_broadcast["type"] == "state_changed"
        assert app_self_broadcast["reason"] == "create_rig"

        display_broadcast = display_ws.receive_json()
        assert display_broadcast["type"] == "state_changed"
        assert display_broadcast["reason"] == "create_rig"
        assert display_broadcast["state"]["rigs"][0]["id"] == rig_id

        app_ws.send_json(
            {
                "type": "create_preset",
                "rig_id": rig_id,
                "name": "Drive",
                "block_states": {"dist": {"enabled": True}},
            }
        )
        preset_ack = app_ws.receive_json()
        preset_id = preset_ack["result"]["preset"]["id"]
        app_ws.receive_json()  # self broadcast
        display_ws.receive_json()

        app_ws.send_json(
            {"type": "select_preset", "rig_index": 0, "preset_index": 0}
        )
        select_ack = app_ws.receive_json()
        assert select_ack["type"] == "command_ok"
        assert select_ack["result"]["active_preset_id"] == preset_id
        assert select_ack["result"]["active_rig_id"] == rig_id
        app_ws.receive_json()  # self broadcast for select_preset

        display_broadcast_2 = display_ws.receive_json()
        assert display_broadcast_2["type"] == "state_changed"
        assert display_broadcast_2["reason"] == "select_preset"
        assert display_broadcast_2["state"]["active_preset_id"] == preset_id


def test_display_role_cannot_send_config_commands(client: TestClient):
    with client.websocket_connect("/ws") as ws:
        _hello(ws, "display")
        ws.send_json({"type": "set_bypass", "bypass": True})
        reply = ws.receive_json()
        assert reply["type"] == "error"
        assert reply["code"] == "role_forbidden"


def test_footswitch_role_cannot_send_config_commands(client: TestClient):
    with client.websocket_connect("/ws") as ws:
        _hello(ws, "footswitch")
        ws.send_json({"type": "create_preset", "name": "Nope"})
        reply = ws.receive_json()
        assert reply["type"] == "error"
        assert reply["code"] == "role_forbidden"


def test_app_role_cannot_send_footswitch_press(client: TestClient):
    with client.websocket_connect("/ws") as ws:
        _hello(ws, "app")
        ws.send_json({"type": "footswitch_press", "switch_index": 0})
        reply = ws.receive_json()
        assert reply["type"] == "error"
        assert reply["code"] == "role_forbidden"


def test_unknown_message_type(client: TestClient):
    with client.websocket_connect("/ws") as ws:
        _hello(ws, "app")
        ws.send_json({"type": "not_a_real_command"})
        reply = ws.receive_json()
        assert reply["type"] == "error"
        assert reply["code"] == "unknown_message_type"


def test_footswitch_press_triggers_mapped_action_and_broadcasts(client: TestClient):
    with client.websocket_connect("/ws") as app_ws, client.websocket_connect(
        "/ws"
    ) as footswitch_ws:
        _hello(app_ws, "app")
        _hello(footswitch_ws, "footswitch")

        app_ws.send_json({"type": "create_rig", "name": "Rig A", "chain": []})
        rig_ack = app_ws.receive_json()
        rig_id = rig_ack["result"]["rig"]["id"]
        app_ws.receive_json()  # self broadcast

        app_ws.send_json(
            {"type": "create_preset", "rig_id": rig_id, "name": "Clean"}
        )
        app_ws.receive_json()  # command_ok
        app_ws.receive_json()  # self broadcast

        app_ws.send_json(
            {"type": "create_preset", "rig_id": rig_id, "name": "Drive"}
        )
        second_ack = app_ws.receive_json()
        second_preset_id = second_ack["result"]["preset"]["id"]
        app_ws.receive_json()  # self broadcast

        app_ws.send_json(
            {
                "type": "set_footswitch_mapping",
                "mapping": {"0": {"type": "next_preset"}},
            }
        )
        app_ws.receive_json()  # command_ok for set_footswitch_mapping
        app_ws.receive_json()  # self broadcast

        footswitch_ws.send_json({"type": "footswitch_press", "switch_index": 0})

        broadcast = app_ws.receive_json()
        assert broadcast["type"] == "state_changed"
        assert broadcast["reason"] == "footswitch_next_preset"
        assert broadcast["state"]["active_preset_id"] == second_preset_id


def test_asset_upload_then_register(client: TestClient):
    payload = b"fake nam model bytes"
    response = client.post(
        "/assets/upload?kind=nam&filename=my_amp.nam",
        content=payload,
    )
    assert response.status_code == 200
    body = response.json()
    assert body["kind"] == "nam"
    assert body["size_bytes"] == len(payload)
    assert len(body["sha256"]) == 64

    with client.websocket_connect("/ws") as ws:
        _hello(ws, "app")
        ws.send_json(
            {
                "type": "register_asset",
                "kind": body["kind"],
                "filename": body["filename"],
                "stored_path": body["stored_path"],
                "size_bytes": body["size_bytes"],
                "sha256": body["sha256"],
            }
        )
        ack = ws.receive_json()
        assert ack["type"] == "command_ok"
        assert ack["result"]["asset"]["filename"] == "my_amp.nam"
        assert ack["result"]["asset"]["sha256"] == body["sha256"]


def test_duplicate_upload_is_refused_and_leaves_no_orphan_file(client: TestClient):
    payload = b"fake ir bytes"

    first = client.post("/assets/upload?kind=ir&filename=cab.wav", content=payload)
    assert first.status_code == 200
    stored_path = Path(first.json()["stored_path"])

    with client.websocket_connect("/ws") as ws:
        _hello(ws, "app")
        ws.send_json({"type": "register_asset", **{
            k: first.json()[k]
            for k in ("kind", "filename", "stored_path", "size_bytes", "sha256")
        }})
        assert ws.receive_json()["type"] == "command_ok"

    # Same bytes under a different name -- refused on content, and the
    # second file must not be left behind on disk.
    before = sorted(p.name for p in stored_path.parent.iterdir())
    second = client.post("/assets/upload?kind=ir&filename=other.wav", content=payload)
    assert second.status_code == 409
    assert second.json()["error"] == "duplicate_asset"
    assert sorted(p.name for p in stored_path.parent.iterdir()) == before
    assert stored_path.exists()


def test_register_asset_duplicate_is_refused_over_ws(client: TestClient):
    payload = b"some nam bytes"
    body = client.post("/assets/upload?kind=nam&filename=amp.nam", content=payload).json()

    with client.websocket_connect("/ws") as ws:
        _hello(ws, "app")
        register = {"type": "register_asset", **{
            k: body[k] for k in ("kind", "filename", "stored_path", "size_bytes", "sha256")
        }}
        ws.send_json(register)
        assert ws.receive_json()["type"] == "command_ok"
        assert ws.receive_json()["type"] == "state_changed"

        ws.send_json({**register, "filename": "amp_copy.nam"})
        err = ws.receive_json()
        assert err["type"] == "error"
        assert err["code"] == "duplicate_asset"
