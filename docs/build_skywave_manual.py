#!/usr/bin/env python
"""databurn DB-WP-009 -- Skywave: the operator's manual.

Everything on these pages is read from the plugin when the manual is built, so it cannot drift:
the panel pictures are the skin itself, drawn by mpc/skin/preview.py with make_skin.py's SHOW
values; the numbered parts and their one-line meanings are make_skin.py's HELP (the same lines the
MPC's own HELP pages show); the controls table is read from build/params.json; the receivers are
Content/receivers.js through the plugin's own parser (receivers_json.cpp, compiled here); the
stations, steps, spans, passbands and timings are parsed out of src/Skywave.cpp. verify_claims()
asserts every number the prose states before a page is drawn.

    python ../../../sdk/tools/build.py plugins/skywave   (from mpc/: params.json and the skin)
    python build_skywave_manual.py [rally|engn|rally-dark|engn-dark|all]

Needs reportlab, fitz, Pillow, numpy, and clang++ on PATH for receivers_json.cpp.
"""
import json
import math
import os
import re
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
PLUG = os.path.normpath(os.path.join(_HERE, ".."))
REPO = os.path.normpath(os.path.join(PLUG, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "plugins", "docs_kit"))
sys.path.insert(0, PLUG)
from manual_kit import bm, install_glyph_guard, fig_head, caption, PW, PH, M, CW  # noqa: E402

from reportlab.pdfgen import canvas  # noqa: E402
from reportlab.lib.pagesizes import A4  # noqa: E402
from reportlab.lib.utils import ImageReader  # noqa: E402
from reportlab.pdfbase import pdfmetrics  # noqa: E402

import make_skin as ms  # noqa: E402  the skin's layout, HELP and SHOW
import preview  # noqa: E402  draws a page of the skin

install_glyph_guard()

bm.DOC_ID = "DB-WP-009"
bm.DOC_REV = "REVISED 11.16.2026"
bm.PRODUCT = "SKYWAVE"
FOOTER = "SKYWAVE"

SRC = open(os.path.join(PLUG, "src", "Skywave.cpp"), encoding="utf-8").read()
CLEAN_H = open(os.path.join(PLUG, "src", "clean.h"), encoding="utf-8").read()
README = open(os.path.join(PLUG, "README.md"), encoding="utf-8").read()
MAKE_SKIN = open(os.path.join(PLUG, "make_skin.py"), encoding="utf-8").read()
PJ = json.load(open(os.path.join(PLUG, "build", "params.json"), encoding="utf-8"))
PARAMS = PJ["params"]
BY = {p["name"]: p for p in PARAMS}
TUI = json.load(open(os.path.join(PLUG, "skins", "TUI.json"), encoding="utf-8"))["pageData"]
LIST_JS = os.path.join(PLUG, "Content", "receivers.js")


# ================================================== what the plugin says =====
def src_num(pattern, text=SRC, cast=float):
    m = re.search(pattern, text)
    assert m, pattern
    return cast(m.group(1))


def stations():
    """kStations without Dial: (name, kHz, mode, category enum)."""
    block = SRC[SRC.index("const Station kStations[] = {"):SRC.index("constexpr int kNumStations")]
    rows = re.findall(r'\{"([^"]+)", ([0-9.]+), k(AM|LSB|USB|CW|NBFM), (kCat\w+|-1)\}', block)
    return [(n, float(f), md, c) for n, f, md, c in rows if c != "-1"]


def categories():
    """(enum, shown name) in order."""
    enum = re.search(r"enum Category \{([^}]*)\}", SRC).group(1)
    keys = [k.strip() for k in enum.replace("\n", " ").split(",") if k.strip() and k.strip() != "kNumCats"]
    names = re.findall(r'"([^"]+)"', re.search(r"kCatNames\[kNumCats\] = \{([^}]*)\}", SRC).group(1))
    assert len(keys) == len(names), (keys, names)
    return list(zip(keys, names))


def passbands():
    block = SRC[SRC.index("const int kPass[5][3][2] = {"):]
    block = block[:block.index("};")]
    body = re.sub(r"//[^\n]*", "", block.split("=", 1)[1])   # the comments carry numbers too
    nums = [int(x) for x in re.findall(r"-?\d+", body)]
    assert len(nums) == 30, nums
    return [[(nums[m * 6 + w * 2], nums[m * 6 + w * 2 + 1]) for w in range(3)] for m in range(5)]


def receivers():
    """The receivers the plugin would call, through its own parser."""
    exe_dir = os.path.join(PLUG, "build", "docs")
    os.makedirs(exe_dir, exist_ok=True)
    exe = os.path.join(exe_dir, "receivers_json.exe" if os.name == "nt" else "receivers_json")
    cpp = os.path.join(_HERE, "receivers_json.cpp")
    hdr = os.path.join(PLUG, "src", "kiwi_list.h")
    if not os.path.exists(exe) or os.path.getmtime(exe) < max(os.path.getmtime(cpp), os.path.getmtime(hdr)):
        subprocess.run(["clang++", "-std=c++17", "-O1", "-I", os.path.join(PLUG, "src"), cpp, "-o", exe], check=True)
    out = subprocess.run([exe, LIST_JS], capture_output=True, check=True)
    return json.loads(out.stdout.decode("utf-8"))


def list_date():
    head = open(LIST_JS, encoding="utf-8").read(400)
    m = re.search(r"list timestamp: \w+, (\d+)-(\w+)-(\d{4})", head)
    months = "Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec".split()
    return f"{months.index(m.group(2)) + 1:02d}.{int(m.group(1)):02d}.{m.group(3)}"


RX = receivers()
REGIONS = BY["Region"]["choices"]          # All, then the six regions
ST = stations()
CATS = categories()
STEPS = BY["Step"]["choices"]
STEP_HZ = [int(x) for x in re.findall(r"\d+", re.search(r"kStepHz\[\] = \{([^}]*)\}", SRC).group(1))]
SPANS = BY["Span"]["choices"]
LENS = BY["Grab Len"]["choices"]
PASS = passbands()
MODES = BY["Mode"]["choices"]
LIST_DATE = list_date()

LIVE_MAX = src_num(r"kLiveMax = (\d+)", cast=int)
LIVE_FRESH = src_num(r"kLiveFresh = std::chrono::minutes\((\d+)\)", cast=int)
IDLE_MIN = src_num(r"kIdle = std::chrono::minutes\((\d+)\)", cast=int)
SCOPE_IDLE = src_num(r"kScopeIdle = std::chrono::minutes\((\d+)\)", cast=int)
SESSION_S = src_num(r"kSessionEnded = seconds\((\d+)\)", cast=int)
MAX_TRIES = src_num(r"kMaxTries = (\d+)", cast=int)
DIG_TRIES = src_num(r"kDigTries = (\d+)", cast=int)
BUFFER_S = src_num(r"kBufferSecs = ([0-9.]+)")
BINS = src_num(r"kBins = (\d+);", cast=int)
WF_ROWS = src_num(r"kWfRows = (\d+)", cast=int)
WF_PUSH_MS = src_num(r"lastWfPush_ >= milliseconds\((\d+)\)", cast=int)
ROWS = src_num(r"kRows = (\d+);", cast=int)
CW_PITCH = src_num(r"kCwPitchKHz = ([0-9.]+)")
GAIN_MAX = src_num(r"const double gain = std::min\(([0-9.]+), ")
GAIN_REF = src_num(r"std::min\([0-9.]+, ([0-9.]+) / peak\)")
GRAB_MAX_S = src_num(r"\(int64_t\)\(([0-9.]+) \* rate\), held")
RING_BITS = src_num(r"kGrabRingBits = (\d+)", cast=int)
SEEK_DB = src_num(r"const int strong = rowFloor_ \+ (\d+);", cast=int)
BAR_DB = src_num(r"rowFloor_ < (\d+)\) \{ setTune", cast=int)
LAMP_DB = src_num(r"on = was \? over >= [0-9.]+ : over >= ([0-9.]+);")
AGC_MS = [int(x) for x in re.findall(r"\d+", re.search(r"kAgcDecayMs\[\] = \{([^}]*)\}", SRC).group(1))]
BLANK_MS = src_num(r"kHold = \d+;\s+// (\d+) ms", CLEAN_H, int)
SPAN_DEFAULT = SPANS[src_num(r"kSpanDefault = (\d+);", cast=int)]


def regions():
    """(name, count) for each region the plugin sorts receivers into."""
    return [(REGIONS[i], sum(1 for r in RX if r["region"] == i)) for i in range(1, len(REGIONS))]


def per_cat():
    """category name -> its presets, in order."""
    names = dict(CATS)
    out = {n: [] for _, n in CATS}
    for s in ST:
        out[names[s[3]]].append(s)
    return out


def step_tune(f_khz, step_hz, d):
    """src/Skywave.cpp stepTune(): off the grid, the first click lands on it."""
    step = step_hz / 1000.0
    g = f_khz / step
    on = round(g)
    base = on if abs(g - on) < 1e-3 else (math.floor(g) if d > 0 else math.ceil(g))
    return round(min(max((base + d) * step, 0.0), 29999.99), 3)


# ================================================== drawing helpers =====
def badge(c, x, y, n, r=6.4):
    c.setFillColor(bm.ACCENT)
    c.circle(x, y, r, stroke=0, fill=1)
    c.setFillColor(bm.PAPER)
    c.setFont("Mono-Bold", 6.6 if n < 10 else 5.8)
    c.drawCentredString(x, y - 2.3, str(n))


def page_shot(c, k, top):
    """Page k of the skin (from 0), full width, its parts badged as HELP numbers them. Returns its bottom."""
    img = preview.render(PLUG, k + 1, ms.SHOW).convert("RGB")
    w = CW
    s = w / img.width
    h = img.height * s
    c.drawImage(ImageReader(img), M, top - h, w, h)
    c.setStrokeColor(bm.TEXT)
    c.setLineWidth(0.6)
    c.rect(M, top - h, w, h, stroke=1, fill=0)
    _, _, blocks, items, _ = ms.HELP[k]
    for n, (where, _, _) in enumerate(items, 1):
        px, py = ms.help_spot(where, blocks)
        badge(c, M + max(7.0, px * s), top - max(7.0, py * s), n)
    return top - h


def bullets(c, y, items, x=M, w=CW, size=9.5, leading=13.0):
    for it in items:
        lines = bm.wrap_text(it, "Serif", size, w - 12)
        c.setFont("Serif", size)
        c.setFillColor(bm.TEXT)
        c.drawString(x, y, "-")
        for ln in lines:
            c.drawString(x + 12, y, ln)
            y -= leading
        y -= 2
    return y


def page_parts(c, pno, ptotal, k, tab, intro):
    bm.blank_page(c)
    tb = bm.draw_tab(c, tab)
    name, key, _, items, para = ms.HELP[k]
    y = page_shot(c, k, tb - 18)
    y = caption(c, M, y - 14, CW, f"the {name} page ({key}), drawn from the skin as it ships, its parts numbered as its "
                                  "own HELP page numbers them.")
    rows = [["", "PART", "WHAT IT SHOWS OR DOES"]] + [[str(n), t, d] for n, (_, t, d) in enumerate(items, 1)]
    y = bm.table(c, M, y - 6, [20, 82, CW - 102], rows, size=8, pad=2.6)
    text = intro or para
    if y - 16 - len(bm.wrap_text(text, "Serif", 9.5, CW)) * 12.5 > 46:
        bm.para(c, M, y - 16, text, CW, leading=12.5)
    bm.draw_footer(c, pno, ptotal, FOOTER)


# ================================================== pages =====
def page_cover(c, pno, ptotal):
    bm.blank_page(c)
    ART_H = 430
    ay = PH - ART_H
    c.setFillColor(bm.ART_BG)
    c.rect(0, ay, PW, ART_H, stroke=0, fill=1)
    for rx, ry in [(20, PH - 20), (PW - 20, PH - 20), (20, ay + 20), (PW - 20, ay + 20)]:
        bm.reg_mark(c, rx, ry)
    # the art: every receiver in the shipped list where it stands, and the 0 to 30 MHz dial under it
    mx0, mx1, my1 = 70, PW - 70, PH - 132
    lat_top, lat_bot = 78.0, -58.0
    mh = (mx1 - mx0) * (lat_top - lat_bot) / 360.0
    my0 = my1 - mh

    def xy(lat, lon):
        return mx0 + (lon + 180.0) / 360.0 * (mx1 - mx0), my0 + (lat - lat_bot) / (lat_top - lat_bot) * mh

    c.setStrokeColor(bm.ART_FG)
    c.setLineWidth(0.25)
    for lon in range(-180, 181, 30):
        c.line(xy(0, lon)[0], my0, xy(0, lon)[0], my1)
    for lat in (-30, 0, 30, 60):
        c.line(mx0, xy(lat, 0)[1], mx1, xy(lat, 0)[1])
    c.setFillColor(bm.ART_FG)
    for r in RX:
        if lat_bot <= r["lat"] <= lat_top:
            x, y = xy(r["lat"], r["lon"])
            c.circle(x, y, 1.05, stroke=0, fill=1)
    # the cross, where the panel's pictures put it: Kansas
    lon_k = dict(ms.SHOW)[ms.LON] * 360.0 - 180.0
    lat_k = dict(ms.SHOW)[ms.LAT] * 180.0 - 90.0
    kx, ky = xy(lat_k, lon_k)
    c.setStrokeColor(bm.ACCENT)
    c.setLineWidth(0.8)
    c.line(kx, my0, kx, my1)
    c.line(mx0, ky, mx1, ky)
    # the dial
    dy = my0 - 46
    c.setStrokeColor(bm.ART_FG)
    c.setLineWidth(0.5)
    c.line(mx0, dy, mx1, dy)
    c.setFont("Mono", 6)
    c.setFillColor(bm.ART_FG)
    for mhz in range(0, 31):
        x = mx0 + mhz / 30.0 * (mx1 - mx0)
        c.line(x, dy, x, dy + (5 if mhz % 5 == 0 else 2.5))
        if mhz % 5 == 0:
            c.drawCentredString(x, dy - 9, str(mhz))
    c.setStrokeColor(bm.ART_WHISPER)
    c.setLineWidth(2.2)
    for name, f, _, _ in ST:
        if name.endswith(" band"):
            x = mx0 + f / 30000.0 * (mx1 - mx0)
            c.line(x - 2.5, dy + 9, x + 2.5, dy + 9)
    c.setFont("Mono", 7)
    c.setFillColor(bm.ART_WHISPER)
    c.drawString(mx0, ay + 48, f"EVERY RECEIVER IN THE LIST SHIPPED {LIST_DATE}: {len(RX)}. THE CROSS: KANSAS, USA, AS ON THE PANEL")
    c.drawString(mx0, ay + 36, "UNDER THEM THE DIAL, 0 TO 30 MHZ, THE BROADCAST METRE BANDS MARKED")
    c.drawString(mx1 + 8, dy - 2, "MHZ")

    bm.draw_tab(c, ["OPERATOR'S", "MANUAL"], size=13)
    c.setStrokeColor(bm.TEXT)
    c.setLineWidth(1.6)
    c.line(0, ay, PW, ay)
    wm_y = ay - 82
    c.setFont("Display-Black", 54)
    c.setFillColor(bm.TEXT)
    c.drawString(M, wm_y, "SKYWAVE")
    wm_w = pdfmetrics.stringWidth("SKYWAVE", "Display-Black", 54)
    c.setFillColor(bm.ACCENT)
    c.rect(M, wm_y - 12, wm_w, 3, stroke=0, fill=1)
    cat_y = wm_y - 34
    bm.tracked_string(c, M, cat_y, "SHORTWAVE RADIO FOR SAMPLING, FOR THE MPC", "Mono-Bold", 10, 1.4, bm.TEXT)
    model_y = cat_y - 20
    c.setFont("Serif-Italic", 12)
    c.drawString(M, model_y, "Model of 2026")
    feat = (f"{len(RX)} public KiwiSDR receivers in {len(REGIONS) - 1} regions of the world · 0 to 30 MHz, tuned in steps of "
            f"{STEPS[0]} to {STEPS[-1]} · {len(ST)} presets in {len(CATS)} categories named for how they sound · a Live "
            f"sweep of what the receiver hears now, its strongest {LIVE_MAX} signals · a scope of {BINS} bars over a "
            "waterfall · GRAB: the last seconds or bars you heard, as a WAV, ready for the sampler")
    lines = bm.wrap_text(feat, "Serif-Italic", 10, CW - 24)
    ft, fl = model_y - 24, 14.5
    c.setStrokeColor(bm.TEXT)
    c.setLineWidth(1.2)
    c.line(M, ft + 9, M, ft + 9 - len(lines) * fl - 2)
    c.setFont("Serif-Italic", 10)
    ty = ft
    for ln in lines:
        c.drawString(M + 14, ty, ln)
        ty -= fl
    lock_y = 82
    c.setFont("Mono-Bold", 9.5)
    c.setFillColor(bm.TEXT)
    c.drawString(M, lock_y, "DATABURN")
    c.setFont("Mono", 7)
    c.setFillColor(bm.FAINT)
    c.drawString(M, lock_y - 11, "AUDIO RESEARCH")
    c.drawString(M, lock_y - 21, "DATABURN.ORG")
    mw = 46
    mx, my = PW - M - mw, lock_y - 26
    c.setStrokeColor(bm.TEXT)
    c.setLineWidth(1.1)
    c.rect(mx, my, mw, mw, stroke=1, fill=0)
    # mark: a mast, and the sky wave coming back down to it off the ionosphere, the far arc in accent
    c.setLineWidth(1.0)
    c.line(mx + 12, my + 8, mx + 12, my + 24)
    c.line(mx + 8, my + 8, mx + 16, my + 8)
    c.setStrokeColor(bm.ACCENT)
    p = c.beginPath()
    p.moveTo(mx + 12, my + 24)
    p.curveTo(mx + 20, my + 44, mx + 30, my + 44, mx + 38, my + 12)
    c.drawPath(p, stroke=1, fill=0)
    c.setFillColor(bm.TEXT)
    c.circle(mx + 38, my + 11, 2.0, stroke=0, fill=1)
    c.setFont("Mono", 6.6)
    c.setFillColor(bm.FAINT)
    c.drawRightString(PW - M, my - 9, "KIWISDR NETWORK")
    bm.draw_footer(c, pno, ptotal, FOOTER)


def page_what(c, pno, ptotal):
    bm.blank_page(c)
    tb = bm.draw_tab(c, ["WHAT IT IS"])
    y = tb - 30
    y = bm.para(c, M, y, (
        "Skywave is an instrument for the Akai MPC that plays a shortwave radio somewhere else in the world. A KiwiSDR "
        "is a software-defined radio (a receiver whose tuning and demodulation happen in software) that covers 10 kHz to "
        "30 MHz and that its owner shares on the internet. Skywave calls one of the public ones, tunes it, cleans what "
        f"it sends and plays it on its track. GRAB writes the last seconds you heard as a WAV on the drive. The list "
        f"shipped with this build holds {len(RX)} receivers (list of {LIST_DATE}); the plugin fetches a fresh one at "
        "most once a day."), CW, leading=13.0)
    y = fig_head(c, y, "THE CHAIN")
    by, bh, gap = y - 62, 40, 10
    bw = (CW - 5 * gap) / 6
    x = M
    blocks = []
    for name, filled in [("KIWISDR\nreceiver", False), ("NETWORK\naudio, 12 kHz", False), ("CLEAN\nblank notch\ndenoise", False),
                         ("BUFFER\n%.2f s\nresample" % BUFFER_S, False), ("GRAB\nlast seconds", True), ("LEVEL\nto the track", False)]:
        blocks.append(bm.make_block(c, name, x, by, bw, bh, filled))
        x += bw + gap
    for a, b in zip(blocks, blocks[1:]):
        bm.route(c, [bm.block_pt(a, "right"), bm.block_pt(b, "left")])
    y = caption(c, M, by - 20, CW, "the signal path. The receiver demodulates; Skywave cleans, buffers against the "
                f"network's jitter ({BUFFER_S:.2f} s), resamples to the MPC's rate, keeps the last seconds for GRAB "
                "(before LEVEL, so a quiet track still grabs at full level) and plays it.")
    y = fig_head(c, y, "WHAT IT NEEDS")
    y = bullets(c, y, [
        "An MPC running MPC OS 3.10.1 or later, connected to the internet.",
        "External plugins enabled: Preferences > Enhancements (Beta) > Enable Enhancements and External Plugin Support.",
        "The folder databurn - VST - Skywave copied into a folder named VST2 Plugins at the top of an MPC drive, then the "
        "MPC restarted. Skywave is in the plugin browser under External Plugins. Grabs go to Skywave Grabs at the top of "
        "the same drive.",
    ])
    y = bm.note_box(c, M, y - 8, CW, "START HERE", ms.HELP[0][4], accent=bm.TEXT)
    bm.draw_footer(c, pno, ptotal, FOOTER)


def page_radio(c, pno, ptotal):
    page_parts(c, pno, ptotal, 0, ["RADIO", "F1"], None)


def page_tuning(c, pno, ptotal):
    bm.blank_page(c)
    tb = bm.draw_tab(c, ["TUNING"])
    y = tb - 30
    y = fig_head(c, y, "A TAP SELECTS, THE WHEEL TURNS", gap=4)
    y = bm.para(c, M, y, (
        "Every knob on the panel is a picture of a knob. A tap selects it: its Q-Link bank and the data wheel now act on "
        "it. The data wheel, the knob's Q-Link or the MPC's double-tap editor turn it. A tap never moves a knob, so "
        "pressing one to select it before reaching for the wheel is safe. Choices (STEP, MODE, CATEGORY and the rest) "
        "step one choice per click of the wheel."), CW, leading=12.5)
    y = fig_head(c, y, "TUNE AND STEP")
    y = bm.para(c, M, y, (
        f"TUNE is the big knob: tap it, then turn the data wheel (or its Q-Link), one STEP per click. The knob turns "
        f"clockwise as the frequency rises and makes a full turn every 32 steps. The frequency reads to 10 Hz. Off "
        f"the STEP's grid, the first click lands on it, as a radio's channel steps do: with 5 kHz from 9652.30 kHz the "
        f"first click up goes to {step_tune(9652.3, 5000, 1):.0f}, the next to {step_tune(step_tune(9652.3, 5000, 1), 5000, 1):.0f}; "
        f"the first click down to {step_tune(9652.3, 5000, -1):.0f}."), CW, leading=12.5)
    use = {"10 Hz": "zero in on a voice in USB or LSB, or a CW note", "50 Hz": "fine tuning of a voice",
           "100 Hz": "a data signal onto its tones", "500 Hz": "across a crowded ham band",
           "1 kHz": "the default: anywhere, in whole kHz",
           "5 kHz": "the shortwave broadcast channels: 9650, 9655, 9660", "9 kHz": "medium wave in Europe, Africa and Asia",
           "10 kHz": "medium wave in the Americas", "100 kHz": "along a band", "1 MHz": "across the dial"}
    rows = [["STEP", "FOR"]] + [[s, use[s]] for s in STEPS]
    y = bm.table(c, M, y - 6, [70, CW - 70], rows, size=8, pad=2.6)
    y = fig_head(c, y, "THE RANGE")
    y = bm.para(c, M, y, (
        "The dial runs from 0 to 30 MHz. Most receivers hear 10 kHz to 30 MHz (a few to 32 MHz); when the dial is "
        "outside what the receiver hears, the status says so (for one that stops at 20 MHz: it hears only 0.0-20.0 MHz). FM broadcast, at 88 to "
        f"108 MHz, is out of reach of every KiwiSDR. In CW the dial reads the signal's own frequency and you hear it "
        f"as a {CW_PITCH * 1000:.0f} Hz tone. MHz, kHz and Fine hold the frequency underneath: they are parameters, off "
        "the panel, for the host's automation."), CW, leading=12.5)
    y = bm.note_box(c, M, y - 6, CW, "STATION, LABEL, SEEK, A BAR", (
        "Four faster ways onto a signal: STATION steps through the CATEGORY's presets; LABEL (SCOPE) through the "
        "receiver's own station list; SEEK to the next signal up or down; and a tap on a scope bar tunes onto the "
        "strongest signal in it, to 10 Hz. Then fine-tune with TUNE until PEAK reads +0 Hz."), accent=bm.TEXT)
    bm.draw_footer(c, pno, ptotal, FOOTER)


def page_scope(c, pno, ptotal):
    page_parts(c, pno, ptotal, 1, ["SCOPE", "F2"], None)


def page_reading(c, pno, ptotal):
    bm.blank_page(c)
    tb = bm.draw_tab(c, ["READING THE SCOPE"])
    hist = WF_ROWS * WF_PUSH_MS / 1000.0
    y = tb - 30
    y = bm.para(c, M, y, (
        f"The {BINS} bars are the dial's neighbourhood over SPAN, from the receiver's own waterfall (its spectrum, "
        "sent as rows of levels), the dial in the middle. Each bar is drawn in dB over that row's noise floor (its "
        "median), green, then amber, then near white as it rises. Under them, a waterfall of its own: "
        f"{WF_ROWS} rows, a new one every {WF_PUSH_MS / 1000:.1f} s, newest on top, about {hist:.0f} seconds; it "
        "slides with the dial and starts again with a new SPAN. The amber line is the dial; the two white lines are "
        "the passband, what you hear."), CW, leading=12.5)
    y = fig_head(c, y, "PEAK, AND TUNING TO +0 HZ")
    y = bm.para(c, M, y, (
        "PEAK names the strongest signal within an eighth of the span either side of the dial, how far it stands over "
        "the noise, and how far off the dial it is: 9650.00 kHz +44 dB (+0 Hz). Turn TUNE until it reads +0 Hz. It is "
        "where the receiver hears it: a receiver without a GPS reference runs a few parts per million off, so WWV can "
        "sit at 15000.17 kHz, and the audio agrees with the scope."), CW, leading=12.5)
    y = fig_head(c, y, "A TAPPED BAR, SEEK, SCALE")
    y = bullets(c, y, [
        f"Tap a bar: the radio tunes onto the strongest signal in it, to 10 Hz (in USB or LSB a kilohertz beside the "
        f"voice, where its carrier is). Nothing {BAR_DB} dB over the floor in it: the bar's middle.",
        f"SEEK DOWN and SEEK UP, by Signals: the next peak {SEEK_DB} dB over the floor, or half a span on if there is "
        "none in view. By Labels: the next of the receiver's own labels.",
        "SCALE: Auto puts the strongest signal of the last moments at the top, bars and waterfall alike. 20, 40 and "
        "60 dB fix the range over the noise, to see how strong things really are.",
    ], leading=12.5)
    y = fig_head(c, y, "SPAN")
    rows = [["SPAN"] + SPANS[:5], [""] + SPANS[5:]]
    widths = [60] + [(CW - 60) / 5] * 5
    y = bm.table(c, M, y - 4, widths, rows, header=False, size=8, pad=2.6)
    y = bm.para(c, M, y - 12, (
        f"The default is {SPAN_DEFAULT}. A receiver that zooms only so far shows its closest (some stop at 15 kHz); "
        "the edge labels under the scope show the real edges."), CW, leading=12.5)
    y = bm.note_box(c, M, y - 6, CW, "THE WATERFALL IS BORROWED", (
        "Most receivers have three waterfalls for eight listeners. Skywave opens one only while the scope is in use: "
        "turning SPAN, SCALE, SEEK or LABEL, tapping a bar, a Live sweep, or fetching the receiver's labels for LIST "
        "or DIG; until then PEAK reads 'turn SPAN to start the scope', and the first rows take a few seconds. If the "
        "receiver's waterfalls are all in use, PEAK reads 'no waterfall slot: try later'; it asks again after a "
        "minute, or after 15 s if you turn SPAN. SEEK "
        "just after a Live sweep steps through the sweep and leaves the waterfall closed. "
        f"After {SCOPE_IDLE} minutes without any of these it stops sending, parked as a browser's waterfall turned "
        "off (closing it would end the audio too), and it closes with the audio when Listen goes off or the receiver "
        "changes."), accent=bm.TEXT)
    bm.draw_footer(c, pno, ptotal, FOOTER)


def page_clean(c, pno, ptotal):
    bm.blank_page(c)
    tb = bm.draw_tab(c, ["CLEAN", "F3"])
    name, key, _, items, para = ms.HELP[2]
    y = page_shot(c, 2, tb - 18)
    y = caption(c, M, y - 14, CW, "the CLEAN page (F3), its parts numbered as its own HELP page numbers them.")
    rows = [["", "PART", "WHAT IT SHOWS OR DOES"]] + [[str(n), t, d] for n, (_, t, d) in enumerate(items, 1)]
    y = bm.table(c, M, y - 6, [20, 82, CW - 102], rows, size=8, leading=9.6, pad=2.0)
    y = fig_head(c, y, "PASSBANDS, HZ ROUND THE DIAL")
    rows = [["MODE", "NARROW", "NORMAL", "WIDE"]]
    for m, name in enumerate(MODES):
        rows.append([name] + [f"{lo:+d} to {hi:+d}" for lo, hi in PASS[m]])
    y = bm.table(c, M, y - 4, [70, (CW - 70) / 3, (CW - 70) / 3, (CW - 70) / 3], rows, size=8, leading=9.6, pad=2.0)
    y = bm.para(c, M, y - 12, (
        f"CW listens {CW_PITCH * 1000:.0f} Hz under the signal, so its passband sits round the tone you hear; NBFM's "
        f"width is its deviation's. AGC decays in {AGC_MS[0] / 1000:.1f} s (Slow), {AGC_MS[1] / 1000:.1f} s (Medium) "
        f"or {AGC_MS[2] / 1000:.1f} s (Fast). The BLANKER gates a crash for {BLANK_MS} ms."), CW, size=9.0, leading=11.8)
    bm.draw_footer(c, pno, ptotal, FOOTER)


def page_list(c, pno, ptotal):
    page_parts(c, pno, ptotal, 3, ["LIST", "F4"], None)


def page_finding(c, pno, ptotal):
    bm.blank_page(c)
    tb = bm.draw_tab(c, ["FINDING SOMETHING"])
    pc = per_cat()
    y = tb - 30
    y = bm.para(c, M, y, (
        f"CATEGORY is the filter: STATION steps through its presets only, LIST shows its stations and its part of the "
        f"Live sweep, and DIG stays in it. {len(ST)} presets in {len(CATS)} categories, named for how they sound "
        "rather than who sends them; Anything lets everything through. Broadcasters move with the seasons; the metre "
        "bands always hold someone."), CW, leading=12.5)
    rows = [["CATEGORY", "PRESETS", "AMONG THEM"]]
    for _, n in CATS:
        names = [s[0] for s in pc[n]]
        rows.append([n, str(len(names)), ", ".join(names[:4]) + (", ..." if len(names) > 4 else "")])
    rows.append(["Anything", str(len(ST)), "no filter: every preset, every band"])
    y = bm.table(c, M, y - 6, [96, 46, CW - 142], rows, size=8, pad=2.4)
    y = fig_head(c, y, "LIVE, DIG, BACK")
    y = bullets(c, y, [
        f"LIVE (LIST): one sweep of the receiver's whole band, a few seconds, then its strongest {LIVE_MAX} signals, "
        "loudest first, those of the CATEGORY shown, each with how far it stands over the noise (9650.0 AM +44 31m "
        "band). Tap a row to go there; PAGE UP at the top sweeps again.",
        f"DIG goes somewhere in the CATEGORY at random. On air it sweeps first when there is no sweep of this receiver "
        f"from the last {LIVE_FRESH} minutes, picks from what it found (the strong more often), listens to each place "
        f"for a moment and digs again on noise, up to {DIG_TRIES} places. Off air it picks a label or a preset.",
        "BACK returns to the last place you stayed three seconds or more, and again to the one before.",
        f"The SIGNAL lamp lights when the dial stands {LAMP_DB:.0f} dB or more over that band's noise.",
    ], leading=12.3)
    y = fig_head(c, y, "RECEIVERS BY REGION")
    reg = regions()
    rows = [["REGION"] + [n for n, _ in reg], ["RECEIVERS"] + [str(k) for _, k in reg]]
    y = bm.table(c, M, y - 4, [70] + [(CW - 70) / len(reg)] * len(reg), rows, size=8, pad=2.4)
    y = bm.para(c, M, y - 12, (
        "Within a region the receivers run west to east, so turning RECEIVER travels across the map. A few sit outside "
        "every region and are listed only under All. Each shows how many listeners it had at the list's last update: "
        "8/8 is full."), CW, leading=12.3)
    bm.draw_footer(c, pno, ptotal, FOOTER)


def page_grab(c, pno, ptotal):
    bm.blank_page(c)
    tb = bm.draw_tab(c, ["GRAB"])
    ring_s = (1 << RING_BITS) / 44100.0
    y = tb - 30
    y = bm.para(c, M, y, (
        "GRAB saves what you just heard. Every tap writes the last GRAB LEN of it as a WAV, from before LEVEL, so a "
        "quiet track still grabs at full level. GRAB is on every page."), CW, leading=12.5)
    secs = [l for l in LENS if l.endswith(" s")]
    bars = [l for l in LENS if not l.endswith(" s")]
    rows = [["GRAB LEN", "WHAT IS SAVED"], [", ".join(secs), "the last that many seconds"],
            [", ".join(bars), "the last that many bars at the project's tempo, sample-exact, so it loops on the grid"]]
    y = bm.table(c, M, y - 6, [120, CW - 120], rows, size=8, pad=2.6)
    y = fig_head(c, y, "THE FILE")
    y = bullets(c, y, [
        f"At most {GRAB_MAX_S:.0f} s: a bar length longer than that is halved until it fits, and the status says so "
        "(2 bars (4 too long)).",
        f"Normalised to peak at {20 * math.log10(GAIN_REF / 32767):.0f} dBFS, with at most {20 * math.log10(GAIN_MAX):.0f} dB "
        "of gain, so static stays static. Silence (under -60 dBFS) is not saved.",
        "Mono, 16-bit, at the MPC's sample rate.",
        "In <drive>/Skywave Grabs/, the drive the plugin is installed on, named for the station or the receiver's label, "
        "the receiver, the mode, the bars and tempo of a bar grab, and the time:",
    ], leading=12.5)
    c.setFont("Mono", 8)
    c.setFillColor(bm.TEXT)
    c.drawString(M + 12, y, "WWV 15 Manhattan, KS, USA am 2bar 90bpm 261115-171636.wav")
    y -= 16
    y = bullets(c, y, ["Two taps in the same second keep both: the second gets 2 after its name."], leading=12.5)
    y = bm.note_box(c, M, y - 8, CW, "WITHOUT THE FILE", (
        "To put the radio straight on a pad: open the Sampler, set its input to Resample, arm it and record while "
        "Skywave plays. GRAB is for the moment you already heard."), accent=bm.TEXT)
    y = bm.para(c, M, y - 18, (
        f"The grab memory holds {int(ring_s)} s at 44.1 kHz but {int((1 << RING_BITS) / 96000.0)} s at 96 kHz, where "
        "a long grab comes out shorter; the status shows the length saved. What you sample off the air may be someone's "
        "broadcast, and what you do with it is up to you."), CW, size=9.0, leading=12.0)
    bm.draw_footer(c, pno, ptotal, FOOTER)


def page_guest(c, pno, ptotal):
    bm.blank_page(c)
    tb = bm.draw_tab(c, ["BEING A GOOD GUEST"])
    waits = [5 << k for k in range(MAX_TRIES - 1)]
    y = tb - 30
    y = bm.para(c, M, y, (
        "Every KiwiSDR is a volunteer's hardware with a few listening slots. Skywave behaves like a considerate "
        "listener:"), CW, leading=12.5)
    y = bullets(c, y - 2, [
        "It takes one slot and says what it is: skywave-mpc, with a link to databurn.org/mpc. It leaves out of the list "
        "the receivers whose owners allow only web browsers.",
        f"A receiver that ends the session after {SESSION_S} s or more on air, or says a time limit was reached, is not "
        "called again until you switch Listen off and on. Busy, password-only and down receivers are treated the same.",
        f"A call that fails waits {', '.join(str(w) for w in waits[:-1])}, then {waits[-1]} s before the next, and gives "
        f"up after {MAX_TRIES}.",
        "Turning RECEIVER does not call every receiver it passes: it waits for the knob to rest a second.",
        "A Live sweep is one waterfall for a few seconds, of the receiver you are on, made when you ask (Live, DIG, "
        "PAGE UP), at most once a minute. Skywave never sweeps receivers you are not listening to.",
        f"Listen switches itself off after {IDLE_MIN} minutes in which nothing was touched, and a project always opens "
        "with Listen off: a slot is only taken by a hand.",
        f"The scope's waterfall is parked after {SCOPE_IDLE} minutes unused.",
    ], leading=12.5)
    y = bm.note_box(c, M, y - 8, CW, "PLEASE", (
        "Switch Listen off when you are done, and do not leave a project playing a receiver overnight."), accent=bm.TEXT)
    y = fig_head(c, y - 6, "HOW IT WORKS")
    y = bm.para(c, M, y, (
        "A worker thread owns the network: it reads the receiver list, opens the receiver's websocket, answers its "
        "messages, cleans the audio (about 12 kHz, mono) and pushes it into a ring. The audio thread resamples it to "
        f"the MPC's rate from a {BUFFER_S:.2f} s jitter buffer whose speed trims itself to the receiver's clock. The "
        "list comes from rx.linkfanel.net, generated from kiwisdr.com/public. The plugin ships a trimmed copy and, the "
        "first time Listen goes on in a day, fetches a fresh one: from databurn.org/mpc/receivers.js first (the same "
        "list, trimmed every six hours, without the owners' contact details), from rx.linkfanel.net when that is "
        "missing or three days old, and the built-in copy when both fail."), CW, leading=12.5)
    bm.draw_footer(c, pno, ptotal, FOOTER)


CONTROL_TEXT = {
    "Level": "output level",
    "Listen": "POWER: on takes one listening slot on the receiver, off gives it back",
    "Region": "which part of the world RECEIVER browses",
    "Receiver": "the receiver, west to east within the region; called once the knob rests a second",
    "Station": "the CATEGORY's presets, round and round; Dial when the radio is elsewhere",
    "MHz": "the frequency's whole MHz (off the panel)",
    "kHz": "the frequency's whole kHz (off the panel)",
    "Fine": "the rest of the frequency, to 10 Hz (off the panel)",
    "Mode": "AM broadcast, LSB and USB voice, CW Morse, NBFM",
    "Grab": "a tap saves the last GRAB LEN as a WAV",
    "Grab Len": "seconds, or bars at the project's tempo",
    "Category": "the filter: what STATION, LIST and DIG offer",
    "Seek Down": "the next signal (or label) below",
    "Seek Up": "the next signal (or label) above",
    "Seek By": "SEEK by the scope's signals, or the receiver's labels",
    "Span": "how wide the scope looks round the dial",
    "Label": "the receiver's own station list; turning it tunes to each",
    "Bandwidth": "the receiver's passband for the mode",
    "Shift": "moves the passband up or down in pitch",
    "AGC": "the receiver's automatic gain; Off hands it to GAIN",
    "Gain": "the receiver's gain with AGC off",
    "Blanker": "gates clicks and crashes far over the average",
    "Notch": "Auto takes out steady whistles",
    "Denoise": "keeps what is predictable, lets the hiss go",
    "List": "what LIST's rows show",
    "Scroll": "the top row of the list; the header's text says where you are",
    "Page Up": "eight rows up; at the top of Live, a new sweep",
    "Page Down": "eight rows down",
    "Back": "the last place you stayed three seconds or more",
    "Dig": "somewhere in the CATEGORY, at random",
    "Step": "how far TUNE moves a click",
    "Tune": "the tuning knob: a STEP a click of the data wheel",
    "Scale": "how the scope's bars and waterfall are scaled",
}


def param_range(p):
    """What a parameter can be: its choices, or its range from its declaration in Skywave.cpp."""
    ch = p.get("choices") or []
    name = p["name"]
    if name == "Station":
        return f"Dial and {len(ST)} presets"
    if name == "Category":
        return f"{len(ch) - 1} categories, Anything"
    if name in ("Tune",):
        return "a jog: one STEP a click"
    if ch:
        return ", ".join(ch) if len(", ".join(ch)) <= 44 else ", ".join(ch[:3]) + f", ... ({len(ch)})"
    m = re.search(r'(?:float|int)Param\("[^"]+", "%s", ([-0-9.f]+), ([-0-9.f]+), [-0-9.f]+(?:, "([^"]*)")?' % re.escape(name), SRC)
    assert m, name
    lo, hi, unit = m.group(1).rstrip("f"), m.group(2).rstrip("f"), m.group(3) or ""
    fmt = lambda v: str(int(float(v))) if float(v) == int(float(v)) else v
    return f"{fmt(lo)} to {fmt(hi)} {unit}".strip()


def controls():
    return [p for p in PARAMS if p["automatable"]]


def page_controls(c, pno, ptotal, part):
    bm.blank_page(c)
    tb = bm.draw_tab(c, ["CONTROLS"] if part == 0 else ["CONTROLS", "CONTINUED"])
    cs = controls()
    half = (len(cs) + 1) // 2
    y = tb - 30
    if part == 0:
        y = bm.para(c, M, y, (
            f"Skywave has {len(cs)} parameters a hand or a host can set; this table is read from the plugin when the "
            "manual is built. The rest are read-outs the plugin writes for the panel to draw (next page)."),
            CW, leading=12.5)
    sel = cs[:half] if part == 0 else cs[half:]
    rows = [["CONTROL", "RANGE", "WHAT IT SETS"]] + [[p["name"].upper(), param_range(p), CONTROL_TEXT[p["name"]]] for p in sel]
    y = bm.table(c, M, y - 6, [80, 150, CW - 230], rows, size=8, pad=2.6)
    if part == 1:
        ro = [p for p in PARAMS if not p["automatable"]]
        wf = sum(1 for p in ro if p["name"].startswith("WF "))
        bins = sum(1 for p in ro if p["name"].startswith("Bin "))
        rws = sum(1 for p in ro if re.fullmatch(r"Row \d+", p["name"]))
        other = len(ro) - wf - bins - rws
        y = fig_head(c, y, "READ-OUTS")
        y = bm.para(c, M, y, (
            f"{len(ro)} more parameters carry what the panel draws, written by the plugin and never automated: "
            f"{bins} scope bars, {wf} waterfall cells ({WF_ROWS} rows of {BINS}), {rws} list rows, and {other} others "
            "(the status, the dial, the meter, the map's cross, the lamps, the passband's edges, PEAK, the knob's "
            "turn, the data wheel's jog). They are why the plugin reports "
            f"{len(PARAMS)} parameters."), CW, leading=12.5)
    bm.draw_footer(c, pno, ptotal, FOOTER)


def page_controls_1(c, pno, ptotal):
    page_controls(c, pno, ptotal, 0)


def page_controls_2(c, pno, ptotal):
    page_controls(c, pno, ptotal, 1)


SOURCES = [
    "[1] The KiwiSDR project and its owners' public receivers, kiwisdr.com (the receivers, the websocket protocol).",
    "[2] The public receiver list, rx.linkfanel.net/kiwisdr_com.js, generated from kiwisdr.com/public.",
    "[3] The MPC Plugin Builder SDK, github.com/inmusic-software/mpc-plugin-builder-sdk, MIT, (c) 2026 inMusic Brands, Inc.",
    "[4] Natural Earth 1:50m land, naturalearthdata.com, public domain (the panel's map).",
    "[5] Barlow, SIL Open Font License (the panel's lettering).",
    "[6] NIST, WWV and WWVH (the time stations in Ticks & Time); the NCDXF / IARU International Beacon Project (Morse Beacons).",
]


def page_limits(c, pno, ptotal):
    bm.blank_page(c)
    tb = bm.draw_tab(c, ["LIMITS AND SOURCES"])
    y = tb - 30
    y = fig_head(c, y, "WHAT IT DOES NOT DO", gap=4)
    y = bullets(c, y, [
        "Play without the internet: offline it keeps its built-in list, but every sound comes from a receiver.",
        "Hold a receiver: owners set time limits, slots fill, receivers go down and come back. The status says why.",
        "Reach above 30 MHz (32 on a few receivers): no FM broadcast, no VHF.",
        "Send MIDI or play notes: Skywave is a radio on a track, not a pitched instrument. Resample or GRAB it, "
        "then play the sample.",
    ], leading=12.5)
    y = fig_head(c, y, "KNOWN ISSUES, 11.16.2026")
    y = bullets(c, y, [
        "Not yet confirmed on the device: that a tap on a knob hands the data wheel to it, and that TUNE's Q-Link moves "
        "one step per detent.",
        f"The waterfall is {WF_ROWS * BINS} read-out cells updated every {WF_PUSH_MS / 1000:.1f} s. If the MPC lags, or "
        "calls stall while SCOPE is in use, the waterfall is the first suspect: report it.",
        "SPAN names the span asked for when a receiver caps it; the edge labels show the real edges.",
    ], leading=12.5)
    y = fig_head(c, y, "SOURCES AND CREDITS")
    for s in SOURCES:
        y = bm.para(c, M, y, s, CW, size=8.6, leading=11.5) - 4
    y = bm.para(c, M, y - 6, "Skywave is MIT licensed; LICENSE carries the SDK's notice too.", CW, size=8.6, leading=11.5)
    bm.draw_footer(c, pno, ptotal, FOOTER)


PAGES = [page_cover, page_what, page_radio, page_tuning, page_scope, page_reading, page_clean, page_list, page_finding,
         page_grab, page_guest, page_controls_1, page_controls_2, page_limits]


# ================================================== claim verification =====
def verify_claims():
    # identity and install
    assert PJ["name"] == "Skywave" and PJ["vendor"] == "databurn", (PJ["name"], PJ["vendor"])
    assert "databurn - VST - Skywave" in README and "MPC OS 3.10.1" in README
    assert "Enable Enhancements and External Plugin Support" in README and "External Plugins" in README
    assert '"/Skywave Grabs"' in SRC and "Skywave Grabs" in README
    # the pages: tab names and keys, in the order the manual shows them
    tabs = [(t["tabName"], t["fnKeyIndex"]) for t in TUI["tabs"]]
    assert tabs[:4] == [("RADIO", 0), ("SCOPE", 1), ("CLEAN", 2), ("LIST", 3)], tabs[:4]
    assert [h[0] for h in ms.HELP] == ["RADIO", "SCOPE", "CLEAN", "LIST"] and [h[1] for h in ms.HELP] == ["F1", "F2", "F3", "F4"]
    # receivers and regions
    assert len(RX) > 100 and all(-90 <= r["lat"] <= 90 and -180 <= r["lon"] <= 180 for r in RX)
    assert len(REGIONS) == 7 and REGIONS[0] == "All"
    assert sum(k for _, k in regions()) + sum(1 for r in RX if r["region"] == 0) == len(RX)
    # stations and categories
    assert len(ST) == 140 and len(CATS) == 10, (len(ST), len(CATS))
    assert BY["Category"]["choices"] == [n for _, n in CATS] + ["Anything"]
    assert 'choiceParam("station", "Station", stations' in SRC and '{"Dial", 0.0, kAM, -1}' in SRC   # Dial, then the presets
    assert all(n for n in per_cat().values()), "every category has presets"
    # tuning
    assert len(STEPS) == len(STEP_HZ) == 10 and STEPS[0] == "10 Hz" and STEPS[-1] == "1 MHz"
    names = {10: "10 Hz", 50: "50 Hz", 100: "100 Hz", 500: "500 Hz", 1000: "1 kHz", 5000: "5 kHz", 9000: "9 kHz",
             10000: "10 kHz", 100000: "100 kHz", 1000000: "1 MHz"}
    assert [names[h] for h in STEP_HZ] == STEPS
    assert BY["Step"]["display"] == "1 kHz", "the default step"
    assert (step_tune(9652.3, 5000, 1), step_tune(9655.0, 5000, 1), step_tune(9652.3, 5000, -1)) == (9655.0, 9660.0, 9650.0)
    assert "tuneTurns_ += (c - tuneKnobKHz_) / (32.0 * step);" in SRC, "a turn every 32 steps"
    assert "turn = 2 * math.pi * v" in MAKE_SKIN, "the knob turns clockwise as the frequency rises"
    assert "29999.99" in SRC and "it hears only" in SRC and "fine\", \"Fine\", -1.0f, 1.0f" in SRC
    assert CW_PITCH == 0.5
    assert "A tap never moves it" in README
    # the scope
    assert BINS == 64 and WF_ROWS == 12 and WF_PUSH_MS == 600 and round(WF_ROWS * WF_PUSH_MS / 1000) == 7
    assert "win = rowSpanKHz_ / 8.0" in SRC, "PEAK looks an eighth of the span either side"
    assert SEEK_DB == 15 and BAR_DB == 8 and LAMP_DB == 10
    assert len(SPANS) == 10 and SPANS[0] == "2 kHz" and SPANS[-1] == "938 kHz" and SPAN_DEFAULT == "59 kHz"
    assert SCOPE_IDLE == 5 and "some stop at 11, 15 kHz" in SRC
    assert "kWfAskAgain = std::chrono::seconds(15)" in SRC and SRC.count("wfRetryAt_ = now + seconds(60);") == 2
    assert 'kNoSlot = "no waterfall slot: try later"' in SRC
    assert "Most receivers have three waterfalls for eight" in SRC and "15000.17 kHz" in README
    # clean
    assert MODES == ["AM", "LSB", "USB", "CW", "NBFM"] and PASS[0][1] == (-4900, 4900)
    assert AGC_MS == [2500, 1000, 300] and BLANK_MS == 2
    assert BY["Bandwidth"]["choices"] == ["Narrow", "Normal", "Wide"]
    # list, live, dig
    assert LIVE_MAX == 200 and LIVE_FRESH == 10 and DIG_TRIES == 5 and ROWS == 8
    assert "surveyedWithin(now, seconds(60))" in SRC, "a sweep at most once a minute"
    assert "now - seenSince_ < seconds(3)" in SRC, "BACK remembers places you stayed 3 s"
    # grab
    assert LENS == ["5 s", "10 s", "20 s", "30 s", "1 bar", "2 bars", "4 bars", "8 bars"]
    assert GRAB_MAX_S == 40 and round(20 * math.log10(GAIN_MAX)) == 24 and round(20 * math.log10(GAIN_REF / 32767)) == -1
    assert "peak < 33) { setStatus(\"nothing to grab: silence\"); return; }   // -60 dBFS" in SRC
    assert "u16(1); u16(1); u32(rate)" in SRC and "u16(2); u16(16);" in SRC, "mono, 16-bit, at the host's rate"
    assert "WWV 15 Manhattan, KS, USA am 2bar 90bpm 261115-171636.wav" in README
    assert 'base + " " + std::to_string(k) + ".wav"' in SRC
    assert int((1 << RING_BITS) / 44100.0) == 47 and "47 s at 44.1 kHz" in SRC
    # being a guest
    assert "SET ident_user=skywave-mpc" in SRC and "databurn.org/mpc" in SRC
    assert SESSION_S == 30 and MAX_TRIES == 5 and [5 << k for k in range(MAX_TRIES - 1)] == [5, 10, 20, 40]
    assert "now - keySince >= seconds(1)" in SRC and IDLE_MIN == 30
    assert BUFFER_S == 0.35 and "3 * 24 * 3600" in SRC and "every six hours" in README
    # controls
    cs = controls()
    assert [p["name"] for p in cs] == list(CONTROL_TEXT), set(p["name"] for p in cs) ^ set(CONTROL_TEXT)
    assert len(cs) == 33 and len(PARAMS) == 892, (len(cs), len(PARAMS))
    for p in cs:
        param_range(p)
    print(f"claims: ok ({len(RX)} receivers, {len(ST)} presets, {len(cs)} controls of {len(PARAMS)} parameters)")


def build(theme="rally", out=None):
    verify_claims()
    bm.set_theme(theme)
    bm.FIG["n"] = 0
    out = out or os.path.join(_HERE, f"skywave_manual_{theme}.pdf")
    c = canvas.Canvas(out, pagesize=A4)
    c.setTitle("Skywave, operator's manual DB-WP-009")
    c.setAuthor("databurn")
    for i, fn in enumerate(PAGES, 1):
        fn(c, i, len(PAGES))
        c.showPage()
    c.save()
    print(f"built {out} ({len(PAGES)} pages, {bm.FIG['n']} figures, {theme})")


if __name__ == "__main__":
    names = sys.argv[1:] or ["rally"]
    if names == ["all"]:
        names = list(bm.THEMES)
    for n in names:
        build(n)
