import pytest

from display_client.boot import BootStatus
from display_client.render import BAD, Renderer
from display_client.view import Chip, LiveView

SIZES = [(480, 320), (320, 240)]

LIVE = LiveView(
    rig_name="A rig name long enough to need shrinking and ellipsizing for sure",
    rig_number=3, rig_count=12,
    preset_name="An extremely long preset name that cannot possibly fit on two lines here",
    preset_number=10, preset_count=10,
    chain=[Chip(label, i % 2 == 0) for i, label in enumerate(
        ["GAIN", "TS", "MUFF", "GATE", "AMP", "CAB", "TONE", "EQ", "DLY", "VST", "VOL"])],
)


@pytest.mark.parametrize("size", SIZES)
def test_every_screen_renders_at_configured_size(size):
    renderer = Renderer(size)
    frames = [
        renderer.boot(BootStatus(0.0, "Starting audio engine"), 0.0),
        renderer.boot(BootStatus(0.75, "Control daemon failed", "Audio engine failed to start",
                                 "journalctl -u multieffect-control-daemon"), 0.8),
        renderer.live(LIVE, connected=True),
        renderer.live(LiveView(bypass=True, engine_error="x" * 300), connected=False),
        renderer.live(LiveView(rig_name="R", rig_number=1, rig_count=1), connected=True),
        renderer.shutdown(),
    ]
    assert all(frame.size == size for frame in frames)


def test_bypass_draws_a_red_border():
    frame = Renderer((480, 320)).live(LiveView(rig_count=1, rig_number=1, bypass=True), connected=True)
    assert frame.getpixel((0, 160)) == BAD
    assert frame.getpixel((479, 319)) == BAD


def test_live_frame_differs_when_preset_changes():
    renderer = Renderer((480, 320))
    a = renderer.live(LiveView(rig_count=1, rig_number=1, preset_name="A", preset_number=1), True)
    b = renderer.live(LiveView(rig_count=1, rig_number=1, preset_name="B", preset_number=2), True)
    assert a.tobytes() != b.tobytes()
