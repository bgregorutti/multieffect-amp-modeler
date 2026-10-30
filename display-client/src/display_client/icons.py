"""Component images for the chain strip, scaled to fit and cached.

The PNGs are rendered from control-daemon/data/img/*.svg by
scripts/render_icons.sh (Pillow cannot read SVG). A missing file is not an
error: the strip falls back to the block's text chip.
"""

from __future__ import annotations

from functools import lru_cache
from pathlib import Path
from typing import Optional, Tuple

from PIL import Image, ImageEnhance

ICON_DIR = Path(__file__).parent / "icons"


@lru_cache(maxsize=None)
def _source(name: str) -> Optional[Image.Image]:
    path = ICON_DIR / f"{name}.png"
    if not path.is_file():
        return None
    return Image.open(path).convert("RGBA")


@lru_cache(maxsize=128)
def icon(name: str, max_w: int, max_h: int, dimmed: bool, bg: Tuple[int, int, int]) -> Optional[Image.Image]:
    """``name`` scaled to fit ``max_w`` x ``max_h``, aspect kept. ``dimmed``
    washes it out towards ``bg``, for a block that is switched off."""
    src = _source(name)
    if src is None or max_w < 1 or max_h < 1:
        return None
    scale = min(max_w / src.width, max_h / src.height)
    size = (max(1, round(src.width * scale)), max(1, round(src.height * scale)))
    image = src.resize(size, Image.LANCZOS)
    if dimmed:
        alpha = image.getchannel("A")
        faded = ImageEnhance.Color(image.convert("RGB")).enhance(0.1)
        faded = Image.blend(faded, Image.new("RGB", size, bg), 0.65)
        image = faded.convert("RGBA")
        image.putalpha(alpha)
    return image
