import pytest
from PIL import ImageStat

from display_client.boot import BootStatus
from display_client.icons import ICON_DIR
from display_client.render import BAD, Renderer, format_db
from display_client.view import BLOCK_ICONS, Chip, LiveView

SIZES = [(480, 320), (320, 240)]

LIVE = LiveView(
    rig_name="A rig name long enough to need shrinking and ellipsizing for sure",
    rig_number=3, rig_count=12,
    preset_name="An extremely long preset name that cannot possibly fit on two lines here",
    preset_number=10, preset_count=10,
    chain=[Chip(label, i % 2 == 0, icon) for i, (label, icon) in enumerate(
        [("TS", "drv"), ("MUFF", "fuzz"), ("GATE", None), ("AMP", "amp"), ("CAB", "cab"),
         ("TONE", "eq"), ("EQ", "eq"), ("DLY", "dly"), ("VST", None)])],
    amp_name="An amp capture name far too long for half the screen width",
    cab_name="Cab", input_gain_db=-12.5, volume_db=6.0,
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


def test_every_mapped_block_type_has_an_image():
    assert all((ICON_DIR / f"{name}.png").is_file() for name in BLOCK_ICONS.values())


def test_off_block_image_is_drawn_dimmer_than_on():
    renderer = Renderer((480, 320))
    def frame(enabled):
        view = LiveView(rig_count=1, rig_number=1, chain=[Chip("DLY", enabled, "dly")])
        return renderer.live(view, connected=True).crop((0, 163, 480, 246)).convert("L")
    assert ImageStat.Stat(frame(True)).sum[0] > ImageStat.Stat(frame(False)).sum[0]


def test_format_db():
    assert [format_db(v) for v in (3.0, -4.25, -0.01, 0.0)] == ["+3.0 dB", "−4.2 dB", "+0.0 dB", "+0.0 dB"]
