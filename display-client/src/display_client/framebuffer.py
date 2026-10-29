"""Where rendered frames go.

The client draws into a Pillow image and writes raw pixels straight into a
Linux framebuffer device -- no X server, no SDL, no browser (spec section
4.4). That works the same whether the panel is driven by the legacy fbtft
driver (the Waveshare ``waveshare35c`` overlay binds ``fb_ili9486``) or by
a DRM tiny driver with fbdev emulation (e.g. ``piscreen,drm``): both expose
a plain ``/dev/fbN`` whose geometry is readable from sysfs.

Only the rows that changed since the previous frame are written. On an SPI
panel every byte written is clocked out over the bus (fbtft's deferred I/O
flushes the touched pages), so a progress bar that only moves a few rows
costs a few rows, not a 300 KB full-screen push.
"""

from __future__ import annotations

import logging
import time
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Optional, Tuple

from PIL import Image, ImageChops, ImageMath

logger = logging.getLogger("display_client.framebuffer")

SYSFS_GRAPHICS = Path("/sys/class/graphics")

# Framebuffers that are the HDMI/firmware console, never the SPI panel.
_NON_PANEL_NAMES = ("vc4drmfb", "simple", "bcm2708 fb", "bcm2835 fb")


class Sink(ABC):
    """A physical output surface of a fixed size."""

    size: Tuple[int, int]

    @abstractmethod
    def show(self, image: Image.Image) -> None:
        """Display ``image``, which must already be ``self.size``."""

    def close(self) -> None:
        pass


def pack_rgb565(image: Image.Image) -> bytes:
    """RGB image -> little-endian RGB565, the format 16 bpp SPI panels use.

    Pillow has no RGB565 raw packer, so the bit math runs through ImageMath
    (C loops, ~1 ms for 480x320) rather than per-pixel Python or numpy.
    """
    r, g, b = (channel.convert("I") for channel in image.convert("RGB").split())
    packed = ImageMath.lambda_eval(
        lambda a: ((a["r"] >> 3) << 11) | ((a["g"] >> 2) << 5) | (a["b"] >> 3),
        r=r,
        g=g,
        b=b,
    )
    return packed.convert("I;16").tobytes()


def pack_pixels(image: Image.Image, bits_per_pixel: int) -> bytes:
    if bits_per_pixel == 16:
        return pack_rgb565(image)
    if bits_per_pixel == 32:
        return image.convert("RGB").tobytes("raw", "BGRX")
    if bits_per_pixel == 24:
        return image.convert("RGB").tobytes("raw", "BGR")
    raise ValueError(f"unsupported framebuffer depth: {bits_per_pixel} bpp")


def find_framebuffer(sysfs_root: Path = SYSFS_GRAPHICS) -> Optional[str]:
    """The SPI panel's device node: the first framebuffer that isn't the
    HDMI console, else the first one at all (a Pi with KMS disabled, where
    the panel is fb0)."""
    candidates = sorted(
        (p for p in sysfs_root.glob("fb*") if p.name[2:].isdigit()),
        key=lambda p: int(p.name[2:]),
    )
    for entry in candidates:
        name = _read(entry / "name").lower()
        if not any(name.startswith(prefix) for prefix in _NON_PANEL_NAMES):
            return f"/dev/{entry.name}"
    return f"/dev/{candidates[0].name}" if candidates else None


def wait_for_framebuffer(
    timeout_s: float, sysfs_root: Path = SYSFS_GRAPHICS
) -> Optional[str]:
    """The panel overlay is loaded by the kernel well before this service
    starts, but the fbtft module can still be probing when it does."""
    deadline = time.monotonic() + timeout_s
    while True:
        device = find_framebuffer(sysfs_root)
        if device is not None or time.monotonic() >= deadline:
            return device
        time.sleep(0.25)


def _read(path: Path) -> str:
    try:
        return path.read_text().strip()
    except OSError:
        return ""


class FramebufferSink(Sink):
    def __init__(self, device: str, sysfs_root: Path = SYSFS_GRAPHICS) -> None:
        info = sysfs_root / Path(device).name
        width, height = (int(v) for v in _read(info / "virtual_size").split(","))
        self.bits_per_pixel = int(_read(info / "bits_per_pixel") or 16)
        self.stride = int(_read(info / "stride") or width * self.bits_per_pixel // 8)
        self.size = (width, height)
        self.device = device
        self._file = open(device, "r+b", buffering=0)
        self._previous: Optional[Image.Image] = None
        logger.info(
            "framebuffer %s (%s): %dx%d, %d bpp",
            device, _read(info / "name"), width, height, self.bits_per_pixel,
        )

    def show(self, image: Image.Image) -> None:
        image = image.convert("RGB")
        if self._previous is None:
            top, bottom = 0, self.size[1]
        else:
            bbox = ImageChops.difference(image, self._previous).getbbox()
            if bbox is None:
                return
            top, bottom = bbox[1], bbox[3]
        self._previous = image

        width = self.size[0]
        rows = pack_pixels(image.crop((0, top, width, bottom)), self.bits_per_pixel)
        row_bytes = width * self.bits_per_pixel // 8
        if row_bytes == self.stride:
            self._file.seek(top * self.stride)
            self._file.write(rows)
        else:
            for i in range(bottom - top):
                self._file.seek((top + i) * self.stride)
                self._file.write(rows[i * row_bytes:(i + 1) * row_bytes])

    def close(self) -> None:
        self._file.close()


class PngSink(Sink):
    """Writes every frame to ``DIR/frame.png`` -- for running the client on
    a dev machine with no panel attached."""

    def __init__(self, directory: Path, size: Tuple[int, int]) -> None:
        self.size = size
        self._path = directory / "frame.png"
        directory.mkdir(parents=True, exist_ok=True)

    def show(self, image: Image.Image) -> None:
        image.save(self._path)


class RotatedSink(Sink):
    """Presents a logical (possibly portrait/landscape-swapped) canvas on
    top of a physical sink. A logical size that doesn't match the panel is
    centred rather than scaled, so text stays crisp."""

    _TRANSPOSE = {
        90: Image.Transpose.ROTATE_270,  # Pillow's ROTATE_* are counter-clockwise
        180: Image.Transpose.ROTATE_180,
        270: Image.Transpose.ROTATE_90,
    }

    def __init__(self, inner: Sink, rotate: int, logical_size: Optional[Tuple[int, int]]) -> None:
        self._inner = inner
        self._rotate = rotate
        width, height = inner.size
        native = (height, width) if rotate in (90, 270) else (width, height)
        self.size = logical_size or native
        if self.size != native:
            logger.warning(
                "configured size %s does not match the panel (%s after rotation); centring",
                self.size, native,
            )
        self._native = native

    def show(self, image: Image.Image) -> None:
        if self.size != self._native:
            canvas = Image.new("RGB", self._native)
            canvas.paste(
                image,
                ((self._native[0] - self.size[0]) // 2, (self._native[1] - self.size[1]) // 2),
            )
            image = canvas
        if self._rotate:
            image = image.transpose(self._TRANSPOSE[self._rotate])
        self._inner.show(image)

    def close(self) -> None:
        self._inner.close()
