from pathlib import Path

import pytest
from PIL import Image, ImageDraw

from display_client.framebuffer import (
    FramebufferSink,
    RotatedSink,
    find_framebuffer,
    pack_pixels,
    pack_rgb565,
)


def _fake_fb(tmp_path: Path, index: int, name: str, size=(8, 4), bpp=16, stride=None) -> str:
    info = tmp_path / "sys" / f"fb{index}"
    info.mkdir(parents=True)
    (info / "name").write_text(name + "\n")
    (info / "virtual_size").write_text(f"{size[0]},{size[1]}\n")
    (info / "bits_per_pixel").write_text(f"{bpp}\n")
    (info / "stride").write_text(f"{stride or size[0] * bpp // 8}\n")
    device = tmp_path / f"fb{index}"
    device.write_bytes(b"\xAA" * (stride or size[0] * bpp // 8) * size[1])
    return str(device)


def test_rgb565_little_endian():
    def px(colour):
        return pack_rgb565(Image.new("RGB", (1, 1), colour))

    assert px((255, 0, 0)) == b"\x00\xf8"
    assert px((0, 255, 0)) == b"\xe0\x07"
    assert px((0, 0, 255)) == b"\x1f\x00"


def test_32bpp_is_bgrx():
    assert pack_pixels(Image.new("RGB", (1, 1), (1, 2, 3)), 32) == b"\x03\x02\x01\x00"


def test_unsupported_depth():
    with pytest.raises(ValueError):
        pack_pixels(Image.new("RGB", (1, 1)), 8)


def test_find_framebuffer_skips_hdmi(tmp_path):
    _fake_fb(tmp_path, 0, "vc4drmfb")
    _fake_fb(tmp_path, 1, "fb_ili9486")
    assert find_framebuffer(tmp_path / "sys") == "/dev/fb1"


def test_find_framebuffer_falls_back_to_fb0(tmp_path):
    _fake_fb(tmp_path, 0, "vc4drmfb")
    assert find_framebuffer(tmp_path / "sys") == "/dev/fb0"
    assert find_framebuffer(tmp_path / "nothing") is None


def test_first_frame_full_then_only_changed_rows(tmp_path):
    device = _fake_fb(tmp_path, 1, "fb_ili9486", size=(8, 4))
    sink = FramebufferSink(device, sysfs_root=tmp_path / "sys")
    assert sink.size == (8, 4)

    sink.show(Image.new("RGB", (8, 4), (0, 0, 0)))
    assert Path(device).read_bytes() == b"\x00" * 64

    # Poison the device behind the sink's back: rows a diff-based write
    # doesn't touch must keep the poison.
    with open(device, "r+b") as f:
        f.write(b"\xEE" * 64)
    frame = Image.new("RGB", (8, 4), (0, 0, 0))
    ImageDraw.Draw(frame).point((3, 2), fill=(255, 255, 255))
    sink.show(frame)
    data = Path(device).read_bytes()
    rows = [data[i * 16:(i + 1) * 16] for i in range(4)]
    assert rows[0] == rows[1] == rows[3] == b"\xEE" * 16
    assert rows[2] == b"\x00" * 6 + b"\xff\xff" + b"\x00" * 8

    # An identical frame writes nothing at all.
    with open(device, "r+b") as f:
        f.write(b"\xEE" * 64)
    sink.show(frame)
    assert Path(device).read_bytes() == b"\xEE" * 64
    sink.close()


def test_padded_stride(tmp_path):
    device = _fake_fb(tmp_path, 1, "fb_ili9486", size=(2, 2), stride=8)
    sink = FramebufferSink(device, sysfs_root=tmp_path / "sys")
    sink.show(Image.new("RGB", (2, 2), (255, 0, 0)))
    data = Path(device).read_bytes()
    assert data == (b"\x00\xf8" * 2 + b"\xAA" * 4) * 2
    sink.close()


class _Capture:
    def __init__(self, size):
        self.size = size
        self.frames = []

    def show(self, image):
        self.frames.append(image)

    def close(self):
        pass


def test_rotation_swaps_logical_size_and_rotates_clockwise():
    inner = _Capture((4, 2))  # landscape panel mounted as portrait
    sink = RotatedSink(inner, 90, None)
    assert sink.size == (2, 4)
    frame = Image.new("RGB", (2, 4), (0, 0, 0))
    frame.putpixel((0, 0), (255, 0, 0))  # logical top-left
    sink.show(frame)
    out = inner.frames[-1]
    assert out.size == (4, 2)
    assert out.getpixel((3, 0)) == (255, 0, 0)  # top-right after 90 deg clockwise


def test_smaller_logical_size_is_centred():
    inner = _Capture((6, 4))
    sink = RotatedSink(inner, 0, (2, 2))
    sink.show(Image.new("RGB", (2, 2), (255, 255, 255)))
    out = inner.frames[-1]
    assert out.getpixel((2, 1)) == (255, 255, 255)
    assert out.getpixel((0, 0)) == (0, 0, 0)
