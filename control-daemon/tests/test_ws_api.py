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
        assert snapshot["state"]["version"] == 1
        assert snapshot["state"]["presets"] == {}


def test_app_create_and_select_preset_broadcasts_to_display(client: TestClient):
    with client.websocket_connect("/ws") as app_ws, client.websocket_connect(
        "/ws"
    ) as display_ws:
        _hello(app_ws, "app")
        _hello(display_ws, "display")

        app_ws.send_json(
            {"type": "create_preset", "name": "Ambient Swell", "blocks": []}
        )
        create_ack = app_ws.receive_json()
        assert create_ack["type"] == "command_ok"
        assert create_ack["command"] == "create_preset"
        preset_id = create_ack["result"]["preset"]["id"]

        # Broadcasts go to ALL connected clients, including the sender
        # itself -- the direct command_ok above is always followed by the
        # same state_changed broadcast every other client gets.
        app_self_broadcast = app_ws.receive_json()
        assert app_self_broadcast["type"] == "state_changed"
        assert app_self_broadcast["reason"] == "create_preset"

        display_broadcast = display_ws.receive_json()
        assert display_broadcast["type"] == "state_changed"
        assert display_broadcast["reason"] == "create_preset"
        assert preset_id in display_broadcast["state"]["presets"]

        app_ws.send_json({"type": "select_preset", "preset_id": preset_id})
        select_ack = app_ws.receive_json()
        assert select_ack["type"] == "command_ok"
        assert select_ack["result"]["active_preset_id"] == preset_id
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

        app_ws.send_json({"type": "create_preset", "name": "P1"})
        create_ack = app_ws.receive_json()
        preset_id = create_ack["result"]["preset"]["id"]
        app_ws.receive_json()  # self broadcast

        app_ws.send_json(
            {
                "type": "create_bank",
                "name": "Bank A",
                "num_slots": 2,
            }
        )
        bank_ack = app_ws.receive_json()
        bank_id = bank_ack["result"]["bank"]["id"]
        app_ws.receive_json()  # self broadcast

        app_ws.send_json(
            {"type": "update_bank", "bank_id": bank_id, "slots": [preset_id, None]}
        )
        app_ws.receive_json()  # command_ok for update_bank
        app_ws.receive_json()  # self broadcast

        app_ws.send_json(
            {
                "type": "set_footswitch_mapping",
                "mapping": {"0": {"type": "select_slot", "slot": 0}},
            }
        )
        app_ws.receive_json()  # command_ok for set_footswitch_mapping
        app_ws.receive_json()  # self broadcast

        footswitch_ws.send_json({"type": "footswitch_press", "switch_index": 0})

        broadcast = app_ws.receive_json()
        assert broadcast["type"] == "state_changed"
        assert broadcast["state"]["active_preset_id"] == preset_id


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
