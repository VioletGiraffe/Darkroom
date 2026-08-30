#!/usr/bin/env python3
"""Generates Quickroom's per-file-type shell icons: one .ico and one .svg per image format.

Outputs are committed and this needs Impact, a Windows font, so it is a dev-time tool - never a build step.

The wordmark is condensed past any font Windows ships (advance/cap 0.57 against Impact's 0.79 and Arial
Black's 1.0), so small sizes use a hand-drawn face and large sizes squeeze Impact horizontally to match.
The threshold is where an antialiased outline stops being crisp, around a 15px cap.
"""
import struct
import sys
from io import BytesIO
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.ttLib import TTFont

OUT = Path(__file__).resolve().parent.parent / "quickroom" / "res" / "filetypes"
IMPACT = Path("C:/Windows/Fonts/impact.ttf")

CREAM, INK = (240, 234, 224, 255), (36, 29, 21, 255)
CREAM_HEX, INK_HEX = "#f0eae0", "#241d15"

TYPES = [("jpg", "JPG", "#e02b17"), ("png", "PNG", "#d926cf"), ("webp", "WEBP", "#f28500"),
         ("tiff", "TIF", "#0c1f63"), ("gif", "GIF", "#37784a"), ("bmp", "BMP", "#1a75c2")]
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

VECTOR_FROM = 40      # at or above this the wordmark comes from Impact, below it from the hand-drawn face
PIXEL_MARK_AT = 16    # only the smallest size uses the bitmap landscape
BMP_UPTO = 24         # largest size stored uncompressed; see write_ico

# Geometry as fractions of the icon, taken from the hand-tuned 16px design.
BODY_X, BODY_W = 3 / 16, 10 / 16
BAND_Y, BAND_H = 6 / 16, 9 / 16
CAP_OF_BAND = 7 / 9
TEXT_W = 15 / 16     # wordmark width budget; a fixed pixel margin would
                     # make the squeeze size-dependent and the design non-scalable

SS = 4                # supersample; chrome lands on exact pixel boundaries so it survives the downsample

FACE = {
    "J": ["...#", "...#", "...#", "...#", "...#", "#..#", ".##."],
    "P": ["###.", "#..#", "#..#", "###.", "#...", "#...", "#..."],
    "G": [".##.", "#..#", "#...", "#.##", "#..#", "#..#", ".##."],
    "N": ["#..#", "##.#", "##.#", "#.##", "#.##", "#..#", "#..#"],
    "B": ["###.", "#..#", "#..#", "###.", "#..#", "#..#", "###."],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "T": ["###", ".#.", ".#.", ".#.", ".#.", ".#.", ".#."],
    "I": ["#", "#", "#", "#", "#", "#", "#"],
    "F": ["####", "#...", "#...", "###.", "#...", "#...", "#..."],
}
# 4-letter labels get ~3px per glyph once gaps are paid for. W keeps 4: the lower-half diagonal is the
# only thing separating it from a U at this size.
FACE_CONDENSED = {
    "W": ["#..#", "#..#", "#..#", "#..#", "#.##", "##.#", "#..#"],
    "E": ["###", "#..", "#..", "##.", "#..", "#..", "###"],
    "B": ["##.", "#.#", "#.#", "##.", "#.#", "#.#", "##."],
    "P": ["##.", "#.#", "#.#", "##.", "#..", "#..", "#.."],
}
MARK_PX = [".......##.", ".......##.", "....#.....", "..#####...", ".########."]

# Landscape normalised to a unit-width box, y down. Sun sits clear of the ridge: at icon sizes any
# contact reads as one cloud-like mass.
MARK_ASPECT = 0.6012                                    # height per unit width
MARK_SUN = (0.8690, 0.1131, 0.1131)                     # cx, cy, r
MARK_RIDGE = [(0.0, 0.6012), (0.2976, 0.1845), (0.4702, 0.4286), (0.6607, 0.2381), (1.0, 0.6012)]


def hx(colour):
    return tuple(int(colour[i:i + 2], 16) for i in (1, 3, 5)) + (255,)


def geom(size):
    band_h = round(BAND_H * size)
    return dict(bx=round(BODY_X * size), bw=round(BODY_W * size),
                band_y=round(BAND_Y * size), band_h=band_h, cap=round(band_h * CAP_OF_BAND))


def mark_box(size, g):
    """The landscape box above the band, fitted to preserve aspect so the sun stays circular."""
    pad_x, pad_y = 0.03 * g["bw"], 0.04 * size
    x0, x1 = g["bx"] + pad_x, g["bx"] + g["bw"] - pad_x
    y0, y1 = pad_y, g["band_y"] - pad_y
    avail_w, avail_h = x1 - x0, y1 - y0
    if avail_w * MARK_ASPECT <= avail_h:
        w = avail_w
    else:
        w = avail_h / MARK_ASPECT
    h = w * MARK_ASPECT
    return x0 + (avail_w - w) / 2, y0 + (avail_h - h) / 2, w


_font = TTFont(IMPACT)
_glyphs = _font.getGlyphSet()
_cmap = _font.getBestCmap()
_upm = _font["head"].unitsPerEm
_hmtx = _font["hmtx"]
_cap_units = getattr(_font["OS/2"], "sCapHeight", 0) or _font["head"].yMax


def _advances(label):
    return [_hmtx[_cmap[ord(c)]][0] for c in label]


def impact_fit(label, cap, avail):
    """Uniform scale set by cap height, then the horizontal squeeze needed to fit the band width."""
    s = cap / _cap_units
    natural = sum(_advances(label)) * s
    return s, (min(1.0, avail / natural) if natural else 1.0)


def render(label, colour, size):
    k = SS
    im = Image.new("RGBA", (size * k, size * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    g = geom(size)

    d.rectangle([g["bx"] * k, 0, (g["bx"] + g["bw"]) * k - 1, size * k - 1], fill=hx(colour))

    if size == PIXEL_MARK_AT:
        cell = g["bw"] * k / 10
        for r, row in enumerate(MARK_PX):
            for i, ch in enumerate(row):
                if ch == "#":
                    x0 = g["bx"] * k + i * cell
                    d.rectangle([x0, r * cell, x0 + cell - 1, (r + 1) * cell - 1], fill=CREAM)
    else:
        mx, my, mw = mark_box(size, g)
        mx, my, mw = mx * k, my * k, mw * k
        d.polygon([(mx + a * mw, my + b * mw) for a, b in MARK_RIDGE], fill=CREAM)
        cx, cy, r = MARK_SUN
        d.ellipse([mx + (cx - r) * mw, my + (cy - r) * mw,
                   mx + (cx + r) * mw, my + (cy + r) * mw], fill=CREAM)

    d.rectangle([0, g["band_y"] * k, size * k - 1, (g["band_y"] + g["band_h"]) * k - 1], fill=INK)

    if size < VECTOR_FROM:
        face = FACE_CONDENSED if len(label) == 4 else FACE
        glyphs = [face[c] for c in label]
        raw = sum(len(x[0]) for x in glyphs)
        gaps = len(glyphs) - 1
        gap = 2 if raw + 2 * gaps <= 15 else 1
        unit = max(1, g["cap"] // 7) * k          # integer scaling only, or the face stops being crisp
        # Both offsets snap to the supersample grid: a half-pixel origin blurs every glyph edge.
        x = (size * k - (raw + gap * gaps) * unit) // 2 // k * k
        top = (g["band_y"] * k + (g["band_h"] * k - 7 * unit) // 2) // k * k
        for glyph in glyphs:
            for r in range(7):
                for i, ch in enumerate(glyph[r]):
                    if ch == "#":
                        d.rectangle([x + i * unit, top + r * unit,
                                     x + (i + 1) * unit - 1, top + (r + 1) * unit - 1], fill=CREAM)
            x += (len(glyph[0]) + gap) * unit
    else:
        cap = g["cap"] * k
        _, sx = impact_fit(label, cap, TEXT_W * size * k)
        font = ImageFont.truetype(str(IMPACT), max(6, round(cap * _upm / _cap_units)))
        mask = font.getmask(label, mode="L")
        w, h = mask.size
        text = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        tp = text.load()
        for y in range(h):
            for x in range(w):
                v = mask.getpixel((x, y))
                if v:
                    tp[x, y] = CREAM[:3] + (v,)
        text = text.resize((max(1, round(w * sx)), h), Image.LANCZOS)
        im.alpha_composite(text, ((size * k - text.width) // 2,
                                  round(g["band_y"] * k + (g["band_h"] * k - h) / 2)))
    # BOX resolves the supersample by area: LANCZOS rings across block edges and greys the pixel face.
    return im.resize((size, size), Image.BOX)


def _bmp_entry(im):
    w, h = im.size
    px = im.load()
    out = bytearray(struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, 0, 0, 0, 0, 0))
    for y in range(h - 1, -1, -1):
        for x in range(w):
            r, g, b, a = px[x, y]
            out += bytes((b, g, r, a))
    stride = ((w + 31) // 32) * 4
    for y in range(h - 1, -1, -1):
        bits = bytearray(stride)
        for x in range(w):
            if px[x, y][3] == 0:
                bits[x // 8] |= 0x80 >> (x % 8)
        out += bits
    return bytes(out)


def write_ico(path, images):
    """BMP up to BMP_UPTO, PNG above it: an uncompressed entry costs ~16x its PNG and the exe embeds these."""
    blobs = []
    for im in images:
        if im.width > BMP_UPTO:
            buf = BytesIO()
            im.save(buf, "PNG")
            blobs.append(buf.getvalue())
        else:
            blobs.append(_bmp_entry(im))
    header = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for im, blob in zip(images, blobs):
        header += struct.pack("<BBBBHHII", im.width & 0xFF, im.height & 0xFF, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    path.write_bytes(bytes(header) + b"".join(blobs))


def write_svg(path, label, colour):
    """The vector tier only: the pixel sizes cannot be expressed as one scalable drawing."""
    size = 512.0
    g = geom(size)
    cap = g["band_h"] * CAP_OF_BAND
    s, sx = impact_fit(label, cap, TEXT_W * size)
    x = (size - sum(_advances(label)) * s * sx) / 2
    baseline = g["band_y"] + g["band_h"] / 2 + cap / 2
    paths = []
    for ch in label:
        pen = SVGPathPen(_glyphs)
        _glyphs[_cmap[ord(ch)]].draw(pen)
        commands = pen.getCommands()
        if commands:
            paths.append(f'<path transform="translate({x:.2f},{baseline:.2f}) '
                         f'scale({s * sx:.5f},{-s:.5f})" d="{commands}"/>')
        x += _hmtx[_cmap[ord(ch)]][0] * s * sx

    mx, my, mw = mark_box(size, g)
    ridge = " ".join(f"{mx + a * mw:.1f},{my + b * mw:.1f}" for a, b in MARK_RIDGE)
    cx, cy, r = MARK_SUN
    path.write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512">\n'
        f'  <rect x="{g["bx"]}" y="0" width="{g["bw"]}" height="512" fill="{colour}"/>\n'
        f'  <g fill="{CREAM_HEX}">\n'
        f'    <polygon points="{ridge}"/>\n'
        f'    <circle cx="{mx + cx * mw:.1f}" cy="{my + cy * mw:.1f}" r="{r * mw:.1f}"/>\n'
        f'  </g>\n'
        f'  <rect x="0" y="{g["band_y"]}" width="512" height="{g["band_h"]}" fill="{INK_HEX}"/>\n'
        f'  <g fill="{CREAM_HEX}">\n    ' + "\n    ".join(paths) + '\n  </g>\n</svg>\n',
        encoding="utf-8")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for stem, label, colour in TYPES:
        images = [render(label, colour, size) for size in SIZES]
        write_ico(OUT / f"{stem}.ico", images)
        write_svg(OUT / f"{stem}.svg", label, colour)
        print(f"{stem:5} {label:5} {colour}  {(OUT / f'{stem}.ico').stat().st_size:>7} B"
              f"  {len(SIZES)} sizes")


if __name__ == "__main__":
    sys.exit(main())
