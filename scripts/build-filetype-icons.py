#!/usr/bin/env python3
"""Generates Quickroom's per-file-type shell icons: one .ico and one .svg per image and video format.

Windows draws these wherever Quickroom owns a type, so a file's format has to be readable at a glance in a
crowded list. One design carries that: a coloured body, a cream landscape, and an ink band across it holding
the extension as a condensed wordmark. Two bodies distinguish the families - PAPER for images, and FILM,
which is wider and carries a sprocket run down both edges. Each entry in TYPES names the body it uses.

What the script is arranged around:
  16px is the critical size. The design was hand-tuned there and every other size derives from it by
    fraction; where a fraction and the 16px grid disagree, 16px wins.
  A flat edge at half coverage reads as a grey line, so terminals land on whole pixels at every size.
  Every label fits its band with a margin. 4-letter labels only manage that on glyphs of their own.

Outputs are committed and this needs Bahnschrift, a Windows font, so it is a dev-time tool - never a build
step.

Two tiers, split at VECTOR_FROM:
  Below it the wordmark is hand-drawn in GLYPHS, a pixel per cell, so no edge is ever antialiased.
  At and above it the wordmark is Bahnschrift Bold Condensed. The split sits at 24 because that is the
    first size the hand-drawn face cannot serve: it needs a taller face, whose 2px stems cost columns a
    4-letter label does not have, where the font squeezes to the band and pays in weight instead.

The face needs three corrections before it can serve as the wordmark:
  It is wider than the design, which wants advance/cap 0.57 against Bahnschrift's 0.68 and Arial Black's 1.0,
    so wordmark_fit squeezes it horizontally.
  A face heavier than STEM_TARGET has erosion pull its contours inward to that weight; Bahnschrift is
    lighter, so erosion is inert for it.
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
from fontTools.varLib import instancer

OUT = Path(__file__).resolve().parent.parent / "quickroom" / "res" / "filetypes"
WORDMARK_FONT = Path("C:/Windows/Fonts/bahnschrift.ttf")
WORDMARK_AXES = {"wght": 700, "wdth": 75}   # the "Bold Condensed" instance; both axes are at their extreme

CREAM, INK = (240, 234, 224, 255), (36, 29, 21, 255)
CREAM_HEX, INK_HEX = "#f0eae0", "#241d15"

SIZES = [16, 20, 24, 32, 40, 48, 60, 64, 256]

VECTOR_FROM = 24      # at or above this the wordmark comes from the font, below it from the hand-drawn face
PIXEL_MARK_AT = 16    # only the smallest size uses the bitmap landscape
BMP_UPTO = 48         # largest size stored uncompressed; see write_ico

# Geometry as fractions of the icon, taken from the hand-tuned 16px design.
BODY_X, BODY_W = 3 / 16, 10 / 16
BODY_X_FILM = 1 / 16   # the film body is wider, so the band overhangs it by 1px rather than 3
BAND_Y = 6 / 16
BAND_H = 8.6 / 16    # rounds to 9 at 16px, the floor: a 7-row face plus one ink row above and below it
CAP_OF_BAND = 7 / 9  # vector tier only; below VECTOR_FROM the cap is FACE_ROWS
TEXT_W = 15 / 16     # wordmark width budget; a fixed pixel margin would
                     # make the squeeze size-dependent and the design non-scalable

# Ceiling on the vertical stem as a fraction of cap: erosion only thins, so a face below it keeps its own.
# Bahnschrift Bold Condensed is 0.17 natural, so both targets are inert until a heavier face replaces it.
# 4-letter labels are squeezed together, so they carry less weight to read at the same density.
STEM_TARGET, STEM_TARGET_CONDENSED = 0.21, 0.17

SS = 4                # supersample; chrome lands on exact pixel boundaries so it survives the downsample

# Sprocket run for the film body: a hole count, and a duty cycle spending that share of each pitch on a hole.
# The run divides the space above the band, so it fills that space at any count and any size. The two tiers
# need separate rules: the pixel run is packed at a 1px gap, which scaled up leaves three 16px holes 1px apart
# at 256, while an even pitch overflows the 9px run at 24.
# Three is the ceiling on the count: 32 is the even-pitch tier's smallest size and its run above the band is
# 12px, so a pitch of 4 is the finest that clears a 2px hole off both ends. Four lands one in the top corner.
# Holes stay square: a rounded corner greys at 32-48, where every other edge in the design is a whole pixel.
SPROCKET = dict(holes=3, duty=0.6, rim=1 / 32)
# Its own split, not VECTOR_FROM: the run above the band has to be about 12px for an even pitch to clear a
# hole off both ends, which 32 is the first size to give. At 24 the run is 9px and the offsets land on .5,
# where rounding drops a hole in the top corner and merges the other two into one slot.
SPROCKET_FROM = 32
HOLES_ABOVE = 3       # per side, below SPROCKET_FROM

# w=None means the width is derived so the body stays symmetric; see geom.
PAPER = dict(x=BODY_X, w=BODY_W, sprocket=None)
FILM = dict(x=BODY_X_FILM, w=None, sprocket=SPROCKET)

# m4v -> MP4, mpeg -> MPG and m2ts/mts/ts -> MTS, so the 13 supported video suffixes need nine icons.
# WEBP's and SVG's colours are too light for cream sprockets to show: paper bodies only.
# Indigo, purple and blue are the closest three, so only one of them (MP4) is on a common video format.
TYPES = [("jpg", "JPG", "#c10500", PAPER), ("png", "PNG", "#c70ea9", PAPER),
         ("webp", "WEBP", "#decb00", PAPER), ("tiff", "TIF", "#210641", PAPER),
         ("gif", "GIF", "#2ba100", PAPER), ("bmp", "BMP", "#3e507a", PAPER),
         ("svg", "SVG", "#00e796", PAPER),
         ("mp4", "MP4", "#004dc4", FILM), ("mov", "MOV", "#0096b6", FILM),
         ("avi", "AVI", "#002120", FILM), ("mkv", "MKV", "#de4600", FILM),
         ("webm", "WEBM", "#460314", FILM), ("wmv", "WMV", "#170089", FILM),
         ("mpg", "MPG", "#8131b4", FILM), ("mts", "MTS", "#7c1c1a", FILM),
         ("flv", "FLV", "#334a01", FILM)]

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
# Letters the video labels added; SVG shares S and V. V must come to a point: on a flat foot it reads as U.
FACE_FILM = {
    "O": [".##.", "#..#", "#..#", "#..#", "#..#", "#..#", ".##."],
    "S": [".###", "#...", "#...", ".##.", "...#", "...#", "###."],
    "V": ["#...#", "#...#", "#...#", "#...#", ".#.#.", ".#.#.", "..#.."],
    "A": [".##.", "#..#", "#..#", "####", "#..#", "#..#", "#..#"],
    "K": ["#..#", "#.#.", "##..", "##..", "#.#.", "#..#", "#..#"],
    "L": ["#..", "#..", "#..", "#..", "#..", "#..", "###"],
    "4": ["..#.", ".##.", "#.#.", "####", "..#.", "..#.", "..#."],
}
# Extra widths, drawn where a label needs a letter narrower than any face above offers.
# W5: outer walls full height, inner vee nested one row lower, feet inset off the baseline.
# V4 ends off-centre: four columns cannot reach a 1px point symmetrically, and every symmetric 4-wide V
#   reads as U, so the tail leans instead of sitting flat.
NARROW = {"W3": ["#.#", "#.#", "#.#", "###", "###", "###", "#.#"],
          "W5": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."],
          "E2": ["##", "#.", "#.", "##", "#.", "#.", "##"],
          "M3": ["#.#", "###", "###", "#.#", "#.#", "#.#", "#.#"],
          "V4": ["#..#", "#..#", "#..#", "#..#", "#..#", ".##.", ".#.."]}

FACE_ROWS = 7         # every hand-drawn glyph is this tall, at one pixel per cell


def _build_variants(*tables):
    """Re-keys the letter-indexed faces by <letter><width>; a letter present at two widths yields two keys."""
    out = {}
    for t in tables:
        for ch, grid in t.items():
            out[f"{ch}{len(grid[0])}"] = grid
    return out


# One flat table: only widths coexist, so where two shapes of one width were weighed the loser was dropped.
# NARROW is merged as-is, its keys already carrying the width.
GLYPHS = {**_build_variants(FACE, FACE_CONDENSED, FACE_FILM), **NARROW}

# Per-size glyph choices; an absent size, or an absent label, means the widest variant of every letter, which
# is what every 3-letter label uses at every size. Specs exist only where 15 columns force a judgement.
# MOV and MKV take V4 at 16: on V5 the run fills the canvas exactly, and M must not sit flush against the
# band edge. V touching the right edge is accepted - its outer column is short, so it already reads as space.
# SVG takes V4 at 16 to keep S off the band edge.
GLYPH_CHOICES = {"WEBP": {16: "W4 E2 B3 P3", 20: "W5 E2 B4 P4"},
                 "WEBM": {16: "W3 E2 B3 M3", 20: "W5 E2 B3 M5"},
                 "WMV": {16: "W5 M3 V5"},
                 "MOV": {16: "M5 O4 V4"},
                 "MKV": {16: "M5 K4 V4"},
                 "SVG": {16: "S4 V4 G4"}}

# The 16px landscape, on a grid of tenths of the body width.
MARK_PX = [".......##.", ".......##.", "....#.....", "..#####...", ".########."]

# Landscape normalised to a unit-width box, y down. Sun sits clear of the ridge: at icon sizes any
# contact reads as one cloud-like mass.
MARK_ASPECT = 0.6012                                    # height per unit width
MARK_SUN = (0.8690, 0.1131, 0.1131)                     # cx, cy, r
MARK_RIDGE = [(0.0, 0.6012), (0.2976, 0.1845), (0.4702, 0.4286), (0.6607, 0.2381), (1.0, 0.6012)]


def hx(colour):
    return tuple(int(colour[i:i + 2], 16) for i in (1, 3, 5)) + (255,)


def geom(size, body=PAPER):
    """Pixel geometry for one size. Rounded per size rather than scaled from a single drawing: that is what
    keeps the band and body edges on whole pixels, and what the .svg cannot reproduce when scaled down.

    A body with no width fraction derives one so both margins stay equal. Rounding the two independently
    works for the paper body but overflows the film one at 24, where 1/16 rounds up and 14/16 does not.
    """
    bx = round(body["x"] * size)
    return dict(bx=bx, bw=round(body["w"] * size) if body["w"] else size - 2 * bx,
                band_y=round(BAND_Y * size), band_h=round(BAND_H * size))


def resolve_glyphs(label, size):
    """Glyph grids for one label at one size. Raises rather than overflowing: a spec is hand-written data
    a later change to the width budget can silently invalidate."""
    spec = GLYPH_CHOICES.get(label, {}).get(size)
    if spec:
        keys = spec.split()
        assert len(keys) == len(label), f"{label}@{size}: spec names {len(keys)} glyphs, label has {len(label)}"
        for k, ch in zip(keys, label):
            assert k[0] == ch and k in GLYPHS, f"{label}@{size}: {k!r} is not a variant of {ch!r}"
    else:
        missing = [ch for ch in label if not any(k[0] == ch for k in GLYPHS)]
        assert not missing, f"{label}@{size}: the face has no {', '.join(missing)}"
        keys = [max((k for k in GLYPHS if k[0] == ch), key=lambda k: len(GLYPHS[k][0])) for ch in label]
    return [GLYPHS[k] for k in keys]


def wordmark_left(glyphs, total, size):
    """Left margin for the wordmark, centred by ink rather than by bounding box. An odd remaining pixel goes
    to the side whose outer column carries more ink: a full-height stem needs the clearance to keep from
    merging into the band edge, where a sparse edge (the 4, P's bowl, F's arm) already reads as space."""
    slack = size - total
    ink_l = sum(row[0] == "#" for row in glyphs[0])
    ink_r = sum(row[-1] == "#" for row in glyphs[-1])
    return (slack + 1) // 2 if ink_l > ink_r else slack // 2


def place(label, size, glyphs):
    """Gap, left offset and total width for a resolved glyph run, all in pixels."""
    raw = sum(len(g[0]) for g in glyphs)
    gaps = len(glyphs) - 1
    gap = 2 if raw + 2 * gaps <= int(TEXT_W * size) else 1   # floor: the budget is a maximum
    total = raw + gap * gaps
    assert total <= size, f"{label}@{size}: wordmark is {total}px on a {size}px canvas"
    return gap, wordmark_left(glyphs, total, size), total


def sprockets(size, g, spec):
    """Hole origins, hole size and rim for one icon size.

    Never in a corner, so the run reads as strip, not as a cut edge. The rim is body colour outboard of every
    hole: a hole never opens onto the background, so the silhouette is the same on a light or dark desktop.
    No holes below the band: the run there is one or two rows, too short to clear the bottom edge.
    """
    rim = max(1, round(spec["rim"] * size))
    if size < SPROCKET_FROM:
        # A constant pitch would give 3, 4 and 2 holes at 16, 20 and 24: the hole doubles at 24 but the run
        # above the band does not. Three per side at every size instead, packed at the minimum gap.
        perf = max(1, round(size / 16))
        free = g["band_y"] - HOLES_ABOVE * perf
        top = 1 + max(0, free - HOLES_ABOVE) // 2   # slack beyond the minimum gaps goes to the top inset first
        rows = [top + i * (perf + 1) for i in range(HOLES_ABOVE)]
    else:
        pitch = g["band_y"] / spec["holes"]
        perf = max(1, round(pitch * spec["duty"]))
        rows = [round((pitch - perf) / 2 + i * pitch) for i in range(spec["holes"])]
    xs = (g["bx"] + rim, g["bx"] + g["bw"] - rim - perf)
    return [(x, y) for y in rows for x in xs], perf, rim


def mark_frame(g, perf, rim):
    """The strip of body the landscape may occupy: inboard of the sprockets, or the whole body on a body that
    has none, where perf and rim are both zero."""
    return dict(g, bx=g["bx"] + rim + perf, bw=g["bw"] - 2 * (rim + perf))


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


def _load_wordmark_font():
    """The named instance as static font bytes: Bahnschrift ships variable-only, and both PIL and the SVG pen
    need an ordinary font."""
    f = TTFont(WORDMARK_FONT)
    instancer.instantiateVariableFont(f, WORDMARK_AXES, inplace=True)
    buf = BytesIO()
    f.save(buf)
    return buf.getvalue()


_font_bytes = _load_wordmark_font()
_font = TTFont(BytesIO(_font_bytes))
_glyphs = _font.getGlyphSet()
_cmap = _font.getBestCmap()
_upm = _font["head"].unitsPerEm
_hmtx = _font["hmtx"]
_cap_units = _font["OS/2"].sCapHeight


def _sized_font(cap):
    """The face at a given cap height in pixels. PIL reads a stream from where it is, so each call gets its
    own view of the bytes."""
    return ImageFont.truetype(BytesIO(_font_bytes), round(cap * _upm / _cap_units))


def _advances(label):
    return [_hmtx[_cmap[ord(c)]][0] for c in label]


def wordmark_fit(label, cap, avail):
    """Uniform scale set by cap height, then the horizontal squeeze needed to fit the band width."""
    s = cap / _cap_units
    natural = sum(_advances(label)) * s
    return s, (min(1.0, avail / natural) if natural else 1.0)


def _stem_of_cap():
    """The face's vertical stem as a fraction of cap height, measured off an I rendered at 400px cap."""
    ref = 400
    mask = _sized_font(ref).getmask("I", mode="1")
    w, h = mask.size
    lit = [x for x in range(w) if mask.getpixel((x, h // 2))]
    return (lit[-1] - lit[0] + 1) / ref


STEM_OF_CAP = _stem_of_cap()


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


def draw_vector_wordmark(im, label, g, size):
    """Composites the wordmark onto a supersampled icon. Depends on the band alone, so it serves any body
    shape."""
    k = SS
    cap = round(g["band_h"] * CAP_OF_BAND) * k
    _, sx = wordmark_fit(label, cap, TEXT_W * size * k)
    font = _sized_font(cap)
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


def render(label, colour, size, body=PAPER):
    k = SS
    im = Image.new("RGBA", (size * k, size * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    g = geom(size, body)
    holes, perf, rim = sprockets(size, g, body["sprocket"]) if body["sprocket"] else ([], 0, 0)

    d.rectangle([g["bx"] * k, 0, (g["bx"] + g["bw"]) * k - 1, size * k - 1], fill=hx(colour))
    for x, y in holes:
        d.rectangle([x * k, y * k, (x + perf) * k - 1, (y + perf) * k - 1], fill=CREAM)

    f = mark_frame(g, perf, rim)
    if size == PIXEL_MARK_AT:
        cell = f["bw"] * k / 10
        x0 = f["bx"] * k + (f["bw"] * k - 10 * cell) / 2
        for r, row in enumerate(MARK_PX):
            for i, ch in enumerate(row):
                if ch == "#":
                    d.rectangle([x0 + i * cell, r * cell, x0 + (i + 1) * cell - 1, (r + 1) * cell - 1],
                                fill=CREAM)
    else:
        mx, my, mw = mark_box(size, f)
        mx, my, mw = mx * k, my * k, mw * k
        d.polygon([(mx + a * mw, my + b * mw) for a, b in MARK_RIDGE], fill=CREAM)
        cx, cy, r = MARK_SUN
        d.ellipse([mx + (cx - r) * mw, my + (cy - r) * mw,
                   mx + (cx + r) * mw, my + (cy + r) * mw], fill=CREAM)

    d.rectangle([0, g["band_y"] * k, size * k - 1, (g["band_y"] + g["band_h"]) * k - 1], fill=INK)

    if size < VECTOR_FROM:
        glyphs = resolve_glyphs(label, size)
        gap, left, _ = place(label, size, glyphs)
        # Both offsets snap to the supersample grid: a half-pixel origin blurs every glyph edge.
        x = left * k
        top = (g["band_y"] * k + (g["band_h"] * k - FACE_ROWS * k) // 2) // k * k
        for glyph in glyphs:
            for r in range(FACE_ROWS):
                for i, ch in enumerate(glyph[r]):
                    if ch == "#":
                        d.rectangle([x + i * k, top + r * k, x + (i + 1) * k - 1, top + (r + 1) * k - 1],
                                    fill=CREAM)
            x += (len(glyph[0]) + gap) * k
    else:
        draw_vector_wordmark(im, label, g, size)
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
    """BMP at and below BMP_UPTO, PNG above it: the convention older icon readers expect."""
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


def svg_wordmark(label, g, size):
    """The wordmark as one <g> element. Depends on the band alone, so it serves any body shape."""
    cap = g["band_h"] * CAP_OF_BAND
    s, sx = wordmark_fit(label, cap, TEXT_W * size)
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
    return f'  <g fill="{CREAM_HEX}"{ink_pen}>\n    ' + "\n    ".join(paths) + '\n  </g>\n'


def write_svg(path, label, colour, body=PAPER):
    """The vector tier only: the pixel sizes cannot be expressed as one scalable drawing.

    Not grid-fitted either, one drawing having no target grid, so at small sizes this will not match the .ico.
    """
    size = 512
    g = geom(size, body)
    holes, perf, rim = sprockets(size, g, body["sprocket"]) if body["sprocket"] else ([], 0, 0)
    perfs = "".join(f'<rect x="{x}" y="{y}" width="{perf}" height="{perf}"/>\n    ' for x, y in holes)
    mx, my, mw = mark_box(size, mark_frame(g, perf, rim))
    ridge = " ".join(f"{mx + a * mw:.1f},{my + b * mw:.1f}" for a, b in MARK_RIDGE)
    cx, cy, r = MARK_SUN
    path.write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512">\n'
        f'  <rect x="{g["bx"]}" y="0" width="{g["bw"]}" height="512" fill="{colour}"/>\n'
        f'  <g fill="{CREAM_HEX}">\n'
        f'    {perfs}<polygon points="{ridge}"/>\n'
        f'    <circle cx="{mx + cx * mw:.1f}" cy="{my + cy * mw:.1f}" r="{r * mw:.1f}"/>\n'
        f'  </g>\n'
        f'  <rect x="0" y="{g["band_y"]}" width="512" height="{g["band_h"]}" fill="{INK_HEX}"/>\n'
        + svg_wordmark(label, g, size) + '</svg>\n',
        encoding="utf-8")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for stem, label, colour, body in TYPES:
        images = [render(label, colour, size, body) for size in SIZES]
        write_ico(OUT / f"{stem}.ico", images)
        write_svg(OUT / f"{stem}.svg", label, colour, body)
        print(f"{stem:5} {label:5} {colour}  {(OUT / f'{stem}.ico').stat().st_size:>7} B"
              f"  {len(SIZES)} sizes")


if __name__ == "__main__":
    sys.exit(main())
