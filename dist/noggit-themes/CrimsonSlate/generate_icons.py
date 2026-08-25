#!/usr/bin/env python3
# This file is part of Noggit3, licensed under GNU General Public License (version 3).
"""Regenerate the Crimson Slate theme PNG widget assets (Ironforge palette).

Usage:  python generate_icons.py images
        (run from dist/noggit-themes/CrimsonSlate/ -- dist/themes/ is a
         third-party submodule and nothing written there is ours)

The colour constants below are the same tokens documented at the top of
theme.qss. Change them here and re-run to reskin every check box, radio,
chevron and window glyph in one pass; nothing is hand-drawn.

Pure stdlib: an SDF rasteriser at 4x supersample, box-downsampled, written
out as 8-bit RGBA PNG via zlib. No Pillow, no ImageMagick, no network.
"""
import math
import os
import struct
import sys
import zlib

SS = 4  # supersample factor

# ---------------------------------------------------------------- palette ---
# These are the tokens documented at the top of theme.qss, plus the three
# values derived from them (marked). Nothing here is a fourth palette: change a
# token in theme.qss and change the same token here, or the PNG indicators will
# drift away from the CSS-drawn controls beside them.
SUNKEN     = (0x10, 0x0E, 0x0B)   # bg.void
PANEL      = (0x29, 0x26, 0x21)   # bg.panel
RAISED     = (0x3C, 0x37, 0x32)   # bg.raised
HOVERBG    = (0x4A, 0x46, 0x40)   # bg.overlay
HAIRLINE   = (0x40, 0x3B, 0x35)   # stroke.soft
CONTROL    = (0x56, 0x50, 0x49)   # stroke
CONTROL_HI = (0x74, 0x6D, 0x64)   # stroke.hi, hover outline
ACCENT     = (0xDF, 0xA5, 0x2E)   # accent
ACCENT_HOV = (0xF0, 0xBA, 0x4A)   # accent.hi, hover on a filled accent
ACCENT_DIM = (0xB8, 0x80, 0x1F)   # accent.press
INK        = (0x10, 0x0E, 0x0B)   # bg.void, used as ink ON an accent fill
TEXT_HI    = (0xF3, 0xF0, 0xE9)   # text.hi
TEXT_BODY  = (0xE4, 0xDF, 0xD7)   # text
TEXT_DIM   = (0xBF, 0xB7, 0xAA)   # text.dim
TEXT_DIS   = (0x7F, 0x78, 0x6A)   # text.off

# CHANGED with the Ironforge palette: an input is a WELL now, not a raised
# slab, so the unchecked indicator takes bg.void and its stroke ring reads at
# 2.42:1 against its own fill. Under the previous sheet this was bg.raised and
# the indicator was the same surface as the button next to it.
INPUT      = SUNKEN

# Ring and corner geometry for the check box and radio indicators. Both are
# drawn on a 32-unit canvas that the sheet displays at 16px, so a value here is
# HALF a screen pixel per unit: RING 4.0 is the 2px ring the component spec
# asks for, and RADIUS 8.0 is its 4px corner. The previous values (1.6 and 5.0)
# drew a 0.8px ring with a 2.5px corner.
RING       = 4.0
RADIUS     = 8.0


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(w * h)]

    def blend(self, x, y, rgb, a):
        if a <= 0.0:
            return
        if a > 1.0:
            a = 1.0
        p = self.px[y * self.w + x]
        na = a + p[3] * (1.0 - a)
        if na <= 0.0:
            return
        for i in range(3):
            p[i] = (rgb[i] * a + p[i] * p[3] * (1.0 - a)) / na
        p[3] = na

    def fill_sdf(self, sdf, rgb, alpha=1.0):
        """Fill every pixel whose sdf(x, y) < 0 (distance in pixel units)."""
        for y in range(self.h):
            for x in range(self.w):
                d = sdf(x + 0.5, y + 0.5)
                if d < 0.0:
                    self.blend(x, y, rgb, alpha)

    def downsample(self, factor):
        ow, oh = self.w // factor, self.h // factor
        out = Canvas(ow, oh)
        n = factor * factor
        for y in range(oh):
            for x in range(ow):
                r = g = b = a = 0.0
                for sy in range(factor):
                    for sx in range(factor):
                        p = self.px[(y * factor + sy) * self.w + x * factor + sx]
                        r += p[0] * p[3]
                        g += p[1] * p[3]
                        b += p[2] * p[3]
                        a += p[3]
                o = out.px[y * ow + x]
                if a > 0.0:
                    o[0], o[1], o[2] = r / a, g / a, b / a
                o[3] = a / n
        return out

    def to_png(self, path):
        raw = bytearray()
        for y in range(self.h):
            raw.append(0)  # filter type 0
            for x in range(self.w):
                p = self.px[y * self.w + x]
                raw += bytes((
                    max(0, min(255, int(round(p[0])))),
                    max(0, min(255, int(round(p[1])))),
                    max(0, min(255, int(round(p[2])))),
                    max(0, min(255, int(round(p[3] * 255.0)))),
                ))

        def chunk(tag, data):
            c = struct.pack('>I', len(data)) + tag + data
            return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

        png = b'\x89PNG\r\n\x1a\n'
        png += chunk(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 6, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
        png += chunk(b'IEND', b'')
        with open(path, 'wb') as fh:
            fh.write(png)


# ------------------------------------------------------------------- sdfs ---
def sdf_round_rect(cx, cy, hw, hh, r):
    hw = max(hw - r, 0.0)
    hh = max(hh - r, 0.0)

    def f(x, y):
        dx = abs(x - cx) - hw
        dy = abs(y - cy) - hh
        ox, oy = max(dx, 0.0), max(dy, 0.0)
        return math.hypot(ox, oy) + min(max(dx, dy), 0.0) - r
    return f


def sdf_circle(cx, cy, r):
    return lambda x, y: math.hypot(x - cx, y - cy) - r


def sdf_polyline(points, half_width, round_caps=True):
    def seg_dist(px, py, ax, ay, bx, by):
        vx, vy = bx - ax, by - ay
        wx, wy = px - ax, py - ay
        L2 = vx * vx + vy * vy
        t = 0.0 if L2 == 0.0 else max(0.0, min(1.0, (wx * vx + wy * vy) / L2))
        return math.hypot(px - (ax + t * vx), py - (ay + t * vy))

    def f(x, y):
        d = min(seg_dist(x, y, *points[i], *points[i + 1])
                for i in range(len(points) - 1))
        return d - half_width
    return f


def sdf_union(*fns):
    return lambda x, y: min(fn(x, y) for fn in fns)


# ------------------------------------------------------------- primitives ---
def new(w, h):
    return Canvas(w * SS, h * SS)


def save(c, name, outdir):
    c.downsample(SS).to_png(os.path.join(outdir, name))
    return name


def box(c, size, fill, stroke, sw=RING, inset=2.5, radius=RADIUS):
    """Rounded square centred in a size x size logical canvas."""
    s = SS
    cx = cy = size * s / 2.0
    half = (size / 2.0 - inset) * s
    outer = sdf_round_rect(cx, cy, half, half, radius * s)
    if stroke:
        c.fill_sdf(outer, stroke)
        inner = sdf_round_rect(cx, cy, half - sw * s, half - sw * s,
                               max(radius * s - sw * s, 0.5))
        if fill:
            c.fill_sdf(inner, fill)
    elif fill:
        c.fill_sdf(outer, fill)


def disc(c, size, fill, stroke, sw=RING, inset=2.5):
    s = SS
    cx = cy = size * s / 2.0
    r = (size / 2.0 - inset) * s
    if stroke:
        c.fill_sdf(sdf_circle(cx, cy, r), stroke)
        if fill:
            c.fill_sdf(sdf_circle(cx, cy, r - sw * s), fill)
    elif fill:
        c.fill_sdf(sdf_circle(cx, cy, r), fill)


def stroke_path(c, pts, colour, width):
    s = SS
    c.fill_sdf(sdf_polyline([(x * s, y * s) for x, y in pts], width * s / 2.0),
               colour)


# ------------------------------------------------------------------ icons ---
def build(outdir):
    os.makedirs(outdir, exist_ok=True)
    made = []
    SZ = 32           # checkbox / radio logical size (drawn at 2x of a 16px box)
    CHECK = [(10.5, 16.5), (14.2, 20.4), (21.8, 11.8)]

    # ---- check boxes -------------------------------------------------------
    specs = [
        ('icon_checkbox_unchecked.png',          INPUT,   CONTROL,    None),
        ('icon_checkbox_unchecked_hover.png',    INPUT,   CONTROL_HI, None),
        ('icon_checkbox_unchecked_disabled.png', PANEL,   HAIRLINE,   None),
        ('icon_checkbox_checked.png',            ACCENT,  ACCENT,     INK),
        ('icon_checkbox_checked_hover.png',      ACCENT_HOV, ACCENT_HOV, INK),
        ('icon_checkbox_checked_disabled.png',   PANEL,   HAIRLINE,   TEXT_DIS),
    ]
    for name, fill, stroke, mark in specs:
        c = new(SZ, SZ)
        box(c, SZ, fill, stroke)
        if mark:
            stroke_path(c, CHECK, mark, 3.2)
        made.append(save(c, name, outdir))

    for name, fill, stroke, mark in (
            ('icon_checkbox_indeterminate.png',          INPUT, ACCENT,   ACCENT),
            ('icon_checkbox_indeterminate_disabled.png', PANEL, HAIRLINE, TEXT_DIS)):
        c = new(SZ, SZ)
        box(c, SZ, fill, stroke)
        stroke_path(c, [(10.0, 16.0), (22.0, 16.0)], mark, 3.2)
        made.append(save(c, name, outdir))

    # ---- radio buttons -----------------------------------------------------
    radios = [
        ('icon_radiobutton_unchecked.png',          INPUT,      CONTROL,    None),
        ('icon_radiobutton_unchecked_hover.png',    INPUT,      CONTROL_HI, None),
        ('icon_radiobutton_unchecked_disabled.png', PANEL,      HAIRLINE,   None),
        ('icon_radiobutton_checked.png',            ACCENT,     ACCENT,     INK),
        ('icon_radiobutton_checked_hover.png',      ACCENT_HOV, ACCENT_HOV, INK),
        ('icon_radiobutton_checked_disabled.png',   PANEL,      HAIRLINE,   TEXT_DIS),
    ]
    for name, fill, stroke, dot in radios:
        c = new(SZ, SZ)
        disc(c, SZ, fill, stroke)
        if dot:
            c.fill_sdf(sdf_circle(SZ * SS / 2.0, SZ * SS / 2.0, 5.0 * SS), dot)
        made.append(save(c, name, outdir))

    # ---- arrows (chevrons) -------------------------------------------------
    # QIcon scales with KeepAspectRatio, so the canvas aspect must match the
    # width/height declared in the QSS: 16x10 for up/down, 10x16 for right.
    # The glyph fills the canvas, otherwise it vanishes at 8px.
    def chevron(name, direction, colour):
        if direction in ('down', 'up'):
            c = new(16, 10)
            pts = ([(2.4, 3.2), (8.0, 7.4), (13.6, 3.2)] if direction == 'down'
                   else [(2.4, 6.8), (8.0, 2.6), (13.6, 6.8)])
        else:
            c = new(10, 16)
            pts = [(3.2, 2.4), (7.4, 8.0), (3.2, 13.6)]
        stroke_path(c, pts, colour, 2.2)
        made.append(save(c, name, outdir))

    chevron('down_arrow.png', 'down', TEXT_BODY)
    chevron('down_arrow_disabled.png', 'down', TEXT_DIS)
    chevron('up_arrow.png', 'up', TEXT_BODY)
    chevron('up_arrow_disabled.png', 'up', TEXT_DIS)
    chevron('right_arrow.png', 'right', TEXT_BODY)
    chevron('right_arrow_disabled.png', 'right', TEXT_DIS)

    # ---- tree branch chevrons (square, 20x20 -> shown at 10px) -------------
    def branch(name, direction, colour):
        c = new(20, 20)
        if direction == 'closed':
            pts = [(7.5, 5.0), (13.0, 10.0), (7.5, 15.0)]
        else:
            pts = [(5.0, 7.5), (10.0, 13.0), (15.0, 7.5)]
        stroke_path(c, pts, colour, 2.2)
        made.append(save(c, name, outdir))

    branch('icon_branch_closed.png', 'closed', TEXT_DIM)
    branch('icon_branch_open.png', 'open', TEXT_DIM)

    # ---- dock / tab chrome glyphs -----------------------------------------
    def close_icon(name, colour):
        c = new(20, 20)
        stroke_path(c, [(6.5, 6.5), (13.5, 13.5)], colour, 1.8)
        stroke_path(c, [(13.5, 6.5), (6.5, 13.5)], colour, 1.8)
        made.append(save(c, name, outdir))

    close_icon('icon_close.png', TEXT_DIM)
    close_icon('icon_close_hover.png', TEXT_HI)

    # qproperty-icon is applied at polish time, so a PNG icon named by the
    # sheet cannot restate itself when the button's pseudo-state changes: ONE
    # pen has to serve the resting title bar and every hover/press fill. The
    # resting bar is bg.void, where ink is 1.00:1 -- invisible -- so the pen
    # must be light, and text.hi is 16.93:1 there.
    #
    # That fixes the pen and leaves the FILL as the only free variable, which
    # is where the sheet used to fail: against bad #E86F62 this glyph measures
    # 2.68:1, not the "ink on bad, 6.31:1" the sheet's comment asserted -- that
    # described a glyph nothing here has ever written. theme.qss now darkens
    # the close fills to close.hover #A75047 and close.press #974840, on which
    # this pen measures 4.75:1 and 5.56:1, and the plates still read 3.56:1 and
    # 3.04:1 against the bar as state marks. Both keep bad's hue.
    #
    # So: change this colour and the two fills in theme.qss stop clearing the
    # floor. They are a pair.
    close_icon('icon_window_close.png', TEXT_HI)

    # float / restore: two offset outlines
    def restore_icon(name, colour):
        c = new(20, 20)
        stroke_path(c, [(8.0, 5.5), (14.5, 5.5), (14.5, 12.0)], colour, 1.6)
        stroke_path(c, [(5.5, 8.0), (11.5, 8.0), (11.5, 14.5),
                        (5.5, 14.5), (5.5, 8.0)], colour, 1.6)
        made.append(save(c, name, outdir))

    restore_icon('icon_restore.png', TEXT_DIM)
    restore_icon('icon_undock.png', TEXT_DIM)

    c = new(20, 20)
    stroke_path(c, [(5.5, 5.5), (14.5, 5.5), (14.5, 14.5), (5.5, 14.5), (5.5, 5.5)],
                TEXT_BODY, 1.6)
    made.append(save(c, 'icon_window_maximize.png', outdir))

    c = new(20, 20)
    stroke_path(c, [(5.5, 10.0), (14.5, 10.0)], TEXT_BODY, 1.6)
    made.append(save(c, 'icon_window_minimize.png', outdir))

    # ---- size grip: three diagonal pips ------------------------------------
    c = new(16, 16)
    for off in (0, 4, 8):
        stroke_path(c, [(13.0 - off, 14.0), (14.0, 13.0 - off)], CONTROL_HI, 1.6)
    made.append(save(c, 'sizegrip.png', outdir))

    # ---- toolbar / splitter grip dots --------------------------------------
    c = new(8, 24)
    for y in (7.0, 12.0, 17.0):
        c.fill_sdf(sdf_circle(4.0 * SS, y * SS, 1.1 * SS), CONTROL_HI)
    made.append(save(c, 'handle_vertical.png', outdir))

    c = new(24, 8)
    for x in (7.0, 12.0, 17.0):
        c.fill_sdf(sdf_circle(x * SS, 4.0 * SS, 1.1 * SS), CONTROL_HI)
    made.append(save(c, 'handle_horizontal.png', outdir))

    # ---- menu check mark (no box, for QMenu::indicator) --------------------
    for name, colour in (('icon_menu_check.png', ACCENT),
                         ('icon_menu_check_disabled.png', TEXT_DIS)):
        c = new(SZ, SZ)
        stroke_path(c, CHECK, colour, 3.2)
        made.append(save(c, name, outdir))

    return made


if __name__ == '__main__':
    out = sys.argv[1]
    names = build(out)
    print('wrote %d files to %s' % (len(names), out))
    for n in sorted(names):
        print('  ', n)
