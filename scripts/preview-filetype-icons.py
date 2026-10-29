"""Contact sheets of the generated file-type icons, read back from the .ico files, on white and dark.

Run build-filetype-icons.py first; the icon list comes from its TYPES. Writes to build/filetype-icons-preview/.
"""
import importlib.util
import sys
from pathlib import Path

from PIL import Image

SCRIPTS = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("build_filetype_icons", SCRIPTS / "build-filetype-icons.py")
gen = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gen)

ICONS = gen.OUT
OUT = SCRIPTS.parent / "build" / "filetype-icons-preview"
SMALL = [s for s in gen.SIZES if s <= 64]


def stems_of(body, legacy=None):
    return [stem for stem, _, colour, b in gen.TYPES
            if b is body and (legacy is None or (colour == gen.LEGACY) == legacy)]


def frame(stem, size):
    ico = Image.open(ICONS / f"{stem}.ico")
    return ico.ico.getimage((size, size)).convert("RGBA")


def small_sheet(stems):
    gap = 6
    row_w = sum(SMALL) + gap * (len(SMALL) + 1)
    row_h = max(SMALL) + gap
    im = Image.new("RGBA", (2 * row_w, gap + len(stems) * row_h), (255, 255, 255, 255))
    im.paste((32, 32, 32, 255), (row_w, 0, 2 * row_w, im.height))
    for side in (0, row_w):
        for r, stem in enumerate(stems):
            x = side + gap
            for s in SMALL:
                im.alpha_composite(frame(stem, s), (x, gap + r * row_h + (max(SMALL) - s)))
                x += s + gap
    return im


def big_sheet(stems):
    gap = 8
    im = Image.new("RGBA", (gap + len(stems) * (256 + gap), 2 * (256 + gap) + gap), (255, 255, 255, 255))
    im.paste((32, 32, 32, 255), (0, 256 + 2 * gap - gap // 2, im.width, im.height))
    for row in range(2):
        for i, stem in enumerate(stems):
            im.alpha_composite(frame(stem, 256), (gap + i * (256 + gap), gap + row * (256 + gap)))
    return im


def zoom_sheet(stems, sizes=(24, 32, 40, 48), k=4):
    """The smallest sizes of the vector tier magnified, nearest-neighbour, to inspect pixel edges."""
    gap = 8
    row_w = gap + sum(s * k + gap for s in sizes)
    row_h = max(sizes) * k + gap
    im = Image.new("RGBA", (row_w, gap + 2 * len(stems) * row_h), (255, 255, 255, 255))
    im.paste((32, 32, 32, 255), (0, gap // 2 + len(stems) * row_h, row_w, im.height))
    for r, stem in enumerate(stems * 2):
        x = gap
        for s in sizes:
            im.alpha_composite(frame(stem, s).resize((s * k, s * k), Image.NEAREST), (x, gap + r * row_h))
            x += s * k + gap
    return im


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    small_sheet(stems_of(gen.PAPER)).save(OUT / "images.png")
    small_sheet(stems_of(gen.FILM)).save(OUT / "videos.png")
    small_sheet(stems_of(gen.CARD, legacy=False)).save(OUT / "raws.png")
    small_sheet(stems_of(gen.CARD, legacy=True)).save(OUT / "raws_legacy.png")
    zoom_sheet(["jpg", "mp4", "cr3", "nefx"]).save(OUT / "zoom_24_to_48.png")
    big_sheet(["jpg", "mp4", "cr3", "nefx", "orf", "crw"]).save(OUT / "256.png")


if __name__ == "__main__":
    sys.exit(main())
