"""Physical footswitches wired into the daemon: pin-list parsing, the default
mapping seeded on first use, presses reaching every client, and the gpiozero
backend's polling (with a fake gpiozero, no hardware needed)."""

import sys
import time
import types
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from control_daemon.app import create_app
from control_daemon.gpio import (
    FootswitchInputController,
    GpioZeroFootswitchBackend,
    MockFootswitchBackend,
)
from control_daemon.main import parse_footswitch_pins
from control_daemon.models import ToggleBypassAction


def _hello(ws, role: str):
    ws.send_json({"type": "hello", "role": role})
    return ws.receive_json()


def _press(backend: MockFootswitchBackend, switch_index: int) -> None:
    """A clean press and release, held past the 20 ms debounce window."""
    backend.inject(switch_index, 0)
    time.sleep(0.03)
    backend.inject(switch_index, 0)
    backend.inject(switch_index, 1)
    time.sleep(0.03)
    backend.inject(switch_index, 1)


def test_parse_footswitch_pins():
    assert parse_footswitch_pins("0:5, 1:12,2:6,3:13") == {0: 5, 1: 12, 2: 6, 3: 13}
    assert parse_footswitch_pins("") == {}
    with pytest.raises(ValueError):
        parse_footswitch_pins("0-5")
    with pytest.raises(ValueError):
        parse_footswitch_pins("0:x")
    with pytest.raises(ValueError, match="more than one switch"):
        parse_footswitch_pins("0:5,1:5")


def test_default_mapping_seeded_when_none_saved(tmp_path: Path):
    app = create_app(tmp_path / "state.json", footswitch_backend=MockFootswitchBackend())
    with TestClient(app) as client:
        with client.websocket_connect("/ws") as ws:
            mapping = _hello(ws, "display")["state"]["footswitch_mapping"]
    assert {k: v["type"] for k, v in mapping.items()} == {
        "0": "next_preset", "1": "prev_preset", "2": "next_rig", "3": "prev_rig",
    }


def test_saved_mapping_is_kept(tmp_path: Path):
    store = tmp_path / "state.json"
    create_app(store).state.state_manager.set_footswitch_mapping({0: ToggleBypassAction()})

    app = create_app(store, footswitch_backend=MockFootswitchBackend())
    assert {k: a.type for k, a in app.state.state_manager.state.footswitch_mapping.items()} == {
        0: "toggle_bypass"
    }


def test_no_mapping_seeded_without_footswitches(tmp_path: Path):
    app = create_app(tmp_path / "state.json")
    assert app.state.state_manager.state.footswitch_mapping == {}


def test_press_changes_preset_and_reaches_clients(tmp_path: Path):
    backend = MockFootswitchBackend()
    with TestClient(create_app(tmp_path / "state.json", footswitch_backend=backend)) as client:
        with client.websocket_connect("/ws") as app_ws:
            _hello(app_ws, "app")
            app_ws.send_json({"type": "create_rig", "name": "R", "chain": []})
            rig_id = app_ws.receive_json()["result"]["rig"]["id"]
            app_ws.receive_json()
            for name in ("One", "Two"):
                app_ws.send_json({"type": "create_preset", "rig_id": rig_id, "name": name})
                app_ws.receive_json()
                app_ws.receive_json()

            with client.websocket_connect("/ws") as display:
                assert _hello(display, "display")["state"]["active_preset_index"] == 0

                _press(backend, 0)  # next_preset
                changed = display.receive_json()
                assert changed["type"] == "state_changed"
                assert changed["reason"] == "footswitch_next_preset"
                assert changed["state"]["active_preset_index"] == 1

                _press(backend, 1)  # prev_preset
                assert display.receive_json()["state"]["active_preset_index"] == 0


class _FakeButton:
    instances = []

    def __init__(self, pin, pull_up=True):
        self.pin = pin
        self.pull_up = pull_up
        self.is_pressed = False
        self.closed = False
        _FakeButton.instances.append(self)

    def close(self):
        self.closed = True


def test_gpiozero_backend_polls_levels_into_presses(monkeypatch):
    _FakeButton.instances = []
    monkeypatch.setitem(sys.modules, "gpiozero", types.SimpleNamespace(Button=_FakeButton))

    backend = GpioZeroFootswitchBackend(pins={0: 5, 1: 12}, poll_interval_s=0.002)
    presses = []
    controller = FootswitchInputController(backend=backend, on_press=presses.append)
    controller.start()
    try:
        buttons = {b.pin: b for b in _FakeButton.instances}
        assert all(b.pull_up for b in buttons.values())

        buttons[12].is_pressed = True
        time.sleep(0.1)
        # Held down: one press, no repeats.
        assert presses == [1]

        buttons[12].is_pressed = False
        time.sleep(0.05)
        buttons[12].is_pressed = True
        time.sleep(0.1)
        assert presses == [1, 1]
    finally:
        controller.stop()
    assert all(b.closed for b in _FakeButton.instances)
