"""Benchmark: uploading a large (~20MB) synthetic asset must not balloon the
daemon process's memory footprint.

This is the regression guard for the "runs alongside the real-time audio
engine on an 8GB-RAM-max Raspberry Pi" resource constraint: the upload
endpoint streams the request body straight to disk in small fixed-size
chunks (see app.py:upload_asset) rather than ever materializing the whole
file as a single Python object, so peak RSS growth during a big upload
should be a small fraction of the file's size, not comparable to it.

This runs a *real* uvicorn server on a loopback socket (rather than driving
the app through FastAPI's ASGI-transport TestClient) and uploads via a real
streamed HTTP request. That's deliberate: an in-process ASGI test transport
can itself materialize the request body while bridging it to the app,
which would confound a memory measurement of our own streaming code with
the test harness's behavior. A real socket forces the exact code path
(uvicorn -> Starlette request.stream() -> our chunked write loop) that runs
in production on the Pi.

Uses only the stdlib `resource` module (no extra dependency such as
psutil). `ru_maxrss` is a monotonically non-decreasing high-water mark for
the whole process (KiB on Linux); it's a coarse proxy, not a precise
allocator trace, so the pass threshold below is deliberately generous while
still catching an obviously-wrong "read the whole upload into bytes"
implementation, which would push growth close to (or beyond, with copies)
the file size.
"""

import contextlib
import os
import resource
import socket
import threading
import time
from pathlib import Path
from typing import Iterator

import httpx
import uvicorn

from control_daemon.app import create_app

FILE_SIZE_BYTES = 20 * 1024 * 1024  # ~20MB, representative of a real IR/NAM file
CHUNK_SIZE = 1024 * 1024  # 1MB


def _write_big_file(path: Path, size: int) -> None:
    # Write in small chunks so *generating the fixture* doesn't itself
    # require holding the whole file in memory.
    block = os.urandom(CHUNK_SIZE)
    with path.open("wb") as f:
        remaining = size
        while remaining > 0:
            n = min(CHUNK_SIZE, remaining)
            f.write(block[:n])
            remaining -= n


def _stream_open_file(f) -> Iterator[bytes]:
    while True:
        chunk = f.read(CHUNK_SIZE)
        if not chunk:
            return
        yield chunk


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@contextlib.contextmanager
def _run_server(app):
    port = _free_port()
    config = uvicorn.Config(app, host="127.0.0.1", port=port, log_level="warning")
    server = uvicorn.Server(config)
    thread = threading.Thread(target=server.run, daemon=True)
    thread.start()
    try:
        deadline = time.monotonic() + 10.0
        while not server.started and time.monotonic() < deadline:
            time.sleep(0.02)
        assert server.started, "uvicorn server did not start in time"
        yield f"http://127.0.0.1:{port}"
    finally:
        server.should_exit = True
        thread.join(timeout=10.0)


def test_large_upload_does_not_buffer_whole_file_in_memory(tmp_path: Path):
    app = create_app(store_path=tmp_path / "state.json")
    big_file = tmp_path / "big_test_asset.nam"
    _write_big_file(big_file, FILE_SIZE_BYTES)

    with _run_server(app) as base_url, httpx.Client(base_url=base_url, timeout=30.0) as client:
        # Warm-up request so one-time costs (import machinery, socket/TLS
        # setup, etc.) don't pollute the baseline measurement below.
        client.post(
            "/assets/upload",
            params={"kind": "nam", "filename": "warmup.bin"},
            content=b"x" * 1024,
        )

        baseline_kb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

        with big_file.open("rb") as f:
            response = client.post(
                "/assets/upload",
                params={"kind": "nam", "filename": "big_test_asset.nam"},
                content=_stream_open_file(f),
            )

        after_kb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

    assert response.status_code == 200
    body = response.json()
    assert body["size_bytes"] == FILE_SIZE_BYTES
    assert len(body["sha256"]) == 64

    growth_mb = (after_kb - baseline_kb) / 1024
    file_mb = FILE_SIZE_BYTES / (1024 * 1024)

    print(
        f"\n[memory benchmark] uploaded {file_mb:.1f} MB over a real HTTP "
        f"connection; peak RSS growth = {growth_mb:.1f} MB (baseline "
        f"ru_maxrss={baseline_kb} KB, after={after_kb} KB)"
    )

    assert growth_mb < file_mb * 0.5, (
        f"peak RSS grew {growth_mb:.1f} MB while uploading a {file_mb:.1f} MB "
        "file -- this suggests the upload endpoint is buffering the file "
        "in memory instead of streaming it to disk"
    )
