#!/usr/bin/env python3
"""Generates Quickroom's per-file-type shell icons: one .ico and one .svg per image format.

Windows draws these wherever Quickroom owns an image type, so a file's format has to be readable at a glance
in a crowded list. One design in six colours carries that: a coloured document body, a cream landscape, and
an ink band across it holding the extension as a condensed wordmark.

What the script is arranged around:
  16px is the critical size. The design was hand-tuned there and every other size derives from it by
    fraction; where a fraction and the 16px grid disagree, 16px wins.
  A flat edge at half coverage reads as a grey line, so terminals land on whole pixels at every size.
  Every label fits its band with a margin. 4-letter labels only manage that on glyphs of their own.

Outputs are committed and this needs Impact, a Windows font, so it is a dev-time tool - never a build step.

Two tiers, split at VECTOR_FROM:
  Below it the wordmark is hand-drawn in FACES, a pixel per cell, so no edge is ever antialiased.
  At and above it the wordmark is Impact. The split sits at 32 because the 7-row face would have to double
    there, which doubles its stroke weight and leaves WEBP flush.

Impact needs three corrections before it can serve as the wordmark:
  It is wider than the design, which wants advance/cap 0.57 against Impact's 0.79 and Arial Black's 1.0, so
    impact_fit squeezes it horizontally.
  It is heavier than the design, so erosion pulls every contour inward to a target stem weight.
  Nothing hints an outline at these sizes, so grid_fit aligns the cap line and baseline by hand.
"""
import struct
import sys
from io import BytesIO
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

OUT = Path(__file__).resolve().parent.parent / "quickroom" / "res" / "filetypes"
IMPACT = Path("C:/Windows/Fonts/impact.ttf")

CREAM, INK = (240, 234, 224, 255), (36, 29, 21, 255)
CREAM_HEX, INK_HEX = "#f0eae0", "#241d15"

TYPES = [("jpg", "JPG", "#e02b17"), ("png", "PNG", "#d926cf"), ("webp", "WEBP", "#f28500"),
         ("tiff", "TIF", "#0c1f63"), ("gif", "GIF", "#37784a"), ("bmp", "BMP", "#1a75c2")]
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

VECTOR_FROM = 32      # at or above this the wordmark comes from Impact, below it from the hand-drawn face
PIXEL_MARK_AT = 16    # only the smallest size uses the bitmap landscape
BMP_UPTO = 24         # largest size stored uncompressed; see write_ico

# Geometry as fractions of the icon, taken from the hand-tuned 16px design.
BODY_X, BODY_W = 3 / 16, 10 / 16
BAND_Y = 6 / 16
BAND_H = 8.6 / 16    # rounds to 9 at 16px, the floor: a 7-row face plus one ink row above and below it
CAP_OF_BAND = 7 / 9  # vector tier only; below VECTOR_FROM the cap is set by pick_face
TEXT_W = 15 / 16     # wordmark width budget; a fixed pixel margin would
                     # make the squeeze size-dependent and the design non-scalable

# Vertical stem as a fraction of cap, after erosion. 0.21 sits between Impact's 0.26 and the pixel tier;
# 4-letter labels are squeezed together, so they carry less weight to read at the same density.
STEM_TARGET, STEM_TARGET_CONDENSED = 0.21, 0.17

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
# A second face, 11 rows with 2px strokes. The 24px budget is 11 rows, where the 7-row face fits only at
# scale 1 and fills half the band.
FACE_11 = {
    "J": ["....##", "....##", "....##", "....##", "....##", "....##", "....##", "##..##", "##..##", "######", ".####."],
    "P": ["#####.", "######", "##..##", "##..##", "######", "#####.", "##....", "##....", "##....", "##....", "##...."],
    "G": [".####.", "######", "##..##", "##....", "##....", "##.###", "##.###", "##..##", "##..##", "######", ".####."],
    "N": ["##..##", "###.##", "###.##", "###.##", "###.##", "##.###", "##.###", "##.###", "##.###", "##..##", "##..##"],
    "B": ["#####.", "######", "##..##", "##..##", "######", "#####.", "##..##", "##..##", "##..##", "######", "#####."],
    "M": ["##...##", "###.###", "#######", "##.#.##", "##...##", "##...##", "##...##", "##...##", "##...##", "##...##", "##...##"],
    "T": ["######", "######", "..##..", "..##..", "..##..", "..##..", "..##..", "..##..", "..##..", "..##..", "..##.."],
    "I": ["##", "##", "##", "##", "##", "##", "##", "##", "##", "##", "##"],
    "F": ["######", "######", "##....", "##....", "#####.", "#####.", "##....", "##....", "##....", "##....", "##...."],
}
# E and P give up a column: at 24 the budget is 22 and the gap is 1, so the four glyphs must fit 19.
# B keeps its width, since two 1px counters stop reading as a B.
FACE_11_CONDENSED = {
    "W": ["##..##", "##..##", "##..##", "##..##", "##..##", "##..##", "##.###", "##.###", "###.##", "###.##", "##..##"],
    "E": ["####", "####", "##..", "##..", "###.", "###.", "##..", "##..", "##..", "####", "####"],
    "B": ["####.", "#####", "##..#", "##..#", "####.", "####.", "##..#", "##..#", "##..#", "#####", "####."],
    "P": ["###.", "####", "##.#", "##.#", "##.#", "####", "###.", "##..", "##..", "##..", "##.."],
}
FACES = {7: (FACE, FACE_CONDENSED), 11: (FACE_11, FACE_11_CONDENSED)}

# The 16px landscape, on a grid of tenths of the body width.
MARK_PX = [".......##.", ".......##.", "....#.....", "..#####...", ".########."]

# Landscape normalised to a unit-width box, y down. Sun sits clear of the ridge: at icon sizes any
# contact reads as one cloud-like mass.
MARK_ASPECT = 0.6012                                    # height per unit width
MARK_SUN = (0.8690, 0.1131, 0.1131)                     # cx, cy, r
MARK_RIDGE = [(0.0, 0.6012), (0.2976, 0.1845), (0.4702, 0.4286), (0.6607, 0.2381), (1.0, 0.6012)]


def hx(colour):
    return tuple(int(colour[i:i + 2], 16) for i in (1, 3, 5)) + (255,)


def geom(size):
    """Pixel geometry for one size. Rounded per size rather than scaled from a single drawing: that is what
    keeps the band and body edges on whole pixels, and what the .svg cannot reproduce when scaled down."""
    return dict(bx=round(BODY_X * size), bw=round(BODY_W * size),
                band_y=round(BAND_Y * size), band_h=round(BAND_H * size))


def pick_face(band_h):
    """Tallest face at whole-number scale that keeps an ink row above and below: a fractional scale blurs it."""
    budget = band_h - 2
    return max(((rows, budget // rows) for rows in FACES if budget >= rows), key=lambda f: f[0] * f[1])


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


def _impact_stem_of_cap():
    """Impact's vertical stem as a fraction of cap height, measured off an I rendered at 400px cap."""
    ref = 400
    mask = ImageFont.truetype(str(IMPACT), round(ref * _upm / _cap_units)).getmask("I", mode="1")
    w, h = mask.size
    lit = [x for x in range(w) if mask.getpixel((x, h // 2))]
    return (lit[-1] - lit[0] + 1) / ref


STEM_OF_CAP = _impact_stem_of_cap()


def erosion(label, cap, sx):
    """Inward contour offset that brings the stem to its target; the squeeze has already thinned it by sx."""
    target = STEM_TARGET_CONDENSED if len(label) == 4 else STEM_TARGET
    return cap * (STEM_OF_CAP * sx - target) / 2


def cap_zone(text):
    """Cap line and baseline of the flat terminals, in rows. Median over the tallest columns: round letters
    overshoot both, so the ink bounding box lies outside the zone the flat terminals share."""
    a = np.asarray(text.split()[3], float) / 255
    sums = a.sum(axis=0)
    tops, bottoms = [], []
    for c in np.nonzero(sums > 0.9 * sums.max())[0]:
        lit = np.nonzero(a[:, c])[0]
        # The outermost lit row is partly ink, so its coverage is how far into that row the edge sits.
        tops.append(lit[0] + 1.0 - a[lit[0], c])
        bottoms.append(lit[-1] + a[lit[-1], c])
    return float(np.median(tops)), float(np.median(bottoms))


def grid_fit(text, band_y, band_h):
    """Scales and places the wordmark so cap line and baseline land on device pixel edges, and returns its
    row. Every glyph shares those two edges, so leaving them fractional greys out both terminals at once."""
    top, bottom = cap_zone(text)
    want = max(SS, round((bottom - top) / SS) * SS)
    # BILINEAR: the scale here is a fraction of a percent, and LANCZOS rings enough at that to move the edge.
    text = text.resize((text.width, max(1, round(text.height * want / (bottom - top)))), Image.BILINEAR)
    top, bottom = cap_zone(text)                    # re-measured: the resize rounds to whole rows
    centred = band_y * SS + (band_h * SS - (bottom - top)) / 2
    want_top = round(centred / SS) * SS
    return text, round(want_top - top)


def erode(im, r):
    """Moves every contour inward by r pixels, centrelines fixed: grey erosion by a disc."""
    if r < 1:
        return im
    a = np.asarray(im.split()[3])
    padded = np.pad(a, r)
    out = np.full(a.shape, 255, np.uint8)
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy <= r * r:
                np.minimum(out, padded[r + dy:r + dy + a.shape[0], r + dx:r + dx + a.shape[1]], out=out)
    return Image.merge("RGBA", im.split()[:3] + (Image.fromarray(out),))


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
        rows, scale = pick_face(g["band_h"])
        normal, condensed = FACES[rows]
        face = condensed if len(label) == 4 else normal
        glyphs = [face[c] for c in label]
        raw = sum(len(x[0]) for x in glyphs)
        gaps = len(glyphs) - 1
        gap = 2 if (raw + 2 * gaps) * scale <= int(TEXT_W * size) else 1   # floor: the budget is a maximum
        unit = scale * k
        # Both offsets snap to the supersample grid: a half-pixel origin blurs every glyph edge.
        x = (size * k - (raw + gap * gaps) * unit) // 2 // k * k
        top = (g["band_y"] * k + (g["band_h"] * k - rows * unit) // 2) // k * k
        for glyph in glyphs:
            for r in range(rows):
                for i, ch in enumerate(glyph[r]):
                    if ch == "#":
                        d.rectangle([x + i * unit, top + r * unit,
                                     x + (i + 1) * unit - 1, top + (r + 1) * unit - 1], fill=CREAM)
            x += (len(glyph[0]) + gap) * unit
    else:
        cap = round(g["band_h"] * CAP_OF_BAND) * k
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
        text = erode(text, round(erosion(label, cap, sx)))   # after the squeeze, so the inset matches in x and y
        text, row = grid_fit(text, g["band_y"], g["band_h"])
        im.alpha_composite(text, ((size * k - text.width) // 2, row))
    # BOX resolves the supersample by area: LANCZOS rings across block edges and greys the pixel face.
    return im.resize((size, size), Image.BOX)


def _bmp_entry(im):
    """One uncompressed ICO entry: BITMAPINFOHEADER, bottom-up BGRA rows, then a 1bpp transparency mask
    padded to 4-byte rows. The declared height covers both sets of rows, hence h * 2."""
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
    """BMP at and below BMP_UPTO, PNG above it. Both are valid at any size here; PNG compresses."""
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
        # A size of 256 is stored as 0: the field is one byte.
        header += struct.pack("<BBBBHHII", im.width & 0xFF, im.height & 0xFF, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    path.write_bytes(bytes(header) + b"".join(blobs))


def write_svg(path, label, colour):
    """The vector tier only: the pixel sizes cannot be expressed as one scalable drawing.

    Not grid-fitted either, one drawing having no target grid, so at small sizes this will not match the .ico.
    """
    size = 512.0
    g = geom(size)
    cap = g["band_h"] * CAP_OF_BAND
    s, sx = impact_fit(label, cap, TEXT_W * size)
    x = (size - sum(_advances(label)) * s * sx) / 2
    baseline = g["band_y"] + g["band_h"] / 2 + cap / 2
    paths = []
    for ch in label:
        pen = SVGPathPen(_glyphs)
        # Baked into the path data rather than a transform attribute: the stroke below must stay circular.
        _glyphs[_cmap[ord(ch)]].draw(TransformPen(pen, (s * sx, 0, 0, -s, x, baseline)))
        commands = pen.getCommands()
        if commands:
            paths.append(f'<path d="{commands}"/>')
        x += _hmtx[_cmap[ord(ch)]][0] * s * sx

    # A stroke is centred on the path, so half of it eats into the glyph: the same inward offset erode makes.
    # The outer half lands on the band, which the wordmark never leaves.
    inset = erosion(label, cap, sx)
    ink_pen = (f' stroke="{INK_HEX}" stroke-width="{2 * inset:.2f}" stroke-linejoin="round"'
               if inset > 0 else "")

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
        f'  <g fill="{CREAM_HEX}"{ink_pen}>\n    ' + "\n    ".join(paths) + '\n  </g>\n</svg>\n',
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
