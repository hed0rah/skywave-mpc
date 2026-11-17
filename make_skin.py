"""Render skywave's skin into skins/ (build.py ships skins/ verbatim when it exists).

A receiver in the manner of an Apollo-era spacecraft panel: grey painted metal, white lettering
and demarcation lines round each group, black instrument windows, figures in the green of the
guidance computer's display (DSKY), amber for caution. The windows: a world map of every public
KiwiSDR as a Mission Control wall map, with a crosshair on the one you are on; a DSKY-style
register for the frequency, the receiver and the mode; a signal strength meter; four
caution-and-warning tiles (CALLING, ON AIR, FAULT, SIGNAL), a red power switch for Listen, and the
status; a dial 0 to 30 MHz with the broadcast and ham bands and a needle. Black pointer knobs over
a printed scale, a big tuning knob, a guarded key for Grab. The controls sit in three blocks, one per
Q-Link column: TUNE, RECEIVER, PRESETS. SCOPE draws 64 bars of the receiver's waterfall over a
waterfall of its own, the passband's edges and the peak nearest the dial; CLEAN draws the passband
between two edges against the tuned frequency, a meter of what the cleanup removes, and BLANK and
NOTCH tiles.

A knob is a picture (a film strip following its parameter), not the MPC's Knob widget: a tap only
selects it, and the data wheel or its Q-Link turns it. The Knob widget took a tap's small slide for
a turn. TUNE is a jog: a step of STEP a turn, so the wheel tunes in the step chosen.

The moving parts are the MPC's own widgets bound to read-out parameters the plugin writes
(Dial, Signal, Lon, Lat, Link): a slider's thumb is the needle and each crosshair line, a
slider's revealed image is the meter, indicators are the tiles, the waterfall's cells are film strips.

    python make_skin.py <ne_50m_land.geojson>   # after a build has written build/params.json; then build again

The land is Natural Earth's 1:50m land polygons (public domain, naturalearthdata.com); the
receiver dots come from Content/receivers.js. Layout, TUI pieces and Barlow lettering (SIL OFL)
come from mpc/skin/skinkit.py, shared by every plugin; this panel draws its own face, knobs and keys.
"""
import json
import math
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "skin"))
from skinkit import *  # noqa: E402,F403  layout, lettering and TUI pieces
import skinkit  # noqa: E402

import preview  # noqa: E402  draws a tab from the skin, for the help pages

skinkit.BUTTONS.update({"listen": ("power_on.png", "power_off.png"), "grab": ("grab.png", "grab.png"),
                        "seekdown": ("seek_down.png", "seek_down.png"), "seekup": ("seek_up.png", "seek_up.png"),
                        "pageup": ("page_up.png", "page_up.png"), "pagedown": ("page_down.png", "page_down.png"),
                        "back": ("back.png", "back.png"), "dig": ("dig.png", "dig.png")})
KEY_IMG = {}                               # a touch key's picture, by parameter: set below

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "skins")

# parameter indices, as Skywave.cpp declares them
(LEVEL, LISTEN, REGION, RECEIVER, STATION, MHZ, KHZ, FINE, MODE, GRAB, GRABLEN, STATUS,
 DIAL, SIGNAL, LON, LAT, LINK, CATEGORY, SEEKDOWN, SEEKUP, SEEKBY, SPAN, LABEL, SCOPELO, SCOPEHI, BIN0) = range(26)
BINS = 64
(BANDWIDTH, SHIFT, AGC, GAIN, BLANKER, NOTCH, DENOISE, PASSLO, PASSHI, BLANKING, NOTCHING, REMOVED) = range(BIN0 + BINS, BIN0 + BINS + 12)
(LIST, SCROLL, PAGEUP, PAGEDOWN, BACK, DIG, ROW0) = range(REMOVED + 1, REMOVED + 8)
ROWS = 8
LAMP = ROW0 + ROWS                         # the SIGNAL lamp: something over the band's noise
WHEEL = LAMP + 1                           # the data wheel's jog: a row a step
(STEP, TUNE, TUNEPOS, SCALE, SPASSLO, SPASSHI, PEAK, WF0) = range(WHEEL + 1, WHEEL + 9)
WF_ROWS, WF_COLS, WF_LEVELS = 12, BINS, 12
KNOB2 = 96                                 # this panel's knobs; skinkit's are 80
VFO = 132                                  # the tuning knob

# the palette
PAINT = (98, 102, 105)                     # the panel's grey
LETTER = (236, 236, 230)                   # lettering and demarcation lines
BLACK = (7, 9, 10)                         # instrument windows
EL = (104, 255, 150)                       # the DSKY's electroluminescent green
EL_DIM = (40, 104, 66)
CAUTION = (255, 186, 52)
WARNING = (255, 76, 52)
A_EL = "ff68ff96"
A_EL_DIM = "ff4fb876"
A_LETTER = "ffecece6"
set_accent(CAUTION)
skinkit.A_ICE = A_EL                       # a window's value, in skinkit's cells
skinkit.NAME = text_style("Nexa", "Bold", 14, A_LETTER)
skinkit.VALUE = text_style("Titillium Web", "SemiBold", 18, A_LETTER)

# layout, tab coordinates
MAP = (24, 22, 568, 284)
THUMB = 9                                  # crosshair line thickness: a slider keeps its thumb inside the track
FREQ = (612, 22, 644, 160)                 # the register: frequency, then receiver and mode
METER_GLASS = (612, 196, 268, 52)
METER = (622, 202, 248, 28)                # the meter itself, inside its window
TILE_W, TILE_H = 60, 52
TILES = [(892 + k * TILE_W, 196) for k in range(4)]   # CALLING, ON AIR, FAULT, then SIGNAL
POWER = (1144, 196, 112, 52)               # Listen as the radio's power switch, after the lamps
INFO = (612, 262, 300, 44)
KEY_W = 80
TOP_KEYS = {p: (924 + k * 84, 258, KEY_W, 52) for k, p in enumerate((SEEKDOWN, SEEKUP, BACK, DIG))}   # touch keys beside the status
DIALG = (24, 330, 1232, 80)
NEEDLE_W = 12
BLOCKS = [   # title, cells: (kind, parameter, width); knobQuiet shows its value elsewhere
    ("TUNE", [("tune", TUNE, 150), ("plate", STEP, 100), ("plate", MODE, 92)]),
    ("RECEIVER", [("plate", REGION, 150), ("knobQuiet", RECEIVER, 96), ("knob", LEVEL, 96)]),
    ("PRESETS", [("plate", CATEGORY, 140), ("plate", STATION, 170), ("grab", GRAB, 92), ("plate", GRABLEN, 84)]),
]

# the SCOPE page: the dial's neighbourhood as 64 bars (a tap on one tunes onto its signal) over a
# waterfall of 12 rows, the newest on top; a column of each is 19 px, so the cells are whole pixels
SPEC = (24, 22, 1232, 272)
COL_W = 19
X0 = SPEC[0] + (SPEC[2] - COL_W * BINS) // 2              # the scope's left edge inside the window
SCOPE_W = COL_W * BINS
BAR_TOP, BAR_H, BAR_W = 46, 102, 13
WF_TOP, WF_ROW_H = 152, 10
SCOPE_BOT = WF_TOP + WF_ROWS * WF_ROW_H                  # 272: the edge labels go under it
INFO2 = (24, 310, 964, 60)
SCOPE_KEYS = {GRAB: (1008, 314, KEY_W, 52), BACK: (1092, 314, KEY_W, 52), DIG: (1176, 314, KEY_W, 52)}
SCOPE_BLOCKS = [
    ("SEEK", [("seekdown", SEEKDOWN, 84), ("seekup", SEEKUP, 84), ("plate", SEEKBY, 96), ("plate", SPAN, 96)]),
    ("TUNE", [("tune", TUNE, 150), ("plate", STEP, 100), ("plate", MODE, 92)]),
    ("VIEW", [("plate", LABEL, 182), ("plate", SCALE, 96), ("listen", LISTEN, 88), ("knob", LEVEL, 96)]),
]

# the CLEAN page: the passband round the tuned frequency, then what the cleanup takes out
PASSW = (24, 22, 1232, 214)
BAND = (44, 62, 1192, 112)                # the passband strip, -6 to +6 kHz
EDGE_W = 4                                 # an edge's thumb
REM_GLASS = (24, 252, 1088, 70)
REM = (36, 262, 1064, 26)                  # the meter of what the cleanup removes, 0 to 20 dB
TILES2 = [(1128 + k * TILE_W, 252) for k in range(2)]   # BLANK, NOTCH
INFO3 = (24, 338, 964, 66)
CLEAN_KEYS = {GRAB: (1008, 345, KEY_W, 52), BACK: (1092, 345, KEY_W, 52), DIG: (1176, 345, KEY_W, 52)}
CLEAN_BLOCKS = [
    ("PASSBAND", [("plate", BANDWIDTH, 150), ("knobBi", SHIFT, 96), ("plate", MODE, 150)]),
    ("NOISE", [("knob", BLANKER, 96), ("plate", NOTCH, 150), ("knob", DENOISE, 96)]),
    ("RECEIVER", [("plate", AGC, 150), ("knob", GAIN, 96), ("listen", LISTEN, 88), ("knob", LEVEL, 96)]),
]


# the LIST page: eight rows, tap one to go there; the radio's register and status beside them
ROWSW = (24, 22, 860, 384)
HEAD_H = 40
ROW_Y0, ROW_H = ROWSW[1] + HEAD_H + 4, 42
SIDE = (908, 22, 348, 150)
SIDE_STATUS = (908, 190, 348, 52)
LMETER_GLASS = (908, 258, 348, 52)
LMETER = (916, 264, 332, 28)
LTILES = [(908 + k * TILE_W, 326) for k in range(4)]
LIST_BLOCKS = [
    ("BROWSE", [("plate", LIST, 148), ("plate", CATEGORY, 164), ("plate", REGION, 148)]),
    ("GO", [("pageup", PAGEUP, 84), ("pagedown", PAGEDOWN, 84), ("back", BACK, 84), ("dig", DIG, 84)]),
    ("PLAY", [("listen", LISTEN, 88), ("grab", GRAB, 92), ("plate", GRABLEN, 84), ("knob", LEVEL, 96)]),
]


def bar_x(b):
    return X0 + b * COL_W + (COL_W - BAR_W) // 2


# ----- the face ---------------------------------------------------------------------------------

def paint(w, h, seed=1):
    """Matte painted metal: fine grain, a slow mottle, lit from above."""
    rng = np.random.default_rng(seed)
    grain = rng.normal(0, 2.4, (h, w))
    mottle = np.asarray(Image.fromarray((rng.normal(128, 30, (h // 40 + 2, w // 40 + 2))).clip(0, 255).astype("uint8"))
                        .resize((w, h), Image.BICUBIC), dtype=float) - 128
    light = 1.05 - 0.09 * np.linspace(0, 1, h)[:, None]
    out = np.stack([(c * light + grain + mottle * 0.05) for c in PAINT], axis=-1)
    return Image.fromarray(out.clip(0, 255).astype("uint8"))


def letter(d, xy, text, size, anchor="la", spacing=2, weight="SemiBold", colour=LETTER):
    """Panel lettering: capitals, spaced."""
    f = font(weight, size)
    x, y = xy
    width = sum(d.textlength(c, font=f) + spacing for c in text) - spacing
    x -= width if anchor[0] == "r" else width / 2 if anchor[0] == "m" else 0
    for c in text:
        d.text((x, y), c, font=f, fill=colour)
        x += d.textlength(c, font=f) + spacing
    return width


def window(img, box):
    """An instrument window cut into the panel: a dark rim, the black face."""
    x, y, w, h = box
    d = ImageDraw.Draw(img)
    d.rectangle([x - 3, y - 3, x + w + 2, y + h + 2], fill=(52, 55, 58))
    d.line([(x - 3, y + h + 3), (x + w + 3, y + h + 3)], fill=(140, 144, 147))
    d.line([(x + w + 3, y - 3), (x + w + 3, y + h + 3)], fill=(140, 144, 147))
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=BLACK)


def screw(d, x, y):
    d.ellipse([x - 5, y - 5, x + 5, y + 5], fill=(58, 61, 64))
    d.ellipse([x - 4, y - 4, x + 4, y + 4], fill=(150, 153, 156))
    d.line([(x - 3, y), (x + 3, y)], fill=(54, 57, 60), width=2)
    d.line([(x, y - 3), (x, y + 3)], fill=(54, 57, 60), width=2)


def screws(d):
    for x, y in ((7, 7), (W - 7, 7), (7, H - 7), (W - 7, H - 7)):
        screw(d, x, y)


def demarcate(d, box, title):
    """A group's white border, its title set into the top line."""
    x, y, w, h = box
    f = font("SemiBold", 13)
    tw = sum(d.textlength(c, font=f) + 3 for c in title) - 3
    cx = x + w / 2
    d.line([(x, y + 8), (cx - tw / 2 - 8, y + 8)], fill=LETTER, width=2)
    d.line([(cx + tw / 2 + 8, y + 8), (x + w, y + 8)], fill=LETTER, width=2)
    d.line([(x, y + 8), (x, y + h)], fill=LETTER, width=2)
    d.line([(x + w, y + 8), (x + w, y + h)], fill=LETTER, width=2)
    d.line([(x, y + h), (x + w, y + h)], fill=LETTER, width=2)
    letter(d, (cx, y), title, 13, "ma", 3)


def draw_blocks(img, blocks):
    d = ImageDraw.Draw(img)
    for b, (bx, by, bw, bh) in zip(blocks, block_bounds(blocks)):
        demarcate(d, (bx + 2, by, bw - 4, bh - 4), b[0])
    for kind, p, x, y, w in cells_xy(blocks):
        if kind == "plate":
            window(img, (x + 3, y + 18, w - 6, 50))


def seam(d, y):
    d.line([(0, y), (W, y)], fill=(54, 57, 60), width=2)
    d.line([(0, y + 2), (W, y + 2)], fill=(138, 142, 145))


def legend_tile(img, box, text):
    """The DSKY's lit legends (PROG, VERB, NOUN): green with black letters, always on."""
    x, y, w, h = box
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(glow).rectangle([x - 2, y - 2, x + w + 2, y + h + 2], fill=EL + (90,))
    img.paste(Image.alpha_composite(img.convert("RGBA"), glow.filter(ImageFilter.GaussianBlur(4))).convert("RGB"))
    d = ImageDraw.Draw(img)
    d.rectangle([x, y, x + w - 1, y + h - 1], fill=EL)
    letter(d, (x + w / 2, y + h / 2 - 8), text, 13, "ma", 2, "SemiBold", (10, 30, 16))


# ----- the map: a Mission Control wall map ------------------------------------------------------

def map_xy(lon, lat):
    """Map-local pixels, as the crosshair sliders place their thumbs."""
    x = THUMB / 2 + (lon + 180.0) / 360.0 * (MAP[2] - THUMB)
    y = THUMB / 2 + (90.0 - lat) / 180.0 * (MAP[3] - THUMB)
    return x, y


def receivers():
    text = open(os.path.join(HERE, "Content", "receivers.js"), encoding="utf-8", errors="replace").read()
    return [(float(m.group(1)), float(m.group(2)))
            for m in re.finditer(r'"gps"\s*:\s*"\(\s*(-?[\d.]+)\s*,\s*(-?[\d.]+)\s*\)"', text)]


def draw_map(land_path):
    """Coastlines on a dark plotting board, a graticule every 30 degrees, receivers as points."""
    w, h = MAP[2] * SS, MAP[3] * SS
    ocean, land_fill, grid, coast = (6, 13, 18), (24, 40, 44), (32, 54, 62), (150, 196, 196)
    img = Image.new("RGB", (w, h), ocean)
    d = ImageDraw.Draw(img)
    rings = []
    for f in json.load(open(land_path, encoding="utf-8"))["features"]:
        g = f["geometry"]
        for poly in (g["coordinates"] if g["type"] == "MultiPolygon" else [g["coordinates"]]):
            for k, ring in enumerate(poly):
                pts = [tuple(c * SS for c in map_xy(lon, lat)) for lon, lat in ring]
                if len(pts) >= 3:
                    d.polygon(pts, fill=ocean if k else land_fill)
                    rings.append((ring + ring[:1], pts + pts[:1]))
    for lon in range(-150, 180, 30):
        x, _ = map_xy(lon, 0)
        d.line([(x * SS, 0), (x * SS, h)], fill=grid, width=SS)
    for lat in (-60, -30, 30, 60):
        _, y = map_xy(0, lat)
        d.line([(0, y * SS), (w, y * SS)], fill=grid, width=SS)
    _, eq = map_xy(0, 0)
    d.line([(0, eq * SS), (w, eq * SS)], fill=(50, 86, 96), width=SS)
    def edge(a, b):                                           # the map's own border (Antarctica, the date line) is no coast
        return (a[1] < -89.9 and b[1] < -89.9) or (abs(a[0]) > 179.99 and abs(b[0]) > 179.99)
    for ring, pts in rings:
        run = [pts[0]]
        for i in range(1, len(pts)):
            if edge(ring[i - 1], ring[i]):
                if len(run) > 1:
                    d.line(run, fill=coast, width=SS)
                run = []
            run.append(pts[i])
        if len(run) > 1:
            d.line(run, fill=coast, width=SS)
    tag = font("Medium", 9 * SS)
    for lon in range(-150, 180, 30):
        x, _ = map_xy(lon, 0)
        d.text((x * SS + 3 * SS, 2 * SS), "%d%s" % (abs(lon), "W" if lon < 0 else "E" if lon else ""), font=tag, fill=(84, 128, 136))
    for lat in (-60, -30, 30, 60):
        _, y = map_xy(0, lat)
        d.text((3 * SS, y * SS + 2 * SS), "%d%s" % (abs(lat), "S" if lat < 0 else "N"), font=tag, fill=(84, 128, 136))
    for lat, lon in receivers():
        x, y = map_xy(lon, lat)
        d.rectangle([x * SS - 1.2 * SS, y * SS - 1.2 * SS, x * SS + 1.2 * SS, y * SS + 1.2 * SS], fill=(240, 244, 232))
    return img.resize((MAP[2], MAP[3]), Image.LANCZOS)


def line_thumb(w, h, vertical):
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    if vertical:
        c = w // 2
        d.rectangle([c - 2, 0, c + 2, h], fill=CAUTION + (60,))
        d.line([(c, 0), (c, h)], fill=CAUTION + (255,), width=1)
    else:
        c = h // 2
        d.rectangle([0, c - 2, w, c + 2], fill=CAUTION + (60,))
        d.line([(0, c), (w, c)], fill=CAUTION + (255,), width=1)
    return img


# ----- the dial ---------------------------------------------------------------------------------

BROADCAST = [("120", 2.3, 2.495), ("90", 3.2, 3.4), ("75", 3.9, 4.0), ("60", 4.75, 5.06), ("49", 5.9, 6.2),
             ("41", 7.2, 7.45), ("31", 9.4, 9.9), ("25", 11.6, 12.1), ("22", 13.57, 13.87), ("19", 15.1, 15.8),
             ("16", 17.48, 17.9), ("15", 18.9, 19.02), ("13", 21.45, 21.85), ("11", 25.67, 26.1)]
HAM = [("160", 1.8, 2.0), ("80", 3.5, 4.0), ("40", 7.0, 7.3), ("30", 10.1, 10.15), ("20", 14.0, 14.35),
       ("17", 18.068, 18.168), ("15", 21.0, 21.45), ("12", 24.89, 24.99), ("10", 28.0, 29.7)]
TIME = [2.5, 5.0, 10.0, 15.0, 20.0, 25.0]


def dial_x(mhz):
    """Window-local pixels, as the needle slider places its thumb."""
    return NEEDLE_W / 2 + mhz / 30.0 * (DIALG[2] - NEEDLE_W)


def draw_dial():
    w, h = DIALG[2] * SS, DIALG[3] * SS
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    f_band, f_num, f_tag = font("Medium", 11 * SS), font("Regular", 13 * SS), font("SemiBold", 10 * SS)
    for name, a, b in BROADCAST:                              # broadcast bands, in metres
        x0, x1 = dial_x(a) * SS, dial_x(b) * SS
        d.rectangle([x0, 9 * SS, max(x1, x0 + 2 * SS), 11 * SS], fill=LETTER + (200,))
        d.text(((x0 + x1) / 2, 13 * SS), name, font=f_band, fill=LETTER + (210,), anchor="ma")
    for name, a, b in HAM:                                    # ham bands
        x0, x1 = dial_x(a) * SS, dial_x(b) * SS
        d.rectangle([x0, 33 * SS, max(x1, x0 + 2 * SS), 35 * SS], fill=CAUTION + (200,))
        d.text(((x0 + x1) / 2, 37 * SS), name, font=f_band, fill=CAUTION + (210,), anchor="ma")
    for f in TIME:                                            # WWV
        x = dial_x(f) * SS
        d.ellipse([x - 2 * SS, 27 * SS, x + 2 * SS, 31 * SS], fill=EL + (230,))
    for k in range(0, 301):
        x = dial_x(k / 10.0) * SS
        major, half = k % 10 == 0, k % 5 == 0
        top = 52 if major else 56 if half else 59
        d.line([(x, top * SS), (x, 62 * SS)], fill=LETTER + ((235 if major else 120),), width=SS)
        if major:
            d.text((x, 63 * SS), str(k // 10), font=f_num, fill=LETTER + (220,), anchor="la" if k == 0 else "ra" if k == 300 else "ma")
    d.text((5 * SS, 4 * SS), "METRE BANDS", font=f_tag, fill=(150, 154, 150, 255))
    d.text((5 * SS, 28 * SS), "HAM", font=f_tag, fill=(150, 154, 150, 255))
    d.text((w - 5 * SS, 4 * SS), "MHz", font=f_tag, fill=(150, 154, 150, 255), anchor="ra")
    return img.resize((DIALG[2], DIALG[3]), Image.LANCZOS)


def needle():
    w, h = NEEDLE_W, DIALG[3]
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = w // 2
    d.rectangle([c - 3, 0, c + 3, h], fill=CAUTION + (55,))
    d.rectangle([c - 1, 0, c, h], fill=CAUTION + (255,))
    d.polygon([(c - 5, 0), (c + 5, 0), (c, 7)], fill=CAUTION + (255,))
    return img


# ----- the meter --------------------------------------------------------------------------------

BARS = 48


def draw_meter(lit, box=METER):
    """Segments left to right, green to S9, amber over it. Unlit, a faint ghost of the same."""
    w, h = box[2], box[3]
    s = SS
    img = Image.new("RGBA", (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pitch = w * s / BARS
    s9 = int(BARS * 54 / 94)
    for i in range(BARS):
        x0 = i * pitch + pitch * 0.15
        c = EL if i < s9 else CAUTION
        col = c + (255,) if lit else tuple(int(v * 0.14) + 10 for v in c) + (255,)
        d.rectangle([x0, 4 * s, x0 + pitch * 0.7, h * s], fill=col)
    img = img.resize((w, h), Image.LANCZOS)
    if lit:
        img = Image.alpha_composite(img.filter(ImageFilter.GaussianBlur(2.5)), img)
    return img


# ----- knobs, keys, tiles -----------------------------------------------------------------------

def knob_frame(v, bipolar, size=None):
    """A black pointer knob over a scale printed on the panel; the white line points at the value.
    Drawn on an 80 px plan, at size."""
    size = size or KNOB2
    u = SS * size / 80.0                                      # a plan pixel, supersampled
    s = int(size * SS)
    c = s / 2
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for k in range(11):
        a = math.radians(-135 + 27 * k)
        major = k in (0, 10) or (bipolar and k == 5)
        r0, r1 = (31 if major else 34) * u, 38 * u
        d.line([(c + math.sin(a) * r0, c - math.cos(a) * r0), (c + math.sin(a) * r1, c - math.cos(a) * r1)],
               fill=LETTER + (255,), width=int((2 if major else 1) * u))
    sh = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([c - 26 * u, c - 23 * u, c + 26 * u, c + 29 * u], fill=(0, 0, 0, 150))
    img = Image.alpha_composite(img, sh.filter(ImageFilter.GaussianBlur(3 * u)))
    d = ImageDraw.Draw(img)
    r = 26 * u                                                # the skirt
    d.ellipse([c - r, c - r, c + r, c + r], fill=(22, 23, 25, 255), outline=(74, 77, 80, 255), width=int(u))
    bar = Image.new("RGBA", (s, s), (0, 0, 0, 0))             # the pointer bar, drawn upright, then turned
    bd = ImageDraw.Draw(bar)
    bd.rounded_rectangle([c - 8 * u, c - 25 * u, c + 8 * u, c + 25 * u], 6 * u, fill=(36, 37, 40, 255), outline=(96, 99, 103, 255), width=int(u))
    bd.line([(c - 5 * u, c - 21 * u), (c - 5 * u, c + 21 * u)], fill=(70, 73, 77, 255), width=int(u))
    bd.line([(c, c - 22 * u), (c, c - 5 * u)], fill=LETTER + (255,), width=int(3 * u))
    bar = bar.rotate(135 - 270 * v, resample=Image.BICUBIC, center=(c, c))
    img = Image.alpha_composite(img, bar)
    return img.resize((size, size), Image.LANCZOS)


def knob_strip(bipolar, size=KNOB2):
    strip = Image.new("RGBA", (size, size * KNOB_FRAMES), (0, 0, 0, 0))
    for k in range(KNOB_FRAMES):
        strip.paste(knob_frame(k / (KNOB_FRAMES - 1), bipolar, size), (0, k * size))
    return strip


def vfo_frame(v):
    """The tuning knob, a radio's main dial: a knurled black skirt, a dished face with a finger dimple,
    a white index; v is a share of a turn (Tune Pos: a turn for 32 steps)."""
    s = VFO * SS
    c = s / 2
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    sh = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse([c - 60 * SS, c - 55 * SS, c + 60 * SS, c + 65 * SS], fill=(0, 0, 0, 160))
    img = Image.alpha_composite(img, sh.filter(ImageFilter.GaussianBlur(4 * SS)))
    d = ImageDraw.Draw(img)
    r = 61 * SS
    d.ellipse([c - r, c - r, c + r, c + r], fill=(18, 19, 21, 255), outline=(70, 73, 76, 255), width=SS)
    turn = 2 * math.pi * v                                   # clockwise as the frequency rises, like the data wheel
    for k in range(60):                                       # the knurl, turning with the knob
        a = turn + 2 * math.pi * k / 60
        r0, r1 = 52 * SS, 60 * SS
        d.line([(c + math.sin(a) * r0, c - math.cos(a) * r0), (c + math.sin(a) * r1, c - math.cos(a) * r1)],
               fill=(58, 61, 65, 255) if k % 2 else (30, 32, 35, 255), width=2 * SS)
    face = Image.new("RGBA", (s, s), (0, 0, 0, 0))            # the face: lit from above
    fd = ImageDraw.Draw(face)
    for k in range(48, 0, -1):
        t = k / 48
        g = int(26 + 26 * (1 - t))
        fd.ellipse([c - k * SS, c - k * SS - (1 - t) * 6 * SS, c + k * SS, c + k * SS - (1 - t) * 6 * SS], fill=(g, g + 1, g + 4, 255))
    fd.ellipse([c - 48 * SS, c - 48 * SS, c + 48 * SS, c + 48 * SS], outline=(84, 88, 92, 255), width=SS)
    img = Image.alpha_composite(img, face)
    d = ImageDraw.Draw(img)
    dx, dy = math.sin(turn + 0.0) * 30 * SS, -math.cos(turn) * 30 * SS   # the finger dimple, opposite the index
    d.ellipse([c - dx - 11 * SS, c - dy - 11 * SS, c - dx + 11 * SS, c - dy + 11 * SS], fill=(12, 13, 15, 255), outline=(66, 69, 73, 255), width=SS)
    d.ellipse([c - dx - 7 * SS, c - dy - 5 * SS, c - dx + 7 * SS, c - dy + 9 * SS], fill=(22, 23, 26, 255))
    r0, r1 = 34 * SS, 47 * SS                                 # the index
    d.line([(c + math.sin(turn) * r0, c - math.cos(turn) * r0), (c + math.sin(turn) * r1, c - math.cos(turn) * r1)],
           fill=LETTER + (255,), width=4 * SS)
    return img.resize((VFO, VFO), Image.LANCZOS)


def vfo_strip():
    strip = Image.new("RGBA", (VFO, VFO * KNOB_FRAMES), (0, 0, 0, 0))
    for k in range(KNOB_FRAMES):
        strip.paste(vfo_frame(k / KNOB_FRAMES), (0, k * VFO))
    return strip


def power(on, w=84, h=66):
    """The power switch: a red LED over POWER, and an illuminated red rocker, I pressed for on."""
    s = SS
    img = Image.new("RGBA", (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, w * s - 1, h * s - 1], 5 * s, fill=(22, 23, 25, 255), outline=(70, 73, 76, 255), width=s)
    lx, ly, r = 17 * s, (h / 2 - 7) * s, 6 * s
    if on:                                                 # the LED's glow, soft and inside the bezel
        glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
        ImageDraw.Draw(glow).ellipse([lx - r * 1.5, ly - r * 1.5, lx + r * 1.5, ly + r * 1.5], fill=(255, 40, 24, 110))
        img.alpha_composite(glow.filter(ImageFilter.GaussianBlur(2 * s)))
        d = ImageDraw.Draw(img)
    d.ellipse([lx - r, ly - r, lx + r, ly + r], fill=(255, 52, 36, 255) if on else (72, 20, 18, 255), outline=(10, 10, 10, 255), width=s)
    if on:
        d.ellipse([lx - r * 0.45, ly - r * 0.55, lx + r * 0.1, ly - r * 0.05], fill=(255, 190, 170, 255))
    d.text((lx, (h / 2 + 10) * s), "POWER", font=font("SemiBold", 8 * s), fill=LETTER + (255,), anchor="mm")
    x0, y0, x1, y1 = 34 * s, 6 * s, w * s - 7 * s, h * s - 7 * s
    my = (y0 + y1) / 2
    base, lit = ((206, 44, 30), (255, 92, 70)) if on else ((92, 22, 18), (128, 34, 28))
    d.rounded_rectangle([x0 - 2 * s, y0 - 2 * s, x1 + 2 * s, y1 + 2 * s], 4 * s, fill=(8, 8, 9, 255))
    d.rounded_rectangle([x0, y0, x1, y1], 3 * s, fill=base + (255,))
    # the raised half catches the light: O raised when on (I pressed in), I raised when off
    ry0, ry1 = (my, y1) if on else (y0, my)
    d.rectangle([x0 + s, ry0, x1 - s, ry1], fill=lit + (255,))
    d.line([(x0, my), (x1, my)], fill=(40, 10, 8, 255), width=s)
    mark = (255, 236, 230, 255) if on else (190, 150, 146, 255)
    cx = (x0 + x1) / 2
    d.line([(cx, y0 + (my - y0) * 0.28), (cx, y0 + (my - y0) * 0.72)], fill=mark, width=2 * s)
    rr = (y1 - my) * 0.2
    oy = my + (y1 - my) / 2
    d.ellipse([cx - rr, oy - rr, cx + rr, oy + rr], outline=mark, width=2 * s)
    return img.resize((w, h), Image.LANCZOS)


def guarded_key(text):
    """A red key between two guard rails, so it is pressed on purpose."""
    w, h, s = 84, 66, SS
    img = Image.new("RGBA", (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for x in (6, 70):
        d.rounded_rectangle([x * s, 4 * s, (x + 8) * s, 62 * s], 2 * s, fill=(176, 179, 182, 255), outline=(56, 59, 62, 255), width=s)
    d.rectangle([17 * s, 9 * s, 67 * s, 59 * s], fill=(30, 31, 33, 255))
    d.rectangle([19 * s, 11 * s, 65 * s, 57 * s], fill=(198, 50, 38, 255))
    d.line([(19 * s, 11 * s), (65 * s, 11 * s)], fill=(236, 110, 96, 255), width=s)
    d.text((42 * s, 34 * s), text, font=font("SemiBold", 14 * s), fill=(252, 246, 240, 255), anchor="mm")
    return img.resize((w, h), Image.LANCZOS)


def dsky_key(legend, glyph=None, w=84, h=66):
    """A DSKY key: light, square, black legend, an arrow over it ("left", "right", "up", "down") or none."""
    s = SS
    img = Image.new("RGBA", (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    side = min(56, h - 10)
    x0, y0 = (w - side) / 2, (h - side) / 2
    d.rectangle([x0 * s, y0 * s, (x0 + side) * s, (y0 + side) * s], fill=(34, 36, 38, 255))
    d.rectangle([(x0 + 2) * s, (y0 + 2) * s, (x0 + side - 2) * s, (y0 + side - 2) * s], fill=(216, 216, 208, 255))
    cx, a = w / 2 * s, side / 8 * s
    cy = (y0 + side * 0.33) * s if glyph else h / 2 * s
    ink = (20, 20, 20, 255)
    if glyph in ("left", "right"):
        k = 1 if glyph == "right" else -1
        d.polygon([(cx + k * a, cy), (cx - k * a, cy - a), (cx - k * a, cy + a)], fill=ink)
    elif glyph in ("up", "down"):
        k = 1 if glyph == "down" else -1
        d.polygon([(cx, cy + k * a), (cx - a, cy - k * a), (cx + a, cy - k * a)], fill=ink)
    ty = (y0 + side * 0.72) * s if glyph else cy
    d.text((cx, ty), legend, font=font("SemiBold", (13 if side >= 50 else 12) * s), fill=ink, anchor="mm")
    return img.resize((w, h), Image.LANCZOS)


def meter_scale(d, box):
    f_scale = font("Medium", 10)
    for name, db in [("S1", 6), ("3", 18), ("5", 30), ("7", 42), ("S9", 54), ("+20", 74), ("+40", 94)]:
        x = box[0] + db / 94 * box[2]
        d.text((min(x, box[0] + box[2] - 10), box[1] + box[3] + 3), name, font=f_scale, fill=(150, 154, 150), anchor="ma")


def red_key(text, w=KEY_W, h=52):
    """GRAB as a touch key: the guarded key's red, square like a DSKY key."""
    s = SS
    img = Image.new("RGBA", (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    side = min(56, h - 10)
    x0, y0 = (w - side) / 2, (h - side) / 2
    d.rectangle([x0 * s, y0 * s, (x0 + side) * s, (y0 + side) * s], fill=(30, 31, 33, 255))
    d.rectangle([(x0 + 2) * s, (y0 + 2) * s, (x0 + side - 2) * s, (y0 + side - 2) * s], fill=(198, 50, 38, 255))
    d.text((w / 2 * s, h / 2 * s), text, font=font("SemiBold", 12 * s), fill=(252, 246, 240, 255), anchor="mm")
    return img.resize((w, h), Image.LANCZOS)


def tile(text, lit):
    """A caution-and-warning tile: dark with a grey legend, or lit with a black one."""
    w, h = TILE_W, TILE_H
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, w - 1, h - 1], fill=(24, 25, 27, 255))
    face = lit if lit else (54, 57, 59)
    d.rectangle([3, 3, w - 4, h - 4], fill=face + (255,))
    if lit:
        hi = tuple(min(255, v + 40) for v in lit)
        d.rectangle([8, 8, w - 9, h - 9], fill=hi + (255,))
    letter(d, (w / 2, h / 2 - 7), text, 11, "ma", 1, "SemiBold", (24, 20, 14) if lit else (120, 124, 126))
    return img


# ----- the panel --------------------------------------------------------------------------------

def draw_background(land_path):
    img = paint(W, H)
    for box in (MAP, FREQ, METER_GLASS, INFO, DIALG):
        window(img, box)
    window(img, (TILES[0][0], TILES[0][1], TILE_W * 4, TILE_H))
    img.paste(draw_map(land_path), MAP[:2])
    dial = draw_dial()
    img.paste(dial, DIALG[:2], dial)
    ghost = draw_meter(False)
    img.paste(ghost, METER[:2], ghost)
    fx, fy, fw, fh = FREQ
    legend_tile(img, (fx + 16, fy + 16, 70, 24), "FREQ")
    legend_tile(img, (fx + 16, fy + 118, 70, 24), "RCVR")
    legend_tile(img, (fx + fw - 206, fy + 118, 70, 24), "MODE")
    d = ImageDraw.Draw(img)
    d.rectangle([fx + 16, fy + 105, fx + fw - 17, fy + 106], fill=EL_DIM)   # the DSKY's register bars
    meter_scale(d, METER)
    letter(d, (MAP[0], MAP[1] + MAP[3] + 8), "RECEIVER NETWORK", 11, "la", 3)
    letter(d, (MAP[0] + MAP[2], MAP[1] + MAP[3] + 7), "SKYWAVE", 13, "ra", 4)
    draw_blocks(img, BLOCKS)
    d = ImageDraw.Draw(img)
    seam(d, 420)
    screws(d)
    return img


def draw_scope_background():
    img = paint(W, H, seed=2)
    for box in (SPEC, INFO2):
        window(img, box)
    d = ImageDraw.Draw(img)
    x0, x1 = X0, X0 + SCOPE_W
    grid = (22, 50, 34)
    for k in range(1, 5):                                     # level lines over the bars
        y = BAR_TOP + BAR_H - k * BAR_H / 5
        d.line([(x0, y), (x1, y)], fill=grid, width=1)
    for k in range(1, 8):                                     # eighths of the span, through the waterfall
        if k == 4:
            continue                                          # the dial's line is an overlay, over the cells
        x = x0 + k * SCOPE_W / 8
        d.line([(x, BAR_TOP - 4), (x, SCOPE_BOT - 1)], fill=grid, width=1)
    d.line([(x0, WF_TOP - 3), (x1, WF_TOP - 3)], fill=EL_DIM, width=1)   # bars above, waterfall below
    for b in range(BINS):                                     # the bars' unlit ghosts
        x = bar_x(b)
        d.rectangle([x, BAR_TOP + BAR_H - 3, x + BAR_W - 1, BAR_TOP + BAR_H - 1], fill=(22, 44, 30))
    tag = font("SemiBold", 10)
    d.text((x0, SPEC[1] + 8), "PEAK", font=tag, fill=(90, 170, 118))
    d.text((x1, SPEC[1] + 8), "TAP A BAR: TUNED ONTO ITS SIGNAL", font=tag, fill=(90, 170, 118), anchor="ra")
    draw_blocks(img, SCOPE_BLOCKS)
    d = ImageDraw.Draw(img)
    seam(d, 420)
    screws(d)
    return img


# the scope's shades, weak to strong: deep green, the DSKY's green, amber, near white
HEAT = [(0.0, (14, 40, 24)), (0.35, (40, 150, 86)), (0.6, EL), (0.82, CAUTION), (1.0, (255, 244, 214))]


def heat(t):
    for (t0, c0), (t1, c1) in zip(HEAT, HEAT[1:]):
        if t <= t1:
            u = (t - t0) / (t1 - t0)
            return tuple(int(a + (b - a) * u) for a, b in zip(c0, c1))
    return HEAT[-1][1]


def bar_image():
    """A scope bar, revealed from the bottom: its colour says how high it reaches, in the waterfall's shades."""
    img = Image.new("RGBA", (BAR_W, BAR_H), (0, 0, 0, 0))
    px = img.load()
    for y in range(BAR_H):
        c = heat(0.12 + 0.88 * (1.0 - y / (BAR_H - 1)))      # 0 at the bottom
        for x in range(BAR_W):
            edge = 0.75 if x in (0, BAR_W - 1) else 1.0
            px[x, y] = tuple(int(v * edge) for v in c) + (255,)
    return img


def wf_strip():
    """A waterfall cell's frames: clear at the floor (the grid shows through), then the shades."""
    strip = Image.new("RGBA", (COL_W, WF_ROW_H * WF_LEVELS), (0, 0, 0, 0))
    d = ImageDraw.Draw(strip)
    for k in range(1, WF_LEVELS):
        d.rectangle([0, k * WF_ROW_H, COL_W - 1, (k + 1) * WF_ROW_H - 1], fill=heat(k / (WF_LEVELS - 1)) + (255,))
    return strip


def mark(w, h, colour, alpha):
    """A vertical line the height of the scope: the dial, or a passband edge."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ImageDraw.Draw(img).rectangle([0, 0, w - 1, h - 1], fill=colour + (alpha,))
    return img



def band_x(khz):
    """Where a passband edge at khz (-6 to +6) sits in BAND: a slider keeps its thumb inside the track."""
    return BAND[0] + EDGE_W / 2 + (khz + 6.0) / 12.0 * (BAND[2] - EDGE_W)


def draw_clean_background():
    img = paint(W, H, seed=3)
    for box in (PASSW, REM_GLASS, INFO3):
        window(img, box)
    window(img, (TILES2[0][0], TILES2[0][1], TILE_W * 2, TILE_H))
    d = ImageDraw.Draw(img)
    tag = font("SemiBold", 10)
    dim = (90, 170, 118)
    bx, by, bw, bh = BAND
    d.text((PASSW[0] + 10, PASSW[1] + 6), "PASSBAND", font=tag, fill=dim)
    d.text((PASSW[0] + PASSW[2] - 10, PASSW[1] + 6), "KHZ EITHER SIDE OF THE TUNED FREQUENCY", font=tag, fill=dim, anchor="ra")
    d.text((band_x(-3), by - 14), "LOWER SIDEBAND", font=tag, fill=(70, 120, 88), anchor="ma")
    d.text((band_x(3), by - 14), "UPPER SIDEBAND", font=tag, fill=(70, 120, 88), anchor="ma")
    scale = font("Medium", 12)
    for k in range(-6, 7):
        x = band_x(k)
        d.line([(x, by + bh + 2), (x, by + bh + (10 if k % 3 == 0 else 6))], fill=dim, width=1)
        d.text((x, by + bh + 12), "%+d" % k if k else "0", font=scale, fill=(150, 154, 150), anchor="ma")
    ghost = draw_rem_meter(False)
    img.paste(ghost, REM[:2], ghost)
    d = ImageDraw.Draw(img)
    for db in (0, 5, 10, 15, 20):
        x = REM[0] + db / 20 * REM[2]
        d.text((min(max(x, REM[0] + 6), REM[0] + REM[2] - 12), REM[1] + REM[3] + 3), str(db), font=scale, fill=(150, 154, 150), anchor="ma")
    d.text((REM[0] + REM[2], REM[1] + REM[3] + 19), "DB TAKEN OUT BY THE BLANKER, NOTCH AND DENOISE", font=tag, fill=dim, anchor="ra")
    draw_blocks(img, CLEAN_BLOCKS)
    d = ImageDraw.Draw(img)
    seam(d, 420)
    screws(d)
    return img


def pass_fill():
    """The passband, lit: green, brighter at its top edge, the way a filter's response is drawn."""
    w, h = BAND[2], BAND[3]
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = 1.0 - y / (h - 1)
        c = tuple(int(10 + (v - 10) * (0.18 + 0.32 * t)) for v in EL)
        d.line([(0, y), (w, y)], fill=c + (255,))
    d.rectangle([0, 0, w, 3], fill=EL + (255,))
    return img


def pass_grid():
    """Over the strip: faint kilohertz lines, and the tuned frequency as a dashed white line."""
    w, h = BAND[2], BAND[3]
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    for k in range(-6, 7):
        x = band_x(k) - BAND[0]
        if k:
            d.line([(x, 0), (x, h)], fill=(104, 255, 150, 40), width=1)
    x0 = band_x(0) - BAND[0]
    for y in range(0, h, 10):
        d.line([(x0, y), (x0, y + 5)], fill=LETTER + (230,), width=2)
    return img


def edge_thumb():
    img = Image.new("RGBA", (EDGE_W, BAND[3]), CAUTION + (255,))
    return img


def draw_rem_meter(lit):
    """What the cleanup takes out: segments, green to 10 dB, amber over it."""
    w, h = REM[2], REM[3]
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    n = 40
    pitch = w / n
    for i in range(n):
        x0 = i * pitch + pitch * 0.15
        c = EL if i < n // 2 else CAUTION
        col = c + (255,) if lit else tuple(int(v * 0.14) + 10 for v in c) + (255,)
        d.rectangle([x0, 3, x0 + pitch * 0.7, h - 1], fill=col)
    if lit:
        img = Image.alpha_composite(img.filter(ImageFilter.GaussianBlur(2.5)), img)
    return img

def draw_list_background():
    img = paint(W, H, seed=4)
    for box in (ROWSW, SIDE, SIDE_STATUS, LMETER_GLASS):
        window(img, box)
    window(img, (LTILES[0][0], LTILES[0][1], TILE_W * 4, TILE_H))
    ghost = draw_meter(False, LMETER)
    img.paste(ghost, LMETER[:2], ghost)
    d = ImageDraw.Draw(img)
    x0, x1 = ROWSW[0] + 10, ROWSW[0] + ROWSW[2] - 10
    d.line([(x0, ROWSW[1] + HEAD_H), (x1, ROWSW[1] + HEAD_H)], fill=EL_DIM, width=1)
    for r in range(1, ROWS):                                  # faint rules between the rows
        y = ROW_Y0 + r * ROW_H
        d.line([(x0, y), (x1, y)], fill=(18, 34, 24), width=1)
    tag = font("SemiBold", 10)
    d.text((x1, ROWSW[1] + 14), "TAP HERE, TURN THE WHEEL: SCROLL", font=tag, fill=(90, 170, 118), anchor="ra")
    d.text((SIDE[0] + 12, SIDE[1] + 8), "FREQ", font=tag, fill=(90, 170, 118))
    d.text((SIDE[0] + 12, SIDE[1] + 96), "RCVR", font=tag, fill=(90, 170, 118))
    meter_scale(d, LMETER)
    hint = font("Medium", 12)
    d.text((SIDE[0] + 2, LTILES[0][1] + TILE_H + 12), "LIVE: PAGE UP AT THE TOP SCANS AGAIN", font=hint, fill=LETTER)
    draw_blocks(img, LIST_BLOCKS)
    d = ImageDraw.Draw(img)
    seam(d, 420)
    screws(d)
    return img


# ----- HELP: each page drawn small, its parts numbered, a legend beside it ------------------------
# An entry's place is a box (x, y, w, h) on that page, or a parameter, found in the page's blocks.

HELP = [
    ("RADIO", "F1", BLOCKS, [
        (MAP, "MAP", "every public receiver; the cross is yours"),
        (FREQ, "REGISTER", "frequency, receiver, mode"),
        (METER_GLASS, "S-METER", "how strong, S1 to S9 and over"),
        ((TILES[0][0], TILES[0][1], TILE_W * 3, TILE_H), "LAMPS", "calling, on air, fault"),
        ((TILES[3][0], TILES[3][1], TILE_W, TILE_H), "SIGNAL", "lit: something over the noise here"),
        (POWER, "POWER", "LISTEN: on takes a slot, off frees it"),
        (INFO, "STATUS", "what it is doing; read it when stuck"),
        (TOP_KEYS[SEEKDOWN], "SEEK", "the next signal down or up"),
        (TOP_KEYS[BACK], "BACK", "the last place you stayed"),
        (TOP_KEYS[DIG], "DIG", "somewhere in the CATEGORY, at random"),
        (DIALG, "DIAL", "0 to 30 MHz, the bands marked"),
        (TUNE, "TUNE", "tap it, turn the wheel: a STEP a click"),
        (STEP, "STEP", "10 Hz to 1 MHz; 5, 9, 10 kHz channels"),
        (MODE, "MODE", "AM broadcast, USB/LSB voice, CW"),
        (REGION, "REGION", "where in the world to look"),
        (RECEIVER, "RECEIVER", "which radio; called after 1 s"),
        (LEVEL, "LEVEL", "volume"),
        (CATEGORY, "CATEGORY", "what it sounds like; or Anything"),
        (STATION, "STATION", "the CATEGORY's stations"),
        (GRAB, "GRAB", "save the last seconds as a WAV"),
        (GRABLEN, "GRAB LEN", "seconds, or bars at the tempo"),
    ], "Start here. Turn POWER on and the radio calls the receiver under the cross on the map. Pick a CATEGORY "
       "(or Anything) and press DIG: "
       "it sweeps the receiver's band once and lands on something on the air; SEEK steps to the next one. Or pick "
       "a CATEGORY and a STATION. Like what you hear? GRAB saves the last seconds (or bars) as a WAV in the "
       "Skywave Grabs folder at the top of the drive. BACK returns to the last place you stayed. To tune by hand, tap "
       "TUNE and turn the data wheel (or its Q-Link): a STEP each click. Every knob works that way: a tap picks it, "
       "the wheel turns it."),
    ("SCOPE", "F2", SCOPE_BLOCKS, [
        ((X0, BAR_TOP + 40, SCOPE_W, BAR_H - 40), "BARS", "signals near the dial; tap one to tune onto it"),
        ((X0, WF_TOP, SCOPE_W, WF_ROWS * WF_ROW_H), "WATERFALL", "the last seven seconds, newest on top"),
        ((X0, SPEC[1] + 2, 300, 22), "PEAK", "the strongest near the dial; how far off"),
        (INFO2, "READOUT", "frequency, mode, status"),
        (SCOPE_KEYS[GRAB], "GRAB", "save the last seconds as a WAV"),
        (SCOPE_KEYS[BACK], "BACK", "the last place you stayed"),
        (SCOPE_KEYS[DIG], "DIG", "somewhere in the CATEGORY, at random"),
        (SEEKDOWN, "SEEK DOWN", "the next signal below"),
        (SEEKUP, "SEEK UP", "the next signal above"),
        (SEEKBY, "SEEK BY", "signals, or the receiver's labels"),
        (SPAN, "SPAN", "how wide the view is"),
        (TUNE, "TUNE", "tap it, turn the wheel: a STEP a click"),
        (STEP, "STEP", "10 Hz to 1 MHz; 5, 9, 10 kHz channels"),
        (MODE, "MODE", "AM broadcast, USB/LSB voice, CW"),
        (LABEL, "LABEL", "the receiver's own station names"),
        (SCALE, "SCALE", "Auto: the strongest at the top"),
        (LISTEN, "LISTEN", "on: take a slot; off: give it back"),
        (LEVEL, "LEVEL", "volume"),
    ], "What is near you, and exactly where. The bars are the receiver's waterfall across SPAN, centred on the "
       "dial (the amber line); the white lines are the passband, what you hear. Tap a bar and the radio tunes "
       "onto the signal in it, to 10 Hz. PEAK names the strongest signal near the dial and how far off it is: "
       "turn TUNE until it reads +0 Hz. Under the bars, the last seconds as a waterfall. SCALE Auto puts the "
       "strongest at the top; 20, 40, 60 dB show how strong it really is. The scope borrows one of the "
       "receiver's few waterfalls, so it closes itself after five minutes untouched."),
    ("CLEAN", "F3", CLEAN_BLOCKS, [
        (PASSW, "PASSBAND", "what the receiver sends round the dial"),
        (REM_GLASS, "REMOVED", "how much the cleanup takes out"),
        ((TILES2[0][0], TILES2[0][1], TILE_W * 2, TILE_H), "LAMPS", "blanker and notch at work"),
        (CLEAN_KEYS[GRAB], "GRAB", "save the last seconds as a WAV"),
        (CLEAN_KEYS[BACK], "BACK", "the last place you stayed"),
        (CLEAN_KEYS[DIG], "DIG", "somewhere in the CATEGORY, at random"),
        (BANDWIDTH, "BANDWIDTH", "narrow, normal, wide"),
        (SHIFT, "SHIFT", "slide the passband up or down"),
        (MODE, "MODE", "AM broadcast, USB/LSB voice, CW"),
        (BLANKER, "BLANKER", "clips static crashes"),
        (NOTCH, "NOTCH", "removes a whistle"),
        (DENOISE, "DENOISE", "lowers the hiss"),
        (AGC, "AGC", "evens the level; off: GAIN by hand"),
        (GAIN, "GAIN", "the receiver's gain, AGC off"),
        (LISTEN, "LISTEN", "on: take a slot; off: give it back"),
        (LEVEL, "LEVEL", "volume"),
    ], "Make it usable, or don't. BANDWIDTH and SHIFT set what the receiver sends: the green strip, either "
       "side of the dial. BLANKER clips static crashes, NOTCH takes out a whistle, DENOISE lowers the hiss; "
       "the meter shows how much they remove. AGC evens the level; switch it off and GAIN sets it by hand. "
       "The noise is half the charm: leave it in when it sounds good."),
    ("LIST", "F4", LIST_BLOCKS, [
        ((ROWSW[0], ROW_Y0, ROWSW[2], ROW_H * ROWS), "ROWS", "tap one to go there; > is where you are"),
        ((ROWSW[0], ROWSW[1], ROWSW[2], HEAD_H), "HEADER", "tap, then turn the wheel to scroll"),
        (SIDE, "REGISTER", "frequency, mode, receiver"),
        (SIDE_STATUS, "STATUS", "what it is doing"),
        (LMETER_GLASS, "S-METER", "how strong; the lamps under it"),
        (LIST, "LIST", "live, stations, labels, receivers"),
        (CATEGORY, "CATEGORY", "filters STATIONS and LIVE"),
        (REGION, "REGION", "which receivers"),
        (PAGEUP, "PAGE UP", "eight rows up"),
        (PAGEDOWN, "PAGE DOWN", "eight rows down"),
        (BACK, "BACK", "the last place you stayed"),
        (DIG, "DIG", "somewhere in the CATEGORY, at random"),
        (LISTEN, "LISTEN", "on: take a slot; off: give it back"),
        (GRAB, "GRAB", "save the last seconds as a WAV"),
        (GRABLEN, "GRAB LEN", "seconds, or bars at the tempo"),
        (LEVEL, "LEVEL", "volume"),
    ], "LIVE is what this receiver hears now, of the CATEGORY: one sweep of its band, the strongest first, +dB over "
       "the noise. PAGE UP at the top sweeps again. Or the STATIONS of a CATEGORY, the receiver's own LABELS, "
       "or the RECEIVERS of a REGION (8/8 is full). Tap a row to go there; tap the header and turn the wheel to scroll."),
]
HELP_SCALE = 0.62
HELP_SHOT = (16, 52)                       # where the page's picture sits
HELP_LEGEND = (832, 52, 432, 560)


def help_spot(where, blocks):
    """Where a part's badge goes: a panel part, its top left corner; a window, astride its left edge."""
    if isinstance(where, tuple):
        x, y, w, h = where
        return x - 9, y + min(10, h / 2)                 # astride its left edge, off the first letters
    for kind, p, x, y, w in cells_xy(blocks):
        if p == where:
            return x + 14, y + 20
    raise KeyError(where)


def badge(d, x, y, n, r=11):
    d.ellipse([x - r, y - r, x + r, y + r], fill=CAUTION, outline=(24, 20, 14), width=2)
    d.text((x, y + 1), str(n), font=font("SemiBold", 13), fill=(24, 20, 14), anchor="mm")


def wrap(d, text, f, width):
    lines, line = [], ""
    for word in text.split():
        trial = (line + " " + word).strip()
        if d.textlength(trial, font=f) > width and line:
            lines.append(line)
            line = word
        else:
            line = trial
    return lines + [line]


def help_page(k, shot):
    """Help page k (from 0): the page's picture with numbered badges, the legend, a paragraph."""
    name, key, blocks, items, para = HELP[k]
    img = paint(W, H, seed=5 + k)
    d = ImageDraw.Draw(img)
    letter(d, (16, 14), "HELP  %d/%d   %s  (%s)" % (k + 1, len(HELP), name, key), 15, "la", 3)
    letter(d, (W - 16, 16), "PRESS F5 AGAIN FOR THE NEXT PAGE", 12, "ra", 2, "Medium")
    sx, sy = HELP_SHOT
    small = shot.convert("RGB").resize((int(W * HELP_SCALE), int(H * HELP_SCALE)), Image.LANCZOS)
    window(img, (sx, sy, small.width, small.height))
    img.paste(small, (sx, sy))
    d = ImageDraw.Draw(img)
    for n, (where, _, _) in enumerate(items, 1):
        cx, cy = help_spot(where, blocks)
        badge(d, sx + cx * HELP_SCALE, sy + cy * HELP_SCALE, n)
    lx, ly, lw, lh = HELP_LEGEND
    window(img, HELP_LEGEND)
    d = ImageDraw.Draw(img)
    pitch = min(30, (lh - 12) / len(items))
    fn, fd = font("SemiBold", 14), font("Regular", 14)
    for n, (_, title, text) in enumerate(items, 1):
        y = ly + 8 + (n - 1) * pitch + pitch / 2
        badge(d, lx + 18, y, n, 10)
        d.text((lx + 36, y), title, font=fn, fill=EL, anchor="lm")
        tx = lx + 36 + max(96, d.textlength(title, font=fn) + 10)
        while d.textlength(text, font=fd) > lx + lw - 10 - tx:
            text = text[:-2] + "."
        d.text((tx, y), text, font=fd, fill=LETTER, anchor="lm")
    px, py = sx, sy + small.height + 16
    pw, ph = small.width, H - py - 14
    window(img, (px, py, pw, ph))
    d = ImageDraw.Draw(img)
    fp = font("Regular", 16)
    for i, line in enumerate(wrap(d, para, fp, pw - 28)):
        d.text((px + 14, py + 10 + i * 22), line, font=fp, fill=LETTER)
    screws(d)
    return img


# ----- cells: knobs as pictures, the tuning knob ------------------------------------------------

_kit_cell = skinkit.cell_component


def cell_component(kind, w):
    """skinkit's cells, but a knob is a film strip picture, not the MPC's Knob widget, which took the
    small slide of a tap for a turn. A tap selects it (its Q-Link bank, and the data wheel's focus)
    and never moves it; the wheel, its Q-Link or the overlay (double-tap) turn it. TUNE is the tuning
    knob: the cell holds Tune, the jog, and its picture (from Tune Pos) is drawn under the cell."""
    h = CELL_H
    if kind.startswith("knob"):
        ov = "knob overlay bipolar" if kind == "knobBi" else "knob overlay"
        kids = [focus(w, h),
                entry("knob", "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0",
                                        "image": "knob_bi.png" if kind == "knobBi" else "knob.png",
                                        "numFrames": KNOB_FRAMES, "handleName": "Data"}, rect((w - KNOB2) // 2, 0, KNOB2, KNOB2)),
                label("name", "Name", rect(0, KNOB2 + 2, w, 20), skinkit.NAME)]
        if kind != "knobQuiet":
            kids.append(label("value", "Value", rect(0, KNOB2 + 22, w, 28), skinkit.VALUE))
        return component([act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", ov), act("Enter Pressed", "Show Overlay", ov)], kids)
    if kind == "tune":
        return component([act("Mouse Down", "Q-Link")], [focus(w, h), label("name", "Name", rect(0, VFO + 1, w, h - VFO - 1), skinkit.NAME)])
    return _kit_cell(kind, w)


skinkit.cell_component = cell_component


def tune_knobs(blocks):
    """The tuning knob's picture, under its cell: it turns with Tune Pos."""
    return [entry("tune knob", "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": "vfo.png",
                                         "numFrames": KNOB_FRAMES, "handleName": "Data"}, rect(x + (w - VFO) // 2, y, VFO, VFO),
                  {"Data": "Parameter %d" % TUNEPOS}) for kind, p, x, y, w in cells_xy(blocks) if kind == "tune"]


# ----- TUI.json --------------------------------------------------------------------------------

def tui(params):
    cell_defs = {}
    fx, fy, fw, fh = FREQ
    root = [image("panel", "panel.png", rect(0, 0, W, H)),
            thumb_slider("lon", LON, rect(*MAP), "Horizontal", "cross_v.png"),
            thumb_slider("lat", LAT, rect(*MAP), "Vertical", "cross_h.png"),
            thumb_slider("needle", DIAL, rect(*DIALG), "Horizontal", "needle.png"),
            thumb_slider("signal", SIGNAL, rect(*METER), "Horizontal", "blank.png", "meter.png"),
            label("frequency", "Value", rect(fx + 96, fy + 4, fw - 112, 100), text_style("Nexa", "Light", 72, A_EL, "right verticallyCentred"),
                  {"Data": "Parameter %d" % DIAL}),     # Dial: told to the host whenever any part of the frequency moves
            label("receiver", "Value", rect(fx + 98, fy + 108, fw - 314, 44), text_style("Titillium Web", "SemiBold", 24, A_EL, "left verticallyCentred"),
                  {"Data": "Parameter %d" % RECEIVER}),
            label("mode", "Value", rect(fx + fw - 128, fy + 108, 112, 44), text_style("Nexa", "Bold", 22, A_EL, "right verticallyCentred", "Upper Case"),
                  {"Data": "Parameter %d" % MODE}),
            label("status", "Value", rect(INFO[0] + 14, INFO[1], INFO[2] - 24, INFO[3]),
                  text_style("Titillium Web", "Regular", 19, A_EL, "left verticallyCentred"), {"Data": "Parameter %d" % STATUS})]
    root += touch_keys(TOP_KEYS, params) + lamp_tiles(TILES) + tune_knobs(BLOCKS)
    root.append(entry("power", "Button", {"version": 2, "onImage": "power_on_top.png", "offImage": "power_off_top.png", "buttonId": 1,
                                          "numButtonsInGroup": 1, "handleName": "Data", "gestureBehaviour": "Instant"},
                      rect(*POWER), {"Data": "Parameter %d" % LISTEN}))
    radio = control_tab("RADIO", 0, root, BLOCKS, cell_defs, params)

    # SCOPE: 64 bars over Span, centred on the dial (a bar touched tunes onto its signal), the
    # waterfall under them (cells the bars' columns wide, a film strip of shades each), the dial's
    # line and the passband's edges over both, the bars last so a touch finds them
    sx, sy, sw, sh = SPEC
    ix, iy, iw, ih = INFO2
    scope = [image("panel", "scope.png", rect(0, 0, W, H))]
    for r in range(WF_ROWS):
        for c in range(WF_COLS):
            scope.append(entry("wf %d %d" % (r + 1, c + 1), "Image",
                               {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": "wf.png", "numFrames": WF_LEVELS, "handleName": "Data"},
                               rect(X0 + c * COL_W, WF_TOP + r * WF_ROW_H, COL_W, WF_ROW_H), {"Data": "Parameter %d" % (WF0 + r * WF_COLS + c)}))
    mark_h = SCOPE_BOT - (BAR_TOP - 4)
    scope.append(image("dial line", "dial_mark.png", rect(X0 + SCOPE_W // 2 - 1, BAR_TOP - 4, 2, mark_h)))
    for name, p in (("pass lo", SPASSLO), ("pass hi", SPASSHI)):   # a 2 px thumb on a track 2 px wider: its middle is at X0 + v * width
        scope.append(thumb_slider(name, p, rect(X0 - 1, BAR_TOP - 4, SCOPE_W + 2, mark_h), "Horizontal", "pass_mark.png"))
    for b in range(BINS):
        scope.append(thumb_slider("bar %d" % (b + 1), BIN0 + b, rect(bar_x(b), BAR_TOP, BAR_W, BAR_H), "Vertical", "blank.png", "bar.png"))
    scope += [label("peak", "Value", rect(X0 + 40, sy + 2, 760, 22), text_style("Titillium Web", "SemiBold", 17, A_EL, "left verticallyCentred"),
                    {"Data": "Parameter %d" % PEAK}),
              label("lo", "Value", rect(sx + 12, SCOPE_BOT + 1, 160, 20), text_style("Titillium Web", "Regular", 15, A_EL_DIM, "left verticallyCentred"),
                    {"Data": "Parameter %d" % SCOPELO}),
              label("hi", "Value", rect(sx + sw - 172, SCOPE_BOT + 1, 160, 20), text_style("Titillium Web", "Regular", 15, A_EL_DIM, "right verticallyCentred"),
                    {"Data": "Parameter %d" % SCOPEHI}),
              label("frequency", "Value", rect(ix + 16, iy, 380, ih), text_style("Nexa", "Light", 44, A_EL, "left verticallyCentred"),
                    {"Data": "Parameter %d" % DIAL}),
              label("mode", "Value", rect(ix + 396, iy, 70, ih), text_style("Nexa", "Bold", 18, A_EL_DIM, "left verticallyCentred", "Upper Case"),
                    {"Data": "Parameter %d" % MODE}),
              label("status", "Value", rect(ix + 520, iy, iw - 536, ih), text_style("Titillium Web", "Regular", 20, A_EL, "right verticallyCentred"),
                    {"Data": "Parameter %d" % STATUS})]
    scope += touch_keys(SCOPE_KEYS, params) + tune_knobs(SCOPE_BLOCKS)
    scope_tab = control_tab("SCOPE", 1, scope, SCOPE_BLOCKS, cell_defs, params)

    # CLEAN: the passband between two amber edges (Pass Hi reveals the fill from the left, Pass Lo
    # masks it back to its own edge), the grid over both; what the cleanup removes; its two tiles
    px, py, pw, ph = PASSW
    ix, iy, iw, ih = INFO3
    edge_text = text_style("Titillium Web", "SemiBold", 17, A_EL, "left verticallyCentred")
    clean = [image("panel", "clean.png", rect(0, 0, W, H)),
             thumb_slider("pass hi", PASSHI, rect(*BAND), "Horizontal", "pass_edge.png", "pass_fill.png"),
             thumb_slider("pass lo", PASSLO, rect(*BAND), "Horizontal", "pass_edge.png", "pass_mask.png"),
             image("pass grid", "pass_grid.png", rect(*BAND)),
             thumb_slider("removed", REMOVED, rect(*REM), "Horizontal", "blank.png", "rem_meter.png"),
             label("lo", "Value", rect(px + 14, py + ph - 26, 240, 22), edge_text, {"Data": "Parameter %d" % PASSLO}),
             label("hi", "Value", rect(px + pw - 254, py + ph - 26, 240, 22),
                   text_style("Titillium Web", "SemiBold", 17, A_EL, "right verticallyCentred"), {"Data": "Parameter %d" % PASSHI}),
             label("frequency", "Value", rect(ix + 16, iy, 380, ih), text_style("Nexa", "Light", 44, A_EL, "left verticallyCentred"),
                   {"Data": "Parameter %d" % DIAL}),
             label("mode", "Value", rect(ix + 396, iy, 70, ih), text_style("Nexa", "Bold", 18, A_EL_DIM, "left verticallyCentred", "Upper Case"),
                   {"Data": "Parameter %d" % MODE}),
             label("status", "Value", rect(ix + 520, iy, iw - 536, ih), text_style("Titillium Web", "Regular", 20, A_EL, "right verticallyCentred"),
                   {"Data": "Parameter %d" % STATUS})]
    for k, (name, p) in enumerate((("blank", BLANKING), ("notch", NOTCHING))):
        x, y = TILES2[k]
        clean.append(entry("tile " + name, "Indicator",
                           {"version": 1, "onImage": "tile_%s_on.png" % name, "offImage": "tile_%s.png" % name, "indicatorId": 1,
                            "numIndicatorsInGroup": 2, "handleName": "Data"}, rect(x, y, TILE_W, TILE_H), {"Data": "Parameter %d" % p}))
    clean += touch_keys(CLEAN_KEYS, params)
    clean_tab = control_tab("CLEAN", 2, clean, CLEAN_BLOCKS, cell_defs, params)

    # LIST: the header says which list and where in it (Scroll's text); each row is a component that
    # flips its parameter when touched, and shows the entry (the parameter's text)
    rx, ry, rw, rh = ROWSW
    cell_defs["list row"] = component([act("Mouse Down", "Toggle Switch")],
                                      [focus(rw, ROW_H),
                                       label("entry", "Value", rect(18, 0, rw - 36, ROW_H),
                                             text_style("Titillium Web", "SemiBold", 25, A_EL, "left verticallyCentred"))])
    cell_defs["list wheel"] = component([act("Mouse Down", "Q-Link")], [focus(rw, HEAD_H)])
    sx, sy, sw, sh = SIDE
    lst = [image("panel", "list.png", rect(0, 0, W, H)),
           label("header", "Value", rect(rx + 16, ry, rw - 260, HEAD_H), text_style("Nexa", "Bold", 19, A_EL, "left verticallyCentred", "Upper Case"),
                 {"Data": "Parameter %d" % SCROLL}),
           entry("wheel", "list wheel", {"version": 1, "handleName": "Data"}, rect(rx, ry, rw, HEAD_H),
                 {"Data": "Parameter %d" % WHEEL}, focus="Yes"),
           label("frequency", "Value", rect(sx + 12, sy + 16, sw - 24, 72), text_style("Nexa", "Light", 42, A_EL, "right verticallyCentred"),
                 {"Data": "Parameter %d" % DIAL}),
           label("mode", "Value", rect(sx + 60, sy + 4, sw - 72, 20), text_style("Nexa", "Bold", 15, A_EL_DIM, "right verticallyCentred", "Upper Case"),
                 {"Data": "Parameter %d" % MODE}),
           label("receiver", "Value", rect(sx + 12, sy + 108, sw - 24, 34), text_style("Titillium Web", "SemiBold", 19, A_EL, "left verticallyCentred"),
                 {"Data": "Parameter %d" % RECEIVER}),
           label("status", "Value", rect(SIDE_STATUS[0] + 12, SIDE_STATUS[1], SIDE_STATUS[2] - 24, SIDE_STATUS[3]),
                 text_style("Titillium Web", "Regular", 17, A_EL, "left verticallyCentred"), {"Data": "Parameter %d" % STATUS}),
           thumb_slider("signal", SIGNAL, rect(*LMETER), "Horizontal", "blank.png", "list_meter.png")]
    lst += lamp_tiles(LTILES)
    for r in range(ROWS):
        lst.append(entry("row %d" % (r + 1), "list row", {"version": 1, "handleName": "Data"}, rect(rx, ROW_Y0 + r * ROW_H, rw, ROW_H),
                         {"Data": "Parameter %d" % (ROW0 + r)}))      # no wheel focus: it stays on the header
    list_tab = control_tab("LIST", 3, lst, LIST_BLOCKS, cell_defs, params)
    return [radio, scope_tab, clean_tab, list_tab], cell_defs


def touch_keys(keys, params):
    """Keys on the panel's face (not a Q-Link cell): each press flips its parameter, the plugin acts on the change."""
    return [entry("key " + params[p]["name"].lower(), "Button",
                  {"version": 2, "onImage": KEY_IMG[p], "offImage": KEY_IMG[p], "buttonId": 1, "numButtonsInGroup": 1,
                   "handleName": "Data", "gestureBehaviour": "Instant"}, rect(*box), {"Data": "Parameter %d" % p}) for p, box in keys.items()]


def lamp_tiles(tiles):
    """CALLING, ON AIR, FAULT from Link; SIGNAL from the lamp."""
    out = []
    for k, name in ((1, "calling"), (2, "onair"), (3, "fault")):
        x, y = tiles[k - 1]
        out.append(entry("tile " + name, "Indicator",
                         {"version": 1, "onImage": "tile_%s_on.png" % name, "offImage": "tile_%s.png" % name, "indicatorId": k,
                          "numIndicatorsInGroup": 4, "handleName": "Data"}, rect(x, y, TILE_W, TILE_H), {"Data": "Parameter %d" % LINK}))
    x, y = tiles[3]
    out.append(entry("tile signal", "Indicator",
                     {"version": 1, "onImage": "tile_signal_on.png", "offImage": "tile_signal.png", "indicatorId": 1,
                      "numIndicatorsInGroup": 2, "handleName": "Data"}, rect(x, y, TILE_W, TILE_H), {"Data": "Parameter %d" % LAMP}))
    return out


def help_tabs():
    """F5: one sub-tab per page; pressing F5 again steps through them. The knobs stay the radio's."""
    return [{"version": 1, "tabName": "HELP", "fnKeyIndex": 4, "fnKeySubIndex": k,
             "componentDefinition": component([], [image("help", "help_%d.png" % (k + 1), rect(0, 0, W, H))], "ff000000"),
             "qlinkBoundsData": []} for k in range(len(HELP))]


# what the help pages' pictures show: a radio on air on the 31 m band, rather than a fresh instance
SHOW = [(DIAL, 9650.0 / 30000), (DIAL, "9650.00 kHz"), (STEP, "5 kHz"), (TUNEPOS, 0.3), (SCALE, "Auto"), (MODE, "AM"), (RECEIVER, "Kansas, USA"),
        (STATUS, "on air: idle 30 min"), (SIGNAL, 0.62), (LINK, 2 / 3), (LON, (180 - 97.0) / 360), (LAT, (90 + 38.9) / 180),
        (LISTEN, 1.0), (CATEGORY, "Broadcast"), (STATION, "31m band"), (REGION, "N America"), (GRABLEN, "10 s"),
        (SCOPELO, "9620.7"), (SCOPEHI, "9679.3"), (LABEL, "9650.0 broadcast"), (SPAN, "59 kHz"),
        (PEAK, "9650.00 kHz +44 dB (+0 Hz)"), (SPASSLO, 0.5 - 4.9 / 58.59), (SPASSHI, 0.5 + 4.9 / 58.59),
        (PASSLO, (6000 - 4900) / 12000), (PASSLO, "-4900 Hz"), (PASSHI, (6000 + 4900) / 12000), (PASSHI, "+4900 Hz"),
        (REMOVED, 0.3), (LIST, "Live"), (SCROLL, "Live now  1-8 of 64"), (LAMP, 1.0), (CATEGORY, "Talk & Music")]
PEAKS = ((0.92, 31.5, 2.2), (0.55, 12, 1.4), (0.7, 47, 1.8), (0.4, 57, 1.0))   # height, bar, width
SHOW += [(BIN0 + b, max([0.05] + [a * math.exp(-((b - c) / w) ** 2) for a, c, w in PEAKS])) for b in range(BINS)]
SHOW += [(WF0 + r * WF_COLS + c, round(min(1.0, max([0.0] + [a * (0.75 + 0.25 * math.sin(r * 1.7 + c * 0.9 + k))
                                                                 * math.exp(-((c - cc) / w) ** 2) for k, (a, cc, w) in enumerate(PEAKS)])
                                            + (0.1 if ((r * 73856093) ^ (c * 19349663)) % 7 == 0 else 0.0)) * (WF_LEVELS - 1)) / (WF_LEVELS - 1))
         for r in range(WF_ROWS) for c in range(WF_COLS)]
SHOW += [(ROW0 + r, t) for r, t in enumerate(["5935.0 AM +50 49m band", "> 9650.0 AM +44 31m band", "4625.0 USB +38 The Buzzer",
                                               "7350.0 AM +37 41m band", "8846.0 USB +33 MWARA NAT-B", "11175.0 USB +30 HFGCS 11175",
                                               "7074.0 USB +28 40m ham", "6604.0 USB +25 NY VOLMET 6604"])]


def main():
    land = sys.argv[1]
    params = json.load(open(os.path.join(HERE, "build", "params.json")))["params"]
    assert params[BIN0]["name"] == "Bin 1", "build first: params.json is older than the scope's parameters"
    assert len(params) > REMOVED and params[REMOVED]["name"] == "Removed", "build first: params.json is older than the clean page"
    assert params[WHEEL]["name"] == "Wheel", "build first: params.json is older than the wheel"
    assert len(params) == WF0 + WF_ROWS * WF_COLS and params[TUNE]["name"] == "Tune" and params[WF0]["name"] == "WF 1.1", \
        "build first: params.json is older than Tune and the waterfall"
    start(OUT)
    knob_strip(False).save(os.path.join(OUT, "knob.png"), optimize=True)
    knob_strip(True).save(os.path.join(OUT, "knob_bi.png"), optimize=True)
    draw_background(land).save(os.path.join(OUT, "panel.png"), optimize=True)
    draw_scope_background().save(os.path.join(OUT, "scope.png"), optimize=True)
    bar_image().save(os.path.join(OUT, "bar.png"))
    vfo_strip().save(os.path.join(OUT, "vfo.png"), optimize=True)
    wf_strip().save(os.path.join(OUT, "wf.png"))
    mark(2, SCOPE_BOT - (BAR_TOP - 4), CAUTION, 220).save(os.path.join(OUT, "dial_mark.png"))
    mark(2, SCOPE_BOT - (BAR_TOP - 4), LETTER, 150).save(os.path.join(OUT, "pass_mark.png"))
    draw_meter(True).save(os.path.join(OUT, "meter.png"))
    line_thumb(THUMB, MAP[3], True).save(os.path.join(OUT, "cross_v.png"))
    line_thumb(MAP[2], THUMB, False).save(os.path.join(OUT, "cross_h.png"))
    needle().save(os.path.join(OUT, "needle.png"))
    power(False).save(os.path.join(OUT, "power_off.png"))
    power(True).save(os.path.join(OUT, "power_on.png"))
    power(False, POWER[2], POWER[3]).save(os.path.join(OUT, "power_off_top.png"))
    power(True, POWER[2], POWER[3]).save(os.path.join(OUT, "power_on_top.png"))
    guarded_key("GRAB").save(os.path.join(OUT, "grab.png"))
    dsky_key("SEEK", "left").save(os.path.join(OUT, "seek_down.png"))
    dsky_key("SEEK", "right").save(os.path.join(OUT, "seek_up.png"))
    dsky_key("PAGE", "up").save(os.path.join(OUT, "page_up.png"))
    dsky_key("PAGE", "down").save(os.path.join(OUT, "page_down.png"))
    dsky_key("BACK", "left").save(os.path.join(OUT, "back.png"))
    dsky_key("DIG").save(os.path.join(OUT, "dig.png"))
    for p, (img, name) in {SEEKDOWN: (dsky_key("SEEK", "left", KEY_W, 52), "seek_down_top.png"),
                           SEEKUP: (dsky_key("SEEK", "right", KEY_W, 52), "seek_up_top.png"),
                           BACK: (dsky_key("BACK", "left", KEY_W, 52), "back_top.png"),
                           DIG: (dsky_key("DIG", None, KEY_W, 52), "dig_top.png"),
                           GRAB: (red_key("GRAB"), "grab_top.png")}.items():
        img.save(os.path.join(OUT, name))
        KEY_IMG[p] = name
    draw_meter(True, LMETER).save(os.path.join(OUT, "list_meter.png"))
    draw_list_background().save(os.path.join(OUT, "list.png"), optimize=True)
    for name, text, colour in (("calling", "CALLING", CAUTION), ("onair", "ON AIR", LETTER), ("fault", "FAULT", WARNING), ("signal", "SIGNAL", EL)):
        tile(text, None).save(os.path.join(OUT, "tile_%s.png" % name))
        tile(text, colour).save(os.path.join(OUT, "tile_%s_on.png" % name))
    draw_clean_background().save(os.path.join(OUT, "clean.png"), optimize=True)
    pass_fill().save(os.path.join(OUT, "pass_fill.png"))
    Image.new("RGBA", BAND[2:], BLACK + (255,)).save(os.path.join(OUT, "pass_mask.png"))
    pass_grid().save(os.path.join(OUT, "pass_grid.png"))
    edge_thumb().save(os.path.join(OUT, "pass_edge.png"))
    draw_rem_meter(True).save(os.path.join(OUT, "rem_meter.png"))
    for name, text in (("blank", "BLANK"), ("notch", "NOTCH")):
        tile(text, None).save(os.path.join(OUT, "tile_%s.png" % name))
        tile(text, CAUTION).save(os.path.join(OUT, "tile_%s_on.png" % name))
    params[SCROLL]["choices"] = []      # its text is a place in the list: the host's sample of it is no list of choices
    params[WHEEL]["choices"] = [str(k) for k in range(-16, 17)]   # read as an int; its steps make the data wheel go a row a step
    params[TUNE]["choices"] = [str(k) for k in range(-16, 17)]    # and Tune a STEP a step
    params[PEAK]["choices"] = []        # its text is the peak's: the host's sample of it is no list of choices
    tabs, cell_defs = tui(params)
    write_tui(os.path.join(OUT, "TUI.json"), tabs, cell_defs, params)
    for k in range(len(HELP)):          # the pages as drawn so far, then the help over them
        help_page(k, preview.render(HERE, k + 1, SHOW)).save(os.path.join(OUT, "help_%d.png" % (k + 1)), optimize=True)
    write_tui(os.path.join(OUT, "TUI.json"), tabs + help_tabs(), cell_defs, params)
    write_qlinks(os.path.join(OUT, "Q-Links.json"),
                 [BLOCKS, SCOPE_BLOCKS, CLEAN_BLOCKS, LIST_BLOCKS] + [(5, k + 1, BLOCKS) for k in range(len(HELP))],
                 [p for _, cs in BLOCKS for _, p, _ in cs])
    print("skin written to", OUT)


if __name__ == "__main__":
    main()
