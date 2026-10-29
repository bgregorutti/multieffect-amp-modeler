import socket
import tempfile
from pathlib import Path

from control_daemon import sd_notify


def test_noop_without_notify_socket(monkeypatch):
    monkeypatch.delenv("NOTIFY_SOCKET", raising=False)
    assert sd_notify.notify("READY=1") is False


def test_sends_state_to_notify_socket(monkeypatch):
    # Short dir: AF_UNIX paths are capped at ~104 bytes on macOS.
    path = Path(tempfile.mkdtemp(prefix="sdn")) / "n.sock"
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as server:
        server.bind(str(path))
        monkeypatch.setenv("NOTIFY_SOCKET", str(path))
        assert sd_notify.notify("READY=1") is True
        assert server.recv(64) == b"READY=1"


def test_unreachable_socket_fails_open(monkeypatch):
    monkeypatch.setenv("NOTIFY_SOCKET", "/nonexistent/notify.sock")
    assert sd_notify.notify("READY=1") is False
