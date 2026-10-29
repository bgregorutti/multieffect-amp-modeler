from display_client.boot import (
    UNKNOWN,
    SmoothProgress,
    UnitState,
    boot_complete,
    boot_status,
    parse_systemctl_show,
)

ACTIVATING = UnitState(active="activating")
ACTIVE = UnitState(active="active")
FAILED = UnitState(active="failed")
MISSING = UnitState(load="not-found", active="inactive")


def test_steps_advance_with_service_readiness():
    first = boot_status(ACTIVATING, UnitState(), connected=False)
    assert first.fraction == 0.25 and first.label == "Starting audio engine"

    engine_up = boot_status(ACTIVE, ACTIVATING, connected=False)
    assert engine_up.fraction == 0.5 and engine_up.label == "Starting control daemon"

    daemon_up = boot_status(ACTIVE, ACTIVE, connected=False)
    assert daemon_up.fraction == 0.75 and daemon_up.label == "Connecting to control daemon"

    done = boot_status(ACTIVE, ACTIVE, connected=True)
    assert done.fraction == 1.0 and done.label == "Ready"


def test_failed_engine_warns_but_does_not_block():
    # The daemon (and the app) work without an engine, so neither should
    # the pedal's screen.
    status = boot_status(FAILED, ACTIVE, connected=True)
    assert status.fraction == 1.0
    assert status.warning == "Audio engine failed to start"
    assert status.error is None
    assert boot_complete(FAILED, connected=True)


def test_failed_daemon_is_an_error():
    status = boot_status(ACTIVE, FAILED, connected=False)
    assert status.error and "control-daemon" in status.error


def test_missing_units_count_as_done():
    # e.g. running on a dev machine, or a pedal deployed without an engine
    assert boot_status(MISSING, MISSING, connected=True).fraction == 1.0


def test_boot_waits_for_engine_after_connecting():
    assert not boot_complete(ACTIVATING, connected=True)
    assert not boot_complete(ACTIVE, connected=False)
    assert boot_complete(ACTIVE, connected=True)
    # No systemd to ask (dev machine): the snapshot alone is enough.
    assert boot_complete(UNKNOWN, connected=True)


def test_smooth_progress_creeps_but_never_passes_the_next_step():
    progress = SmoothProgress()
    values = [progress.update(0.25, 0.2) for _ in range(500)]
    assert values == sorted(values)
    assert 0.25 < values[-1] < 0.5


def test_smooth_progress_never_moves_backwards():
    progress = SmoothProgress()
    for _ in range(50):
        progress.update(0.75, 0.2)
    high = progress.value
    assert progress.update(0.25, 0.2) >= high


def test_parse_systemctl_show():
    output = "LoadState=loaded\nActiveState=active\n\nLoadState=not-found\nActiveState=inactive\n"
    states = parse_systemctl_show(output, ["a.service", "b.service"])
    assert states["a.service"] == ACTIVE
    assert states["b.service"].missing
