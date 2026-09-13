"""Atomic JSON persistence for DaemonState.

The store is a single plain JSON file (see models.py for the schema). Writes
are atomic: we write the full new state to a temp file in the same
directory, fsync it, then ``os.replace`` it over the real path. ``os.replace``
is a single filesystem rename, which POSIX guarantees is atomic -- a reader
(or a crash) can only ever see the fully-old file or the fully-new file,
never a half-written one. This matters because this daemon may run on a
Raspberry Pi where power can be pulled at any time.
"""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path

from .models import SCHEMA_VERSION, DaemonState


def load_state(path: Path) -> DaemonState:
    """Load daemon state from ``path``.

    Returns a fresh, empty default ``DaemonState`` if the file does not
    exist yet (first run on a new device).
    """
    if not path.exists():
        return DaemonState()

    raw = path.read_text(encoding="utf-8")
    data = json.loads(raw)

    on_disk_version = data.get("version")
    if on_disk_version != SCHEMA_VERSION:
        # Schema v1 is the only version that has ever existed, so there is
        # no migration to run yet. When the schema changes, add a
        # migration step here keyed by on_disk_version before validating.
        raise ValueError(
            f"Unsupported preset store schema version {on_disk_version!r} "
            f"(expected {SCHEMA_VERSION}); no migration is defined for it."
        )

    return DaemonState.model_validate(data)


def save_state(path: Path, state: DaemonState) -> None:
    """Atomically persist ``state`` as JSON to ``path``.

    Writes to a temp file in the same directory then renames over the
    target, so a crash mid-write can never leave a corrupt/partial store on
    disk -- readers only ever see the fully old file or the fully new one.
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = state.model_dump_json(indent=2)

    fd, tmp_path_str = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=str(path.parent)
    )
    tmp_path = Path(tmp_path_str)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(payload)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_path, path)
    except BaseException:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise
