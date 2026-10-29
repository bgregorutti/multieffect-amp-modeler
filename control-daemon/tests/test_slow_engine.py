"""A slow audio engine (a Pi loading a big NAM model) must not freeze the
daemon: every other client keeps being served while one command waits."""

import threading
import time
from pathlib import Path

from fastapi.testclient import TestClient

from control_daemon.app import create_app
from control_daemon.audio_engine_client import NullAudioEngineClient

LOAD_DELAY_S = 1.5


class SlowEngine(NullAudioEngineClient):
    def __init__(self) -> None:
        self.delay_s = 0.0

    def load_preset(self, preset) -> None:
        time.sleep(self.delay_s)


def _hello(ws, role: str):
    ws.send_json({"type": "hello", "role": role})
    return ws.receive_json()


def test_slow_load_preset_does_not_block_other_clients(tmp_path: Path):
    engine = SlowEngine()
    with TestClient(create_app(tmp_path / "state.json", audio_engine=engine)) as client:
        with client.websocket_connect("/ws") as app_ws:
            _hello(app_ws, "app")
            app_ws.send_json({"type": "create_rig", "name": "R", "chain": [{"id": "a", "type": "nam"}]})
            rig_id = app_ws.receive_json()["result"]["rig"]["id"]
            app_ws.receive_json()
            for name in ("One", "Two"):
                app_ws.send_json({"type": "create_preset", "rig_id": rig_id, "name": name})
                app_ws.receive_json()
                app_ws.receive_json()

            engine.delay_s = LOAD_DELAY_S
            replies = []

            def select() -> None:
                app_ws.send_json({"type": "select_preset", "preset_index": 1})
                replies.append(app_ws.receive_json())
                replies.append(app_ws.receive_json())

            worker = threading.Thread(target=select)
            worker.start()
            time.sleep(0.3)  # the engine call is now in flight

            started = time.monotonic()
            with client.websocket_connect("/ws") as other:
                snapshot = _hello(other, "display")
            latency = time.monotonic() - started
            worker.join()

            assert snapshot["type"] == "state_snapshot"
            assert latency < 0.5, f"daemon was frozen for {latency:.2f}s during a slow engine call"
            # Ordering the phone relies on: its command_ok, then the broadcast.
            assert [m["type"] for m in replies] == ["command_ok", "state_changed"]
