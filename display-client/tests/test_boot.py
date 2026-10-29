from display_client.boot import (
    UNKNOWN,
    SmoothProgress,
    ENGINE_WAIT_S,
    UnitState,
    boot_complete,
    boot_status,
    parse_systemctl_show,
)

ACTIVATING = UnitState(active="activating")
ACTIVE = UnitState(active="active")
FAILED = UnitState(active="failed")
MISSING = UnitState(load="not-found", active="inactive")
# What systemd reports between Restart= attempts of a crashing unit -- e.g.
# the audio engine with its USB interface unplugged.
RESTARTING = UnitState(active="activating", sub="auto-restart")


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
    assert "Audio engine not running" in status.warning
    assert status.error is None
    assert boot_complete(FAILED, connected=True)


def test_crash_looping_engine_counts_as_settled():
    # Never reported as "failed" while systemd keeps restarting it, so
    # waiting for "active or failed" would hold the boot screen forever.
    status = boot_status(RESTARTING, ACTIVE, connected=True)
    assert status.fraction == 1.0
    assert "Audio engine not running" in status.warning
    assert boot_complete(RESTARTING, connected=True)


def test_engine_stuck_starting_is_waited_for_only_so_long():
    assert not boot_complete(ACTIVATING, connected=True, connected_s=ENGINE_WAIT_S - 1)
    assert boot_complete(ACTIVATING, connected=True, connected_s=ENGINE_WAIT_S)
    # The limit runs from the daemon connecting, not from power-on.
    assert not boot_complete(ACTIVATING, connected=False, connected_s=ENGINE_WAIT_S)


def test_restarting_daemon_does_not_count_as_started():
    status = boot_status(ACTIVE, RESTARTING, connected=False)
    assert status.fraction == 0.5
    assert status.warning == "Control daemon is restarting"


def test_slow_step_shows_elapsed_time():
    assert boot_status(ACTIVATING, ACTIVATING, False, step_s=2.0).elapsed_s is None
    assert boot_status(ACTIVATING, ACTIVATING, False, step_s=12.7).elapsed_s == 12
    assert boot_status(ACTIVE, ACTIVE, True, step_s=12.7).elapsed_s is None


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
    output = (
        "LoadState=loaded\nActiveState=active\nSubState=running\n\n"
        "LoadState=loaded\nActiveState=activating\nSubState=auto-restart\n\n"
        "LoadState=not-found\nActiveState=inactive\nSubState=dead\n"
    )
    states = parse_systemctl_show(output, ["a.service", "b.service", "c.service"])
    assert states["a.service"].ready
    assert states["b.service"].restarting and states["b.service"].settled
    assert states["c.service"].missing
