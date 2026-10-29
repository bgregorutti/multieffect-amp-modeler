"""Draws the three screens: boot progress, live state, shutting down.

Every size is a fraction of the canvas, so the same code fits the 480x320
and the 320x240 panel (spec section 4.4). Readability from standing height
on a stage is the priority: the preset is the largest thing on screen, and
bypass turns the whole frame red.
"""

from __future__ import annotations

from functools import lru_cache
from typing import List, Optional, Tuple

from PIL import Image, ImageDraw, ImageFont

from .boot import BootStatus
from .view import LiveView

BG = (13, 15, 18)
PANEL = (28, 32, 38)
TEXT = (242, 242, 242)
DIM = (128, 136, 146)
ACCENT = (255, 180, 0)
OK = (61, 220, 132)
BAD = (255, 77, 77)
CHIP_OFF = (58, 65, 75)

_FONT_FILES = {
    True: ("DejaVuSans-Bold.ttf", "Arial Bold.ttf"),
    False: ("DejaVuSans.ttf", "Arial.ttf"),
}

Font = ImageFont.FreeTypeFont


@lru_cache(maxsize=64)
def font(size: int, bold: bool = True) -> Font:
    # fonts-dejavu-core on the Pi (installed by deploy/install.sh); Pillow's
    # bundled scalable font otherwise, so a dev machine needs nothing extra.
    for name in _FONT_FILES[bold]:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default(size)


def text_width(draw: ImageDraw.ImageDraw, text: str, f: Font) -> float:
    return draw.textlength(text, font=f)


def ellipsize(draw: ImageDraw.ImageDraw, text: str, f: Font, max_w: float) -> str:
    if text_width(draw, text, f) <= max_w:
        return text
    while text and text_width(draw, text + "…", f) > max_w:
        text = text[:-1]
    return text.rstrip() + "…"


def fit_line(
    draw: ImageDraw.ImageDraw, text: str, max_w: float, max_size: int, min_size: int,
    bold: bool = True,
) -> Tuple[Font, str]:
    """Largest font in [min_size, max_size] that fits ``text`` on one line,
    ellipsized at ``min_size`` if even that is too wide."""
    size = max_size
    while size > min_size and text_width(draw, text, font(size, bold)) > max_w:
        size -= 2
    f = font(max(size, min_size), bold)
    return f, ellipsize(draw, text, f, max_w)


def fit_two_lines(
    draw: ImageDraw.ImageDraw, text: str, max_w: float, max_size: int, min_size: int
) -> Tuple[Font, List[str]]:
    """One line if it fits at a comfortable size, else a word-wrapped pair."""
    comfortable = (max_size + min_size) // 2
    f, line = fit_line(draw, text, max_w, max_size, comfortable)
    if line == text:
        return f, [text]
    words = text.split()
    for size in range(comfortable, min_size - 1, -2):
        f = font(size)
        for split in range(len(words) - 1, 0, -1):
            first, second = " ".join(words[:split]), " ".join(words[split:])
            if text_width(draw, first, f) <= max_w:
                return f, [first, ellipsize(draw, second, f, max_w)]
    f = font(min_size)
    return f, [ellipsize(draw, text, f, max_w)]


class Renderer:
    def __init__(self, size: Tuple[int, int]) -> None:
        self.size = size
        self.w, self.h = size

    def _canvas(self) -> Tuple[Image.Image, ImageDraw.ImageDraw]:
        image = Image.new("RGB", self.size, BG)
        return image, ImageDraw.Draw(image)

    def _px(self, fraction: float) -> int:
        return max(1, round(self.h * fraction))

    def _centered(self, draw: ImageDraw.ImageDraw, y: float, text: str, f: Font, fill) -> None:
        draw.text((self.w / 2, y), text, font=f, fill=fill, anchor="mm")

    # -- boot ----------------------------------------------------------------

    def boot(self, status: BootStatus, fraction: float) -> Image.Image:
        image, draw = self._canvas()
        margin = self.w * 0.12
        self._centered(draw, self.h * 0.30, "AMP MODELER", font(self._px(0.11)), TEXT)

        bar_h = self._px(0.045)
        top = round(self.h * 0.52)
        left, right = round(margin), round(self.w - margin)
        draw.rounded_rectangle((left, top, right, top + bar_h), radius=bar_h // 2, fill=PANEL)
        fill_right = left + round((right - left) * min(1.0, max(0.0, fraction)))
        if fill_right - left >= bar_h:
            colour = BAD if status.error else ACCENT
            draw.rounded_rectangle((left, top, fill_right, top + bar_h), radius=bar_h // 2, fill=colour)

        label = status.label if status.error or fraction >= 1.0 else status.label + "…"
        f, label = fit_line(draw, label, right - left, self._px(0.065), self._px(0.045), bold=False)
        self._centered(draw, self.h * 0.66, label, f, BAD if status.error else DIM)

        y = self.h * 0.78
        for message, colour in ((status.warning, ACCENT), (status.error, BAD)):
            if message:
                f, message = fit_line(draw, message, self.w - 2 * self._px(0.05),
                                      self._px(0.055), self._px(0.04), bold=False)
                self._centered(draw, y, message, f, colour)
                y += self._px(0.08)
        return image

    # -- shutdown ------------------------------------------------------------

    def shutdown(self) -> Image.Image:
        image, draw = self._canvas()
        self._centered(draw, self.h * 0.44, "Shutting down…", font(self._px(0.11)), TEXT)
        self._centered(draw, self.h * 0.60, "Power cuts off automatically",
                       font(self._px(0.055), bold=False), DIM)
        return image

    # -- live state ----------------------------------------------------------

    def live(self, view: LiveView, connected: bool) -> Image.Image:
        image, draw = self._canvas()
        pad = self._px(0.04)
        header_h = self._px(0.17)
        chain_top, chain_bottom = round(self.h * 0.70), round(self.h * 0.85)

        # Header: rig position + name, then badges on the right.
        draw.rectangle((0, 0, self.w, header_h), fill=PANEL)
        right = self.w - pad
        status_text, status_colour = ("LIVE", OK) if connected else ("OFFLINE", BAD)
        f_badge = font(self._px(0.055))
        right -= text_width(draw, status_text, f_badge)
        draw.text((right, header_h / 2), status_text, font=f_badge, fill=status_colour, anchor="lm")
        dot = self._px(0.025)
        right -= dot * 2
        draw.ellipse((right, header_h / 2 - dot / 2, right + dot, header_h / 2 + dot / 2), fill=status_colour)
        right -= pad
        if view.bypass:
            pill_w = text_width(draw, "BYPASS", f_badge) + pad
            draw.rounded_rectangle((right - pill_w, header_h * 0.2, right, header_h * 0.8),
                                   radius=self._px(0.03), fill=BAD)
            draw.text((right - pill_w / 2, header_h / 2), "BYPASS", font=f_badge, fill=TEXT, anchor="mm")
            right -= pill_w + pad

        x = pad
        if not view.empty:
            rig_tag = f"RIG {view.rig_number}/{view.rig_count}"
            draw.text((x, header_h / 2), rig_tag, font=f_badge, fill=ACCENT, anchor="lm")
            x += text_width(draw, rig_tag, f_badge) + pad
        if view.rig_name:
            f, name = fit_line(draw, view.rig_name, right - x, self._px(0.085), self._px(0.05))
            draw.text((x, header_h / 2), name, font=f, fill=TEXT, anchor="lm")

        if view.empty:
            self._centered(draw, self.h * 0.45, "No rigs yet", font(self._px(0.1)), TEXT)
            self._centered(draw, self.h * 0.58, "Create one in the mobile app",
                           font(self._px(0.06), bold=False), DIM)
        else:
            self._preset(draw, view, header_h, chain_top, pad)
            self._chain(draw, view, chain_top, chain_bottom, pad)

        footer = None
        if not connected:
            footer = ("Reconnecting to control daemon…", BAD)
        elif view.engine_error:
            footer = (f"Engine: {view.engine_error}", BAD)
        if footer:
            f, text = fit_line(draw, footer[0], self.w - 2 * pad, self._px(0.055), self._px(0.04), bold=False)
            draw.text((pad, (chain_bottom + self.h) / 2), text, font=f, fill=footer[1], anchor="lm")

        if view.bypass:
            border = self._px(0.015)
            draw.rectangle((0, 0, self.w - 1, self.h - 1), outline=BAD, width=border)
        return image

    def _preset(self, draw: ImageDraw.ImageDraw, view: LiveView, top: int, bottom: int, pad: int) -> None:
        box = (bottom - top) - 2 * pad
        box_top = top + pad
        colour = DIM if view.bypass else ACCENT
        draw.rounded_rectangle((pad, box_top, pad + box, box_top + box), radius=self._px(0.04), fill=colour)
        number = str(view.preset_number) if view.preset_number else "–"
        f, number = fit_line(draw, number, box * 0.85, round(box * 0.75), round(box * 0.3))
        draw.text((pad + box / 2, box_top + box / 2), number, font=f, fill=BG, anchor="mm")

        x = pad * 2 + box
        max_w = self.w - x - pad
        name = view.preset_name or "No preset"
        f, lines = fit_two_lines(draw, name, max_w, self._px(0.16), self._px(0.075))
        line_h = f.size * 1.15
        sub_f = font(self._px(0.05), bold=False)
        block_h = line_h * len(lines) + sub_f.size * 1.6
        y = box_top + (box - block_h) / 2
        for line in lines:
            draw.text((x, y), line, font=f, fill=TEXT if view.preset_name else DIM, anchor="la")
            y += line_h
        if view.preset_count:
            draw.text((x, y + sub_f.size * 0.4), f"PRESET {view.preset_number}/{view.preset_count}",
                      font=sub_f, fill=DIM, anchor="la")

    def _chain(self, draw: ImageDraw.ImageDraw, view: LiveView, top: int, bottom: int, pad: int) -> None:
        chips = view.chain
        if not chips:
            return
        gap = self._px(0.015)
        width = (self.w - 2 * pad - gap * (len(chips) - 1)) / len(chips)
        radius = self._px(0.025)
        # One size for every chip (the largest the widest label allows), so
        # the strip reads as a row rather than a ransom note.
        size = round((bottom - top) * 0.45)
        widest = max(chips, key=lambda c: text_width(draw, c.label, font(size))).label
        f, _ = fit_line(draw, widest, width * 0.82, size, 6)
        for i, chip in enumerate(chips):
            left = pad + i * (width + gap)
            rect = (round(left), top, round(left + width), bottom)
            on = chip.enabled and not view.bypass
            if on:
                draw.rounded_rectangle(rect, radius=radius, fill=TEXT)
            else:
                draw.rounded_rectangle(rect, radius=radius, outline=CHIP_OFF, width=max(1, self._px(0.006)))
            label = ellipsize(draw, chip.label, f, width * 0.9)
            draw.text(((rect[0] + rect[2]) / 2, (top + bottom) / 2), label,
                      font=f, fill=BG if on else DIM, anchor="mm")


def render_screen(
    renderer: Renderer,
    *,
    shutting_down: bool,
    booting: bool,
    boot: Optional[BootStatus],
    boot_fraction: float,
    view: Optional[LiveView],
    connected: bool,
) -> Image.Image:
    if shutting_down:
        return renderer.shutdown()
    if booting or view is None:
        return renderer.boot(boot or BootStatus(boot_fraction, "Starting"), boot_fraction)
    return renderer.live(view, connected)
