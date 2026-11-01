"""Reboot / poweroff requests from the app."""

import subprocess
import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from control_daemon import system_power as sp
from control_daemon.app import create_app
from control_daemon.system_power import SystemPower, SystemPowerError


class _FakeRun:
    def __init__(self, can: str = 's "yes"', returncode: int = 0) -> None:
        self.can, self.returncode = can, returncode
        self.calls = []

    def __call__(self, args, **kwargs):
        self.calls.append(args)
        out = self.can if args[0] == "busctl" else ""
        return subprocess.CompletedProcess(args, self.returncode, stdout=out, stderr="")


@pytest.fixture(autouse=True)
def _pretend_busctl_exists(monkeypatch):
    monkeypatch.setattr(sp.shutil, "which", lambda name: f"/usr/bin/{name}")


def test_check_asks_logind_and_accepts_only_yes():
    run = _FakeRun()
    SystemPower(run=run).check("reboot")
    assert run.calls[0][-1] == "CanReboot"

    for refusal in ('s "challenge"', 's "no"'):
        with pytest.raises(SystemPowerError, match="polkit"):
            SystemPower(run=_FakeRun(can=refusal)).check("poweroff")


def test_schedule_runs_systemctl_after_the_delay():
    run = _FakeRun()
    SystemPower(delay_s=0.05, run=run).schedule("poweroff")
    assert run.calls == []  # not before the reply has gone out
    time.sleep(0.3)
    assert run.calls == [["systemctl", "poweroff"]]


class _FakePower:
    def __init__(self, allowed: bool = True) -> None:
        self.allowed = allowed
        self.scheduled = []

    def check(self, action):
        if not self.allowed:
            raise SystemPowerError("nope")

    def schedule(self, action):
        self.scheduled.append(action)


def _hello(ws, role):
    ws.send_json({"type": "hello", "role": role})
    ws.receive_json()


def test_app_can_request_a_reboot(tmp_path: Path):
    power = _FakePower()
    with TestClient(create_app(tmp_path / "s.json", system_power=power)) as client:
        with client.websocket_connect("/ws") as ws:
            _hello(ws, "app")
            ws.send_json({"type": "system_power", "action": "reboot"})
            reply = ws.receive_json()
    assert reply == {"type": "command_ok", "command": "system_power", "result": {"action": "reboot"}}
    assert power.scheduled == ["reboot"]


def test_refused_request_is_an_error_and_does_nothing(tmp_path: Path):
    power = _FakePower(allowed=False)
    with TestClient(create_app(tmp_path / "s.json", system_power=power)) as client:
        with client.websocket_connect("/ws") as ws:
            _hello(ws, "app")
            ws.send_json({"type": "system_power", "action": "poweroff"})
            reply = ws.receive_json()
    assert reply["type"] == "error" and reply["code"] == "power_refused"
    assert power.scheduled == []


def test_display_cannot_power_off_the_pedal(tmp_path: Path):
    power = _FakePower()
    with TestClient(create_app(tmp_path / "s.json", system_power=power)) as client:
        with client.websocket_connect("/ws") as ws:
            _hello(ws, "display")
            ws.send_json({"type": "system_power", "action": "poweroff"})
            assert ws.receive_json()["code"] == "role_forbidden"
    assert power.scheduled == []
