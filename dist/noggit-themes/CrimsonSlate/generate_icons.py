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

EVERY ICON IS EMITTED TWICE, as `name.png` and `name@2x.png`. The second is
not decoration: a QSS `url()` pixmap has no devicePixelRatio of its own, so
Qt loads the file at 1x and the style scales whatever it got up to the device
rect. On the 2.0-ratio display this theme is drawn for, twelve of the
thirty-four assets were being magnified rather than reduced -- measured as
source pixels against the device pixels each one actually covers:

    sizegrip.png              16x16 src -> 14px box -> 28x28 dev   3.06x short
    handle_vertical.png        8x24 src ->  8px wide -> 16 wide    4.00x short
    handle_horizontal.png     24x8  src ->  8px tall -> 16 tall    4.00x short
    icon_close.png            20x20 src -> 20x20 nat -> 40x40 dev  4.00x short
    icon_close_hover.png      20x20 src -> 20x20 nat -> 40x40 dev  4.00x short
    icon_branch_closed.png    20x20 src -> 20x20 nat -> 40x40 dev  4.00x short
    icon_branch_open.png      20x20 src -> 20x20 nat -> 40x40 dev  4.00x short
    icon_undock.png           20x20 src -> 14px icon -> 28x28 dev  1.96x short
    icon_window_close.png     20x20 src -> 12px icon -> 24x24 dev  1.44x short
    icon_window_minimize.png  20x20 src -> 12px icon -> 24x24 dev  1.44x short
    icon_restore.png          20x20 src -> 11px icon -> 22x22 dev  1.21x short
    icon_window_maximize.png  20x20 src -> 11px icon -> 22x22 dev  1.21x short

(`nat` = the sub-control takes its size from the image because the sheet
declares no width/height for that rule; the figure is a pixel-COUNT ratio.)

The other twenty-two were already at or above their device size -- the 32px
check box and radio rasters land exactly 1:1 on a 16px indicator, and the
16x10 chevrons exactly 1:1 on an 8x5 arrow -- so this pass does not move a
single visible edge on those, it only removes the ceiling.

WHY THE @2x FILENAME AND NOT A BIGGER BASE FILE. Both were checked against
the Qt sources rather than assumed. `image: url(x.png)` reaches
QCss::Declaration::iconValue (qcssparser.cpp), which builds a plain
`QIcon(uri)`; QIcon::addFile (qicon.cpp) then calls qt_findAtNxFile and adds
`x@2x.png` as a second entry whenever qApp->devicePixelRatio() > 1. The @Nx
probe is a QFile::exists on the SAME string the base file resolved through,
so a relative theme path needs no sheet change to find it. Enlarging the base
file instead would have been wrong: ValueExtractor::extractImage reads the
sub-control's natural size straight out of the base file with a QImageReader,
so a 40px icon_close.png would have made the tab close button 40 logical px.
Adding a sidecar leaves that reader looking at an unchanged 20x20 file.

The whole mechanism is gated on Qt::AA_UseHighDpiPixmaps -- qicon.cpp's
qt_effective_device_pixel_ratio returns a flat 1.0 without it, and then
neither the @2x entry nor the device-sized request ever happens. It is set in
ApplicationEntry.cpp. On a 1.0-ratio display qt_findAtNxFile returns early
and only the base files are ever opened, so nothing here costs a 1x user
anything but disk.
"""
import math
import os
import struct
import sys
import zlib

# Box-filter taps per output pixel, per axis. This is quality, not size.
SUPERSAMPLE = 4

# The logical-unit-to-raster scale, rebound once per output pass by build().
# Every drawing helper multiplies its logical coordinates by SS and save()
# divides by SUPERSAMPLE, so SS = SUPERSAMPLE * n emits an n-times-size PNG
# from geometry that is not touched -- which is the point, because a hand-
# written 2x variant would be a second drawing to keep in sync with the first.
SS = SUPERSAMPLE

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
CONTROL    = (0x56, 0x50, 0x49)   # stroke -- disabled and sub-threshold only
CONTROL_HI = (0x74, 0x6D, 0x64)   # stroke.hi, separators on bg.overlay
EDGE       = (0x8A, 0x83, 0x78)   # edge, the visible edge of a control
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
#
# RADIUS STAYS 8.0 (4px) EVEN THOUGH THE SHEET MOVED ITS CONTROL TIER FROM 5px
# TO 6px, and the arithmetic is why. inset 2.5 makes the drawn box 27 units,
# i.e. 13.5px on screen. The control tier is 6px on a 28px control, a ratio of
# 0.214, and 0.214 x 13.5 = 2.9px -- so the 4px already here is the GENEROUS
# end of that proportion and 5px would be 0.37 of the side, a squircle rather
# than a rounded square. Radius follows proportion at this size, not the tier
# number.
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
    """Downsample and write, decorating the name on any pass above 1x.

    The suffix has to go before the extension and nowhere else: qt_findAtNxFile
    inserts "@2x" at the last dot of the base name, so `icon_close@2x.png` is
    found and `icon_close.png@2x` is not.
    """
    scale = SS // SUPERSAMPLE

    if scale > 1:
        stem, ext = os.path.splitext(name)
        name = '%s@%dx%s' % (stem, scale, ext)

    c.downsample(SUPERSAMPLE).to_png(os.path.join(outdir, name))
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
def build(outdir, out_scale=1):
    global SS

    # Rebound rather than threaded through every helper because the helpers all
    # read it as the one logical-to-raster factor, and a second parameter on
    # each of them would be a second thing to forget at a call site.
    SS = SUPERSAMPLE * out_scale

    os.makedirs(outdir, exist_ok=True)
    made = []
    SZ = 32           # checkbox / radio logical size (drawn at 2x of a 16px box)
    CHECK = [(10.5, 16.5), (14.2, 20.4), (21.8, 11.8)]

    # ---- check boxes -------------------------------------------------------
    # THE UNCHECKED RING IS edge AND ITS HOVER IS text.dim, matching the border
    # the sheet now draws on every other enabled control. It was stroke on rest
    # and stroke.hi on hover. A ring here has TWO neighbours and has to clear
    # 3:1 against both -- the bg.void fill it encloses and the bg.panel the
    # indicator sits on -- and the old pair cleared that on neither surface it
    # needed to:
    #     stroke    #565049   2.422:1 on the fill   1.894:1 on the panel
    #     stroke.hi #746D64   3.775:1 on the fill   2.952:1 on the panel
    # THE PREVIOUS REVISION OF THIS COMMENT PUT stroke.hi AT 2.952:1 ON THE
    # FILL AND CONCLUDED "all four under the 3:1 floor". Both are wrong: 2.952
    # is stroke.hi's PANEL figure duplicated into the fill slot, the fill
    # figure is 3.775, and three of the four were under the floor, not four.
    # The change still stands and for the stated reason -- the hover ring was
    # the one that had to carry the state and it failed on the panel, so the
    # indicator was sub-threshold on the nine-radio brush grid that is the most
    # prominent control in the editor. What is no longer claimed is that every
    # figure failed.
    # edge is 5.138:1 on the fill and 4.018:1 on the panel; text.dim is 9.699:1
    # and 7.585:1, and sits 1.888:1 above edge, so the hover is a step in the
    # same direction the sheet's button hover moves. Both clear 3:1 on both
    # surfaces, which is the whole point. Change one of these and re-run this
    # script, or the PNG rings and the CSS-drawn borders beside them drift
    # apart.
    specs = [
        ('icon_checkbox_unchecked.png',          INPUT,   EDGE,       None),
        ('icon_checkbox_unchecked_hover.png',    INPUT,   TEXT_DIM,   None),
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
        ('icon_radiobutton_unchecked.png',          INPUT,      EDGE,       None),
        ('icon_radiobutton_unchecked_hover.png',    INPUT,      TEXT_DIM,   None),
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

    # 1x first so the @2x pass cannot be the thing that defines the geometry:
    # if the two ever disagree the base file is the one the sheet measures.
    names = build(out, 1) + build(out, 2)

    print('wrote %d files to %s' % (len(names), out))
    for n in sorted(names):
        print('  ', n)
