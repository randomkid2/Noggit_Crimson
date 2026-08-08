#!/usr/bin/env python3
# This file is part of Noggit3, licensed under GNU General Public License (version 3).
"""Draw the Crimson Slate icon set: one SVG plus two PNGs per icon.

Usage
-----
    cd dist/noggit-themes/CrimsonSlate/icons
    python generate_icon_set.py .

That rewrites every ``*.svg``, ``*.png``, ``*@2x.png``, ``contact_sheet.png``
and ``manifest.json`` in this directory. The SVGs are the source of truth; the
PNGs are build output that happens to be committed so the application needs no
Python at build time and CMakeLists.txt needs no new target.

    python generate_icon_set.py . --verify

renders a handful of icons and prints the alpha profile across a few known
axis-aligned strokes, so "is this crisp or is it mud" is a measurement rather
than an opinion. See ``verify()`` at the bottom.

Why PNG and not SVG at runtime
------------------------------
``Qt5Svg.lib`` exists in the Qt install but CMakeLists.txt links neither the
module nor the ``qsvg`` image-format plugin, so ``QIcon(":/x.svg")`` returns a
null icon *silently*. PNG needs nothing. The ``@2x`` twin is picked up by Qt's
own high-DPI file convention (``qt_findAtNxFile``), so a 200 % display gets the
48 px art without any code asking for it.

The PNGs are white (#FFFFFF) alpha masks, not pre-coloured art. The icon has to
appear in four states in four themes; baking a colour in would mean sixteen
copies of every file. Tint at paint time:

    QPixmap pm (":/…/tool_raise_lower.png");
    QPainter p (&pm);
    p.setCompositionMode (QPainter::CompositionMode_SourceIn);
    p.fillRect (pm.rect(), pen_colour);

Drawing system
--------------
Everything is authored on a 24x24 grid, y down, SVG convention. Strokes are
2 grid units wide with round caps and round joins, and every axis-aligned
centreline sits on an integer coordinate — at 24 px one grid unit is one pixel,
so a 2-unit stroke centred at y=15 covers device rows 14 and 15 *exactly* and
is crisp. At 48 px it covers four rows, also exactly. A centreline on a half
coordinate would cover three rows at partial coverage and read as mud, which is
why there are no x.5 stroke centrelines anywhere in the table below.

No two parallel strokes are closer than 3 grid units, because at 16 px the
2-unit stroke collapses to one device pixel and anything tighter merges.

Path syntax is the absolute-only subset agreed with the C++ side: ``M x y``,
``L x y``, ``C x1 y1 x2 y2 x y``, ``Q x1 y1 x y``, ``Z``, plus two conveniences
``O cx cy r`` (circle) and ``R x y w h r`` (rounded rect). No relative
commands, no arcs — circles come from ``O``, everything else is approximated
with cubics by the helpers below. The same strings are copied verbatim into
manifest.json so the C++ ``IconTable`` can consume them without a second
authoring pass.

Pure stdlib: no Pillow, no cairosvg, no network. The PNG encoder is lifted from
``../generate_icons.py`` (the widget-chrome generator in the parent directory)
rather than written a third time; only the rasteriser below it is new. That one
supersamples; this one uses the exact signed distance to a union of capsules,
which is both faster and sharper — the union of convex parts has an exact SDF
equal to the minimum of the parts' SDFs, so ``clamp(0.5 - d)`` is analytic
coverage, not an estimate.
"""

import json
import math
import os
import struct
import sys
import zlib

GRID = 24.0            # authoring grid
STROKE_UNITS = 2.0     # nominal stroke weight, in grid units
SIZES = (24, 48)       # 1x and Qt's @2x


# --------------------------------------------------------------- png output ---
# Encoder taken from ../generate_icons.py so the theme has one PNG writer, not
# two that can drift. Kept as a free function here because this generator only
# ever writes straight-alpha white, never a colour ramp.
def write_png(path, w, h, rgba):
    """rgba is a flat bytearray of w*h*4, straight (non-premultiplied) alpha."""
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)                       # filter type 0, none
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 9))
    png += chunk(b'IEND', b'')
    with open(path, 'wb') as fh:
        fh.write(png)


# ------------------------------------------------------------- path parsing ---
def _nums(s):
    out, cur = [], ''
    for ch in s:
        if ch in '-+' and cur and cur[-1] not in 'eE':
            out.append(float(cur))
            cur = ch
        elif ch in ' ,\t\n':
            if cur:
                out.append(float(cur))
                cur = ''
        else:
            cur += ch
    if cur:
        out.append(float(cur))
    return out


def _cubic(p0, p1, p2, p3, n=24):
    pts = []
    for i in range(1, n + 1):
        t = i / float(n)
        u = 1.0 - t
        x = (u * u * u * p0[0] + 3 * u * u * t * p1[0]
             + 3 * u * t * t * p2[0] + t * t * t * p3[0])
        y = (u * u * u * p0[1] + 3 * u * u * t * p1[1]
             + 3 * u * t * t * p2[1] + t * t * t * p3[1])
        pts.append((x, y))
    return pts


def _quad(p0, p1, p2, n=18):
    pts = []
    for i in range(1, n + 1):
        t = i / float(n)
        u = 1.0 - t
        pts.append((u * u * p0[0] + 2 * u * t * p1[0] + t * t * p2[0],
                    u * u * p0[1] + 2 * u * t * p1[1] + t * t * p2[1]))
    return pts


def flatten(d):
    """'d' -> [(points, closed)] in grid units. Absolute-only subset + O and R."""
    d = d.strip()
    if d.startswith('O '):
        cx, cy, r = _nums(d[2:])
        n = 72
        pts = [(cx + r * math.cos(2 * math.pi * i / n),
                cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]
        return [(pts, True)]
    if d.startswith('R '):
        x, y, w, h, r = _nums(d[2:])
        r = min(r, w / 2.0, h / 2.0)
        pts = []
        # corners: (centre, start angle in screen degrees) walked clockwise
        for cx, cy, a0 in ((x + w - r, y + r, -90.0), (x + w - r, y + h - r, 0.0),
                           (x + r, y + h - r, 90.0), (x + r, y + r, 180.0)):
            if r <= 0.0:
                pts.append((cx, cy))
                continue
            for i in range(13):
                a = math.radians(a0 + 90.0 * i / 12.0)
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        return [(pts, True)]

    subs, cur, start = [], [], None
    i, toks = 0, []
    # split into (command, numbers)
    cmd, buf = None, ''
    for ch in d:
        if ch in 'MLCQZ':
            if cmd:
                toks.append((cmd, _nums(buf)))
            cmd, buf = ch, ''
        else:
            buf += ch
    if cmd:
        toks.append((cmd, _nums(buf)))

    for cmd, n in toks:
        if cmd == 'M':
            if len(cur) > 1:
                subs.append((cur, False))
            cur = [(n[0], n[1])]
            start = cur[0]
            for k in range(2, len(n), 2):
                cur.append((n[k], n[k + 1]))
        elif cmd == 'L':
            for k in range(0, len(n), 2):
                cur.append((n[k], n[k + 1]))
        elif cmd == 'C':
            for k in range(0, len(n), 6):
                cur += _cubic(cur[-1], (n[k], n[k + 1]),
                              (n[k + 2], n[k + 3]), (n[k + 4], n[k + 5]))
        elif cmd == 'Q':
            for k in range(0, len(n), 4):
                cur += _quad(cur[-1], (n[k], n[k + 1]), (n[k + 2], n[k + 3]))
        elif cmd == 'Z':
            if len(cur) > 1:
                subs.append((cur, True))
            cur = [start] if start else []
    if len(cur) > 1:
        subs.append((cur, False))
    return subs
    _ = i, toks


# ------------------------------------------------------------- path helpers ---
# These emit real 'd' strings so the manifest carries pasteable geometry rather
# than a Python call the C++ side would have to re-implement.
def fmt(v):
    s = '%.3f' % v
    s = s.rstrip('0').rstrip('.')
    return '0' if s in ('-0', '') else s


def pt(p):
    return '%s,%s' % (fmt(p[0]), fmt(p[1]))


def polyline(pts, close=False):
    d = 'M' + pt(pts[0]) + ' L' + ' L'.join(pt(p) for p in pts[1:])
    return d + ' Z' if close else d


def _on(cx, cy, r, deg):
    a = math.radians(deg)
    return (cx + r * math.cos(a), cy + r * math.sin(a))


def arc(cx, cy, r, a0, a1, move=True):
    """Circular arc as cubics. Angles in screen degrees: 0 = right, 90 = down.

    a1 > a0 sweeps clockwise on screen; a1 < a0 sweeps counter-clockwise.
    """
    steps = max(1, int(math.ceil(abs(a1 - a0) / 90.0)))
    span = (a1 - a0) / steps
    k = 4.0 / 3.0 * math.tan(math.radians(span) / 4.0)
    d = ('M' + pt(_on(cx, cy, r, a0))) if move else ''
    for s in range(steps):
        b0 = a0 + span * s
        b1 = b0 + span
        p0, p1 = _on(cx, cy, r, b0), _on(cx, cy, r, b1)
        t0 = (-math.sin(math.radians(b0)) * r, math.cos(math.radians(b0)) * r)
        t1 = (-math.sin(math.radians(b1)) * r, math.cos(math.radians(b1)) * r)
        d += ' C%s %s %s' % (pt((p0[0] + k * t0[0], p0[1] + k * t0[1])),
                             pt((p1[0] - k * t1[0], p1[1] - k * t1[1])), pt(p1))
    return d.strip()


def ellipse(cx, cy, rx, ry):
    k = 0.5522847498
    p = [(cx, cy - ry), (cx + rx, cy), (cx, cy + ry), (cx - rx, cy)]
    d = 'M' + pt(p[0])
    d += ' C%s %s %s' % (pt((cx + k * rx, cy - ry)), pt((cx + rx, cy - k * ry)), pt(p[1]))
    d += ' C%s %s %s' % (pt((cx + rx, cy + k * ry)), pt((cx + k * rx, cy + ry)), pt(p[2]))
    d += ' C%s %s %s' % (pt((cx - k * rx, cy + ry)), pt((cx - rx, cy + k * ry)), pt(p[3]))
    d += ' C%s %s %s' % (pt((cx - rx, cy - k * ry)), pt((cx - k * rx, cy - ry)), pt(p[0]))
    return d + ' Z'


def head(tip, tail, size=2.6, spread=36.0):
    """Two-stroke chevron arrowhead at 'tip', opening back towards 'tail'."""
    a = math.atan2(tip[1] - tail[1], tip[0] - tail[0])
    out = []
    for s in (spread, -spread):
        b = a + math.pi + math.radians(s)
        out.append((tip[0] + size * math.cos(b), tip[1] + size * math.sin(b)))
    return 'M%s L%s L%s' % (pt(out[0]), pt(tip), pt(out[1]))


def arc_head(cx, cy, r, at, clockwise, size=2.6):
    """Arrowhead sitting on a circle at screen-angle 'at', pointing along travel."""
    p = _on(cx, cy, r, at)
    t = (-math.sin(math.radians(at)), math.cos(math.radians(at)))
    if not clockwise:
        t = (-t[0], -t[1])
    return head(p, (p[0] - t[0], p[1] - t[1]), size)


def star(cx, cy, r_out, r_in, points=5):
    pts = []
    for i in range(points * 2):
        r = r_out if i % 2 == 0 else r_in
        a = -90.0 + 180.0 * i / points
        pts.append(_on(cx, cy, r, a))
    return polyline(pts, close=True)


def rays(cx, cy, r0, r1, count, phase=0.0):
    out = []
    for i in range(count):
        a = phase + 360.0 * i / count
        out.append('M%s L%s' % (pt(_on(cx, cy, r0, a)), pt(_on(cx, cy, r1, a))))
    return ' '.join(out)


def dots(coords, r):
    return [F('O %s %s %s' % (fmt(x), fmt(y), fmt(r))) for x, y in coords]


def gapped_polygon(pts, gap):
    """Closed polygon with every edge pulled back from its vertices.

    Used where the *vertices* are the subject (the vertex-paint triangle): a
    continuous outline with dots on top just thickens the corners, whereas an
    edge that stops short leaves the dot legible as a separate mark.
    """
    out = []
    n = len(pts)
    for i in range(n):
        a, b = pts[i], pts[(i + 1) % n]
        L = math.hypot(b[0] - a[0], b[1] - a[1])
        if L <= 2 * gap:
            continue
        t0, t1 = gap / L, 1.0 - gap / L
        out.append('M%s L%s'
                   % (pt((a[0] + (b[0] - a[0]) * t0, a[1] + (b[1] - a[1]) * t0)),
                      pt((a[0] + (b[0] - a[0]) * t1, a[1] + (b[1] - a[1]) * t1))))
    return ' '.join(out)


def xf(pts, deg, tx, ty):
    """Rotate (screen degrees, clockwise positive) then translate."""
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return [(x * c - y * s + tx, x * s + y * c + ty) for x, y in pts]


def _wrench():
    """An open-end spanner: two prongs, a rounded throat, a shaft, at 45 degrees.

    Two earlier attempts failed at 96 px. A C-arc plus a diagonal shaft reads as
    a magnifying glass; an open hexagon plus a shaft reads as a pennant. What
    makes a spanner unambiguous is the *pair of parallel prongs* with a gap
    between them, so that is drawn literally, authored upright and rotated into
    place. Prong centrelines are 6 grid units apart, which clears the 3-unit
    minimum at 16 px.
    """
    ang, tx, ty = -45.0, 13.06, 13.06
    (l0, l1) = xf([(-3, -9), (-3, -4)], ang, tx, ty)
    (r0, r1) = xf([(3, -9), (3, -4)], ang, tx, ty)
    throat = xf([(-3, -4), (-3.2, 1.2), (3.2, 1.2), (3, -4)], ang, tx, ty)
    (s0, s1) = xf([(0, -0.2), (0, 9)], ang, tx, ty)
    return [S('M%s L%s' % (pt(l0), pt(l1))),
            S('M%s L%s' % (pt(r0), pt(r1))),
            S('M%s C%s %s %s' % (pt(throat[0]), pt(throat[1]),
                                 pt(throat[2]), pt(throat[3]))),
            S('M%s L%s' % (pt(s0), pt(s1)))]


# -------------------------------------------------------------- subpath ops ---
def S(d, **kw):
    return dict(d=d, op='stroke', **kw)


def F(d, **kw):
    return dict(d=d, op='fill', **kw)


def D(d, dash, **kw):
    return dict(d=d, op='stroke', dash=dash, **kw)


def EF(d):
    """Subtract a filled region from what has been drawn so far."""
    return dict(d=d, op='erase_fill')


def ES(d, w):
    """Subtract a stroked band — used to punch a gap in an outline."""
    return dict(d=d, op='erase_stroke', w=w)


# --------------------------------------------------------------- rasteriser ---
def _clamp01(v):
    return 0.0 if v < 0.0 else (1.0 if v > 1.0 else v)


def _dash_split(pts, dash):
    """Split a polyline into on-runs. dash = (on, off) in grid units."""
    on, off = dash
    runs, cur = [], [pts[0]]
    phase, drawing = 0.0, True
    for i in range(len(pts) - 1):
        ax, ay = pts[i]
        bx, by = pts[i + 1]
        seg = math.hypot(bx - ax, by - ay)
        t = 0.0
        while t < seg:
            limit = (on if drawing else off) - phase
            step = min(limit, seg - t)
            t2 = t + step
            p2 = (ax + (bx - ax) * t2 / seg, ay + (by - ay) * t2 / seg)
            if drawing:
                cur.append(p2)
            phase += step
            t = t2
            if phase >= (on if drawing else off) - 1e-9:
                if drawing and len(cur) > 1:
                    runs.append(cur)
                drawing = not drawing
                cur = [p2]
                phase = 0.0
    if drawing and len(cur) > 1:
        runs.append(cur)
    return runs


def _stroke(buf, size, polys, hw_px, scale, dash=None):
    for pts, closed in polys:
        p = list(pts)
        if closed and (abs(p[0][0] - p[-1][0]) > 1e-9 or abs(p[0][1] - p[-1][1]) > 1e-9):
            p.append(p[0])
        runs = _dash_split(p, dash) if dash else [p]
        for run in runs:
            q = [(x * scale, y * scale) for x, y in run]
            for i in range(len(q) - 1):
                _capsule(buf, size, q[i], q[i + 1], hw_px)


def _capsule(buf, size, a, b, hw):
    ax, ay = a
    bx, by = b
    x0 = max(0, int(math.floor(min(ax, bx) - hw - 1.0)))
    x1 = min(size - 1, int(math.ceil(max(ax, bx) + hw + 1.0)))
    y0 = max(0, int(math.floor(min(ay, by) - hw - 1.0)))
    y1 = min(size - 1, int(math.ceil(max(ay, by) + hw + 1.0)))
    vx, vy = bx - ax, by - ay
    L2 = vx * vx + vy * vy
    for py in range(y0, y1 + 1):
        yc = py + 0.5
        row = py * size
        for px in range(x0, x1 + 1):
            xc = px + 0.5
            wx, wy = xc - ax, yc - ay
            t = 0.0 if L2 == 0.0 else (wx * vx + wy * vy) / L2
            t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
            dx, dy = xc - (ax + t * vx), yc - (ay + t * vy)
            cov = _clamp01(0.5 - (math.sqrt(dx * dx + dy * dy) - hw))
            if cov > buf[row + px]:
                buf[row + px] = cov


def _fill(buf, size, polys, scale, sub=16):
    edges = []
    for pts, _closed in polys:
        q = [(x * scale, y * scale) for x, y in pts]
        if abs(q[0][0] - q[-1][0]) > 1e-9 or abs(q[0][1] - q[-1][1]) > 1e-9:
            q.append(q[0])
        for i in range(len(q) - 1):
            if q[i][1] != q[i + 1][1]:
                edges.append((q[i], q[i + 1]))
    if not edges:
        return
    ys = [e[0][1] for e in edges] + [e[1][1] for e in edges]
    y0 = max(0, int(math.floor(min(ys))))
    y1 = min(size - 1, int(math.ceil(max(ys))))
    w = 1.0 / sub
    for py in range(y0, y1 + 1):
        row = py * size
        for k in range(sub):
            y = py + (k + 0.5) / sub
            xs = []
            for a, b in edges:
                if (a[1] <= y < b[1]) or (b[1] <= y < a[1]):
                    t = (y - a[1]) / (b[1] - a[1])
                    xs.append((a[0] + t * (b[0] - a[0]), 1 if b[1] > a[1] else -1))
            if not xs:
                continue
            xs.sort()
            wind, sx = 0, None
            for x, dirn in xs:
                prev = wind
                wind += dirn
                if prev == 0 and wind != 0:
                    sx = x
                elif prev != 0 and wind == 0 and sx is not None:
                    _span(buf, size, row, sx, x, w)
                    sx = None


def _span(buf, size, row, x0, x1, weight):
    if x1 <= x0:
        return
    i0 = max(0, int(math.floor(x0)))
    i1 = min(size - 1, int(math.ceil(x1)) - 1)
    for px in range(i0, i1 + 1):
        cov = min(x1, px + 1.0) - max(x0, float(px))
        if cov > 0.0:
            v = buf[row + px] + cov * weight
            buf[row + px] = 1.0 if v > 1.0 else v


def render(subpaths, size):
    """-> flat list of size*size coverage floats."""
    scale = size / GRID
    cov = [0.0] * (size * size)
    for sp in subpaths:
        polys = flatten(sp['d'])
        op = sp['op']
        w_grid = sp.get('w', STROKE_UNITS)
        # device stroke width is a whole number of pixels at every size, so the
        # weight never lands on a half pixel and grey out.
        w_dev = max(1, int(round(size * w_grid / GRID)))
        hw = w_dev / 2.0
        if op in ('stroke', 'fill'):
            if op == 'stroke':
                _stroke(cov, size, polys, hw, scale, sp.get('dash'))
            else:
                _fill(cov, size, polys, scale)
        else:
            acc = [0.0] * (size * size)
            if op == 'erase_stroke':
                _stroke(acc, size, polys, hw, scale, sp.get('dash'))
            else:
                _fill(acc, size, polys, scale)
            for i in range(len(cov)):
                if acc[i] > 0.0:
                    cov[i] *= (1.0 - acc[i])
    return cov


def cov_to_rgba(cov, tint=(255, 255, 255)):
    out = bytearray()
    r, g, b = tint
    for c in cov:
        a = int(round(_clamp01(c) * 255.0))
        out += bytes((r, g, b, a))
    return out


# --------------------------------------------------------------- svg output ---
_SVG_HEAD = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" '
             'width="24" height="24" fill="none" stroke="currentColor" '
             'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">')


def _svg_shape(sp, extra=''):
    d = sp['d'].strip()
    dash = sp.get('dash')
    a = extra
    if sp.get('w') and sp['w'] != STROKE_UNITS:
        a += ' stroke-width="%s"' % fmt(sp['w'])
    if dash:
        a += ' stroke-dasharray="%s %s"' % (fmt(dash[0]), fmt(dash[1]))
    if sp['op'] in ('fill', 'erase_fill'):
        a += ' fill="currentColor" stroke="none"'
    if d.startswith('O '):
        cx, cy, r = _nums(d[2:])
        return '<circle cx="%s" cy="%s" r="%s"%s/>' % (fmt(cx), fmt(cy), fmt(r), a)
    if d.startswith('R '):
        x, y, w, h, r = _nums(d[2:])
        return ('<rect x="%s" y="%s" width="%s" height="%s" rx="%s"%s/>'
                % (fmt(x), fmt(y), fmt(w), fmt(h), fmt(r), a))
    return '<path d="%s"%s/>' % (d, a)


def to_svg(name, subpaths):
    """SVG twin of what render() rasterises, including the erase operations.

    SVG has no "subtract", so an erase becomes a luminance mask. The subtlety
    that a first attempt got wrong: an erase only affects what was drawn
    *before* it. Masking the whole icon also deletes the outline that is drawn
    afterwards -- for `visibility_baked_shadows` that silently removed the front
    square entirely, so the .svg and the .png disagreed. The subpath list is
    therefore split into blocks at each erase-to-draw boundary, and only the
    block preceding an erase is masked by it.
    """
    blocks, cur = [], {'draws': [], 'masks': []}
    for sp in subpaths:
        if sp['op'].startswith('erase'):
            cur['masks'].append(sp)
        else:
            if cur['masks']:
                blocks.append(cur)
                cur = {'draws': [], 'masks': []}
            cur['draws'].append(sp)
    blocks.append(cur)

    out = []
    for n, block in enumerate(blocks):
        shapes = [_svg_shape(sp) for sp in block['draws']]
        if not block['masks']:
            out += shapes
            continue
        mid = 'm_%s_%d' % (name, n)
        mask = ['<rect x="0" y="0" width="24" height="24" fill="#fff"/>']
        for sp in block['masks']:
            if sp['op'] == 'erase_fill':
                mask.append(_svg_shape(sp).replace('fill="currentColor"',
                                                   'fill="#000"'))
            else:
                mask.append(_svg_shape(sp, ' stroke="#000"'))
        out.append('<mask id="%s" maskUnits="userSpaceOnUse" x="0" y="0" '
                   'width="24" height="24">\n    %s\n  </mask>'
                   % (mid, '\n    '.join(mask)))
        out.append('<g mask="url(#%s)">\n    %s\n  </g>'
                   % (mid, '\n    '.join(shapes)))
    return ('%s\n  <title>%s</title>\n  %s\n</svg>\n'
            % (_SVG_HEAD, name, '\n  '.join(out)))


# =============================================================== THE ICONS ====
# Each entry: slug -> (font, enumerator, codepoint, group, tooltip, subpaths)
# font/enumerator/codepoint are None where no enumerator exists yet; those are
# the icons whose enumerator still has to be added to FontNoggit by hand when the
# set is integrated (see manifest.json, "needs_enumerator").

CONIFER = [S('M12,17 L12,20'), S('M10,8 L12,4 L14,8'),
           S('M9,12 L12,8 L15,12'), S('M8,17 L12,12 L16,17')]

BUILDING = [S('M5,9 L5,20 L19,20 L19,9'), S('M4,9 L12,4 L20,9'),
            S('M10,20 L10,16 L14,16 L14,20')]

# The nine falloff curves share one axis frame on purpose: nine bare curves are
# nine grey squiggles, but nine curves in an identical frame are comparable at a
# glance. The tooltip carries the name because Gaussian and Trigonom genuinely
# cannot be told apart by picture alone.
FRAME = S('M4,4 L4,20 L20,20')

EYE = [S('M3,12 C7,6 17,6 21,12'), S('M3,12 C7,18 17,18 21,12'),
       S('O 12 12 4'), F('O 12 12 1.2')]

TRASH = [S('M4,7 L20,7'), S('M9,7 L9,4 L15,4 L15,7'),
         S('M6.5,7 L8.5,20 L15.5,20 L17.5,7'),
         S('M10,10 L10.5,17'), S('M14,10 L13.5,17')]

PEN_BODY = [S('M5,18 L7,13 L16,4 L19,7 L10,16 Z'), S('M14,6 L17,9')]

GAMEPAD = [S('R 2 8 20 9 4'), S('M5,13 L11,13'), S('M8,10 L8,16'),
           F('O 16 11 1.3'), F('O 18 14 1.3')]

BULB = [S('O 12 9 4'), S('M10,16 L14,16'), S('M10,19 L14,19'),
        S('M12,2 L12,3.5'), S('M8,5 L6.5,3.5'), S('M16,5 L17.5,3.5')]

INFO_MARK = [S('O 12 12 8'), F('O 12 7.5 1.25'), S('M12,11 L12,17')]

TIMES = [S('M6,6 L18,18'), S('M18,6 L6,18')]

SCRIPT_MARK = [S('M10,7 L6,12 L10,17'), S('M14,7 L18,12 L14,17'),
               S('M15,6 L9,18')]


def _particles():
    a, b = (5.0, 19.0), (19.0, 5.0)
    radii = [1.5, 1.2, 1.0, 1.5, 0.9, 1.2, 1.0]
    jitter = [0.0, 1.4, -1.2, 0.8, -1.5, 1.2, 0.0]
    out = []
    for i, (r, j) in enumerate(zip(radii, jitter)):
        t = i / 6.0
        x = a[0] + (b[0] - a[0]) * t + 0.7071 * j
        y = a[1] + (b[1] - a[1]) * t + 0.7071 * j
        out.append(F('O %s %s %s' % (fmt(x), fmt(y), fmt(r))))
    return out


def _cog():
    # The teeth are deliberately heavier than the 2-unit house weight. With
    # 2-unit teeth this is a ship's wheel and is near-indistinguishable from
    # `sun`, which is also a circle with radial spokes; the weight and the solid
    # hub are what separate the two at 24 px.
    return [S('O 12 12 6.5'), S(rays(12, 12, 6.5, 9, 6, 0.0), w=3.4),
            F('O 12 12 2')]


def _sun():
    return [S('O 12 12 4.5'), S(rays(12, 12, 6.5, 9, 8, 0.0))]


def _volume():
    return [S('M4,9 L8,9 L14,4 L14,20 L8,15 L4,15 Z'),
            S(arc(14, 12, 4, -45, 45)),
            S(arc(14, 12, 7, -45, 45))]


def _undo(mirror=False):
    """A 270-degree ring whose head sits at the top pointing sideways.

    The first attempt put a 240-degree arc under the gap with a 2.6-unit head at
    the terminus; at 96 px it rendered as a bare "U" -- the head folded back onto
    the arc and vanished. Ending at the top (screen angle -90 / 270) puts the
    head tangent to the horizontal, where a 3.4-unit chevron is unmistakable,
    and the direction of travel then reads as the direction of the operation.
    """
    cx, cy, r = 12.0, 12.0, 7.0
    if not mirror:                       # undo: anticlockwise, head points left
        return [S(arc(cx, cy, r, 180.0, -90.0)),
                S(arc_head(cx, cy, r, -90.0, False, 3.4))]
    return [S(arc(cx, cy, r, 0.0, 270.0)),
            S(arc_head(cx, cy, r, 270.0, True, 3.4))]


ICONS = []


def icon(slug, font, enum, cp, group, tip, subpaths, alias=None):
    ICONS.append(dict(slug=slug, font=font, enum=enum, cp=cp, group=group,
                      tip=tip, subpaths=subpaths, alias=alias))


# ---- GROUP 1: the left tool strip -------------------------------------------
icon('tool_raise_lower', 'noggit', 'TOOL_RAISE_LOWER', 0xF89C, 'tool-strip',
     'Raise / Lower',
     [S('M4,15 L20,15'), S('M7,10 L12,5 L17,10'), S('M9,18 L12,21 L15,18')])

# A rule laid *across* a bumpy profile, which is what the spec asked for, is
# unreadable: at 96 px the ticks and the profile knot together and the thing
# reads as a Greek letter. Stacking the two states instead -- rough above,
# flat below, a chevron between them -- says "make this into that" with three
# strokes and no crossings.
icon('tool_flatten_blur', 'noggit', 'TOOL_FLATTEN_BLUR', 0xF89D, 'tool-strip',
     'Flatten / Blur',
     [S('M3,7 C6,7 6,3 9,3 C12,3 12,9 15,9 C18,9 18,5 21,5'),
      S('M9,12 L12,15 L15,12'), S('M3,19 L21,19')])

icon('tool_texture_paint', 'noggit', 'TOOL_TEXTURE_PAINT', 0xF89E, 'tool-strip',
     'Texture Painter',
     [S('R 4 5 12 5 2'), S('M16,7 L19,7 L19,17'),
      S('M4,14 L10,14'), S('M4,18 L12,18')])

icon('tool_hole_cutter', 'noggit', 'TOOL_HOLE_CUTTER', 0xF89F, 'tool-strip',
     'Hole Cutter',
     [S('M12,20 L4,20 L4,4 L20,4 L20,12'),
      D('M12,20 L12,12 L20,12', (3, 2.5))])

icon('tool_area_designator', 'noggit', 'TOOL_AREA_DESIGNATOR', 0xF8A0,
     'tool-strip', 'Area Designator',
     [S('M5,9 L11,4 L19,8 L17,18 L7,17 Z'), F('O 12 11 1.3')])

icon('tool_impass_designator', 'noggit', 'TOOL_IMPASS_DESIGNATOR', 0xF8A1,
     'tool-strip', 'Impassable Flag',
     [S('R 4 4 16 16 3'), S('M7,17 L17,7')])

icon('tool_water_editor', 'noggit', 'TOOL_WATER_EDITOR', 0xF8A2, 'tool-strip',
     'Water Editor',
     [S('M12,2 L12,8'), S('M9.5,5.5 L12,8 L14.5,5.5'),
      S('M4,13 C7,10 9,16 12,13 C15,10 17,16 20,13'),
      S('M4,18 C7,15 9,21 12,18 C15,15 17,21 20,18')])

# The edges stop 3 units short of each corner so the corner dots stay legible
# as separate marks; drawn as a continuous outline the dots just thicken the
# joins and the icon says "triangle", not "per-vertex". The apex dot is larger
# because one vertex being selected is the whole point of the tool.
icon('tool_vertex_paint', 'noggit', 'TOOL_VERTEX_PAINT', 0xF8A3, 'tool-strip',
     'Vertex Painter',
     [S(gapped_polygon([(12, 4), (20, 19), (4, 19)], 3.4)),
      F('O 12 4 2.3'), F('O 20 19 1.5'), F('O 4 19 1.5')])

icon('tool_object_editor', 'noggit', 'TOOL_OBJECT_EDITOR', 0xF8A4, 'tool-strip',
     'Object Editor',
     [S('M12,4 L18,8 L12,12 L6,8 Z'), S('M6,8 L6,16 L12,20 L18,16 L18,8'),
      S('M12,12 L12,21')])

icon('tool_minimap_editor', 'noggit', 'TOOL_MINIMAP_EDITOR', 0xF8A5,
     'tool-strip', 'Minimap Editor',
     [S('R 4 4 16 16 2'), S('M12,4 L12,20'), S('M4,12 L20,12'),
      F('R 6 6 5 5 1')])

icon('tool_stamp', 'noggit', 'TOOL_STAMP', 0xF8A6, 'tool-strip', 'Stamp',
     [S('R 5 13 14 5 2'), S('M8,13 L10,6 L14,6 L16,13'), S('M9,3 L15,3')])

icon('tool_light', 'noggit', 'TOOL_LIGHT', 0xF8D0, 'tool-strip', 'Light Editor',
     BULB)

# ChunkTool, ScriptingTool and ErosionTool all three return FontNoggit::INFO
# today, so three of the sixteen strip entries are pixel-identical to each other
# and to the "Details info" toolbar toggle. These three are the fix; they have
# no enumerator yet, which is the one thing integrating them still requires.
icon('tool_scripting', 'noggit', None, None, 'tool-strip', 'Scripting',
     SCRIPT_MARK)

# 4x4 of 4-unit cells is a solid mesh at 24 px -- a 2-unit stroke either side of
# a 4-unit cell leaves nothing -- and the highlighted cell disappears into it.
# 3x3 of 6-unit cells leaves 4 clear units per cell, so the filled centre reads.
icon('tool_chunk', 'noggit', None, None, 'tool-strip', 'Chunk Manipulator',
     [S('M3,3 L21,3 L21,21 L3,21 Z'), S('M9,3 L9,21'), S('M15,3 L15,21'),
      S('M3,9 L21,9'), S('M3,15 L21,15'), F('R 9 9 6 6 0')])

icon('tool_erosion', 'noggit', None, None, 'tool-strip', 'Erosion',
     [S('M3,19 C7,19 8,7 12,7 C16,7 17,19 21,19'),
      S('M11,9 L9,19'), S('M14,9 L17,19')])

icon('area_trigger', 'noggit', 'AREA_TRIGGER', 0xF8E0, 'tool-strip',
     'Area Trigger',
     [D('R 4 7 16 10 3', (4, 3)), F('O 12 12 1.3'), S('M12,4 L12,7')])

# ---- GROUP 2: the view toolbar ----------------------------------------------
icon('visibility_doodads', 'noggit', 'VISIBILITY_DOODADS', 0xF8AB, 'view-toolbar',
     'Doodads', CONIFER)

icon('visibility_wmo', 'noggit', 'VISIBILITY_WMO', 0xF8A9, 'view-toolbar',
     'WMOs', BUILDING)

icon('visibility_wmo_doodads', 'noggit', 'VISIBILITY_WMO_DOODADS', 0xF8AA,
     'view-toolbar', 'WMO doodads',
     [S('M4,13 L4,20 L13,20 L13,13'), S('M3,13 L8,9 L13,13'),
      S('M18,18 L18,20'), S('M16.5,14 L18,11 L19.5,14'), S('M16,18 L18,14 L20,18')])

# The enumerator name is a legacy misnomer -- this is the WMO *exterior* toggle.
# Renaming it is call-site churn for nothing, so only the drawing changes:
# outside solid, inside implied.
icon('ui_toggle', 'noggit', 'UI_TOGGLE', 0xF8C6, 'view-toolbar',
     'WMO exterior',
     [S('M12,4 L4,9 M5,9 L5,20 L12,20'),
      D('M12,4 L20,9', (3, 2.5)), D('M19,9 L19,20 L12,20', (3, 2.5)),
      D('M12,4 L12,20', (3, 2.5))])

icon('visibility_terrain', 'noggit', 'VISIBILITY_TERRAIN', 0xF8AE,
     'view-toolbar', 'Terrain',
     [S('M3,17 L9,9 L14,14 L21,7'), S('M3,20 L21,20')])

icon('visibility_water', 'noggit', 'VISIBILITY_WATER', 0xF8B3, 'view-toolbar',
     'Water',
     [S('M3,10 C7,6 9,14 12,10 C15,6 17,14 21,10'),
      S('M3,16 C7,12 9,20 12,16 C15,12 17,20 21,16')])

icon('visibility_lines', 'noggit', 'VISIBILITY_LINES', 0xF8AF, 'view-toolbar',
     'Lines',
     [S('M6,7 L18,7'), S('M4,12 L20,12'), S('M6,17 L18,17')])

icon('visibility_hole_lines', 'noggit', 'VISIBILITY_HOLE_LINES', 0xF8B7,
     'view-toolbar', 'Hole lines',
     [S('M4,5 L20,5'), S('M4,19 L20,19'),
      S('M4,12 L8,12'), S('M16,12 L20,12'), S('R 8 8 8 8 1')])

icon('visibility_wireframe', 'noggit', 'VISIBILITY_WIREFRAME', 0xF8B0,
     'view-toolbar', 'Wireframe',
     [S('M4,4 L20,4 L20,20 L4,20 Z'), S('M4,4 L20,20'), S('M20,4 L4,20'),
      S('M12,4 L12,20')])

icon('visibility_contours', 'noggit', 'VISIBILITY_CONTOURS', 0xF8B1,
     'view-toolbar', 'Contours',
     [S(ellipse(11.5, 13, 8.5, 7)), S(ellipse(12, 12.5, 5.5, 4.5)),
      S(ellipse(12.5, 12, 2.5, 2))])

icon('visibility_climb', 'noggit', 'VISIBILITY_CLIMB', 0xF8D2, 'view-toolbar',
     'Climb',
     [S('M6,20 L6,6 L20,20 Z'), S('M15,20 Q15,17.9 16.5,16.5')])

icon('visibility_vertex_painter', 'noggit', 'VISIBILITY_VERTEX_PAINTER', 0xF8D1,
     'view-toolbar', 'Vertex colour',
     [S('M12,5 L20,19 L4,19 Z'), F('O 12 5 1.4'),
      S('M8,17 L11,11'), S('M13,17 L16,12')])

icon('visibility_baked_shadows', 'noggit', 'VISIBILITY_BAKED_SHADOWS', 0xF8D3,
     'view-toolbar', 'Baked shadows',
     [F('R 10 10 10 10 2'), EF('R 4 4 10 10 2'), S('R 4 4 10 10 2')])

icon('visibility_animation', 'noggit', 'VISIBILITY_ANIMATION', 0xF8B8,
     'view-toolbar', 'Animations',
     [S('M6,9 L9,12 L6,15'), S('M10,8 L13,12 L10,16'), S('M14,7 L17,12 L14,17')])

icon('visibility_fog', 'noggit', 'VISIBILITY_FOG', 0xF8B2, 'view-toolbar', 'Fog',
     [S('M5,7 L19,7'), S('M3,11 L21,11'), S('M7,15 L19,15'), S('M4,19 L20,19')])

icon('visibility_flight_bounds', 'noggit', 'VISIBILITY_FLIGHT_BOUNDS', 0xF8DF,
     'view-toolbar', 'Flight bounds',
     [D('R 3 9 18 6 3', (4, 3)), S('M12,20 L12,4'), S('M9,7 L12,4 L15,7')])

icon('visibility_with_box', 'noggit', 'VISIBILITY_WITH_BOX', 0xF8AC,
     'view-toolbar', 'Models with box',
     [S('M12,16 L12,18'), S('M10,11 L12,8 L14,11'), S('M9,15 L12,11 L15,15'),
      S('M4,8 L4,4 L8,4'), S('M16,4 L20,4 L20,8'),
      S('M20,16 L20,20 L16,20'), S('M8,20 L4,20 L4,16')])

# No erase band here, unlike `eyeslash`. The conifer's arms sit within 20
# degrees of a 45-degree slash, so a band wide enough to read as a gap runs
# *along* the arms for their whole length and deletes the tree -- measured, not
# guessed. A plain slash over the same conifer `visibility_doodads` uses keeps
# the pair legible as the same subject, on and off.
icon('visibility_hidden_models', 'noggit', 'VISIBILITY_HIDDEN_MODELS', 0xF8B6,
     'view-toolbar', 'Hidden models', CONIFER + [S('M4,20 L20,4')])

icon('info', 'noggit', 'INFO', 0xF8BB, 'view-toolbar', 'Details info', INFO_MARK)

# ---- GROUP 3: the falloff curves --------------------------------------------
icon('falloff_flat', None, None, None, 'falloff', 'Flat',
     [FRAME, S('M4,8 L16,8 L16,20')])
icon('falloff_linear', None, None, None, 'falloff', 'Linear',
     [FRAME, S('M4,8 L20,20')])
icon('falloff_smooth', None, None, None, 'falloff', 'Smooth',
     [FRAME, S('M4,8 C10,8 14,20 20,20')])
icon('falloff_polynomial', None, None, None, 'falloff', 'Polynomial',
     [FRAME, S('M4,8 C13,8 17,12 20,20')])
icon('falloff_trigonom', None, None, None, 'falloff', 'Trigonom',
     [FRAME, S('M4,20 C8,20 8,8 12,8 C16,8 16,20 20,20')])
icon('falloff_quadratic', None, None, None, 'falloff', 'Quadratic',
     [FRAME, S('M4,8 Q4,20 20,20')])
icon('falloff_gaussian', None, None, None, 'falloff', 'Gaussian',
     [FRAME, S('M4,20 C9,20 9,9 12,8 C15,9 15,20 20,20')])
icon('falloff_vertex', None, None, None, 'falloff', 'Vertex',
     [FRAME, S('M5,17 L12,8 L19,17'),
      F('O 5 17 1.25'), F('O 12 8 1.25'), F('O 19 17 1.25')])
icon('falloff_script', None, None, None, 'falloff', 'Script',
     [FRAME, S('M10,9 L7,12 L10,15'), S('M14,9 L17,12 L14,15'),
      S('M14,8 L10,17')])

# ---- GROUP 4: gizmo, view mode, time ----------------------------------------
icon('gizmo_translate', 'noggit', 'GIZMO_TRANSLATE', 0xF8C9, 'gizmo', 'Translate',
     [S('M12,3 L12,21'), S('M3,12 L21,12'),
      S('M9.5,5.5 L12,3 L14.5,5.5'), S('M9.5,18.5 L12,21 L14.5,18.5'),
      S('M5.5,9.5 L3,12 L5.5,14.5'), S('M18.5,9.5 L21,12 L18.5,14.5')])

icon('gizmo_rotate', 'noggit', 'GIZMO_ROTATE', 0xF8CA, 'gizmo', 'Rotate',
     [S(arc(12, 12, 8, -30, 270)), S(arc_head(12, 12, 8, 270, True)),
      F('O 12 12 1.3')])

icon('gizmo_scale', 'noggit', 'GIZMO_SCALE', 0xF8CB, 'gizmo', 'Scale',
     [S('M6,18 L18,6'), S('M15,9 L15,3 L21,3 L21,9 Z'), F('R 4 16 4 4 0.5')])

icon('gizmo_visibility', 'noggit', 'GIZMO_VISIBILITY', 0xF8CC, 'gizmo',
     'Gizmo visibility',
     [S('M2,12 C5,7 13,7 16,12'), S('M2,12 C5,17 13,17 16,12'),
      S('O 9 12 3'), F('O 9 12 1.1'), S('M18,9 L21,12 L18,15')])

icon('gizmo_local', 'noggit', 'GIZMO_LOCAL', 0xF8CE, 'gizmo', 'Local space',
     [S('M12,6 L20,11 L12,16 L4,11 Z'), S('M7,14 L17,9'),
      S(head((17, 9), (7, 14)))])

icon('gizmo_global', 'noggit', 'GIZMO_GLOBAL', 0xF8CD, 'gizmo', 'World space',
     [S('O 12 12 8'), S(ellipse(12, 12, 8, 3)), S(ellipse(12, 12, 3, 8))])

icon('gizmo_visibility_all', 'noggit', 'GIZMO_VISIBILITY_ALL', 0xF8CF, 'gizmo',
     'Show all gizmos',
     [S('M7,4.5 L10.5,8 L7,11.5 L3.5,8 Z'),
      S('M17,4.5 L20.5,8 L17,11.5 L13.5,8 Z'),
      S('M12,13.5 L15.5,17 L12,20.5 L8.5,17 Z')])

# Two rejected attempts, both of which read as "download to a tray": a chevron
# in a box over a rule, and a slab with an arrow descending onto it. Any
# downward arrow above a horizontal container is a download glyph and no amount
# of tuning changes that. The CAD view-cube convention -- a solid on which the
# face you are looking at is filled -- carries no such collision, and the filled
# top separates it from `tool_object_editor`, which is the same cube unfilled.
icon('view_mode_2d', 'noggit', 'VIEW_MODE_2D', 0xF8C7, 'view-mode', 'Top view',
     [F('M12,4 L18,8 L12,12 L6,8 Z'),
      S('M6,8 L6,16 L12,20 L18,16 L18,8'), S('M12,12 L12,20')])

icon('view_mode_game', 'noggit', 'VIEW_MODE_GAME', 0xF8D8, 'view-mode',
     'Game view', GAMEPAD)

icon('view_axis', 'noggit', 'VIEW_AXIS', 0xF8C8, 'view-mode', 'Axis gizmo',
     [S('M12,16 L12,5'), S('M12,16 L20,20'), S('M12,16 L4,20'),
      S(head((12, 5), (12, 16))), S(head((20, 20), (12, 16))),
      S(head((4, 20), (12, 16)))])

icon('time_normal', 'noggit', 'TIME_NORMAL', 0xF8BD, 'time', 'Normal time',
     [S('O 12 12 8'), S('M12,12 L12,7'), S('M12,12 L16,14'), F('O 12 12 1')])

icon('time_pause', 'noggit', 'TIME_PAUSE', 0xF8BE, 'time', 'Pause time',
     [S('O 12 12 8'), F('R 9 8 2 8 0.5'), F('R 13 8 2 8 0.5')])

icon('time_speed', 'noggit', 'TIME_SPEED', 0xF8BF, 'time', 'Speed up time',
     [S('O 12 12 8'), F('M8,8 L13,12 L8,16 Z'), F('M13,8 L18,12 L13,16 Z')])

icon('area_trigger_sphere', 'noggit', 'AREA_TRIGGER_SPHERE', 0xF8E1, 'gizmo',
     'Sphere trigger',
     [D('O 12 12 8', (4, 3)), S(ellipse(12, 12, 8, 3)), F('O 12 12 1.3')])

icon('visibility_particles', 'noggit', 'VISIBILITY_PARTICLES', 0xF8BC,
     'view-toolbar', 'Particles', _particles())

# ---- GROUP 5: panel affordances, dialogs, menus ------------------------------
# The pencil button on every ExtendedSlider is the *tablet* control -- it toggles
# tablet pressure and opens a sensitivity popup. Both drawings therefore have to
# say "stylus pressure", not "rename this": `edit` is the same nib writing onto
# a rule.
icon('pen', 'awesome', 'pen', 0xF304, 'panel', 'Tablet pressure off', PEN_BODY)
icon('edit', 'awesome', 'edit', 0xF044, 'panel', 'Tablet pressure on',
     PEN_BODY + [S('M4,21 L20,21')])

icon('eraser', 'awesome', 'eraser', 0xF12D, 'panel', 'Erase',
     [S('M6,13 L13,5 L19,10 L12,18 Z'), S('M9,15.5 L16,7.5'), S('M4,21 L21,21')])

icon('eye', 'awesome', 'eye', 0xF06E, 'panel', 'Visible', EYE)
icon('eyeslash', 'awesome', 'eyeslash', 0xF070, 'panel', 'Hidden',
     EYE + [ES('M4,20 L20,4', 6.0), S('M4,20 L20,4')])

icon('cog', 'awesome', 'cog', 0xF013, 'panel', 'Settings', _cog())
icon('wrench', 'awesome', 'wrench', 0xF0AD, 'panel', 'Tools', _wrench())

icon('save', 'awesome', 'save', 0xF0C7, 'panel', 'Save',
     [S('R 4 4 16 16 2'), F('R 8 4 8 5 0'), S('R 8 13 8 7 1')])

icon('file', 'awesome', 'file', 0xF15B, 'panel', 'File',
     [S('M6,4 L14,4 L18,8 L18,20 L6,20 Z'), S('M14,4 L14,8 L18,8')])

icon('filearchive', 'awesome', 'filearchive', 0xF1C6, 'panel', 'Archive',
     [S('M6,4 L14,4 L18,8 L18,20 L6,20 Z'), S('M14,4 L14,8 L18,8'),
      D('M10,7 L10,17', (2, 2))])

icon('folderopen', 'awesome', 'folderopen', 0xF07C, 'panel', 'Open folder',
     [S('M3,19 L3,6 L9,6 L11,9 L19,9 L19,12'),
      S('M3,19 L6,12 L21,12 L18,19 Z')])

icon('clipboard', 'awesome', 'clipboard', 0xF328, 'panel', 'Clipboard',
     [S('R 5 5 14 16 2'), S('R 9 3 6 5 1'),
      S('M8,12 L16,12'), S('M8,16 L16,16')])

icon('map', 'awesome', 'map', 0xF279, 'panel', 'Map',
     [S('M3,7 L9,5 L15,7 L21,5 L21,17 L15,19 L9,17 L3,19 Z'),
      S('M9,5 L9,17'), S('M15,7 L15,19')])

icon('bookmark', 'awesome', 'bookmark', 0xF02E, 'panel', 'Bookmark',
     [S('M7,3 L17,3 L17,21 L12,16 L7,21 Z')])

# The one deliberate on/off pair that has to survive without colour: the outline
# is "not favourited", the solid is "favourited".
icon('star', 'awesome', 'star', 0xF005, 'panel', 'Favourite (off)',
     [S(star(12, 12.5, 9, 4))])
icon('star_filled', 'awesome', 'star', 0xF005, 'panel', 'Favourite (on)',
     [F(star(12, 12.5, 9, 4))])

icon('plus', 'awesome', 'plus', 0xF067, 'panel', 'Add',
     [S('M12,5 L12,19'), S('M5,12 L19,12')])
icon('minus', 'awesome', 'minus', 0xF068, 'panel', 'Remove', [S('M5,12 L19,12')])
icon('times', 'awesome', 'times', 0xF00D, 'panel', 'Close', TIMES)
icon('check', 'awesome', 'check', 0xF00C, 'panel', 'Confirm',
     [S('M5,13 L10,18 L19,6')])

icon('play', 'awesome', 'play', 0xF04B, 'panel', 'Play',
     [F('M8,5 L19,12 L8,19 Z')])
icon('pause', 'awesome', 'pause', 0xF04C, 'panel', 'Pause',
     [F('R 8 5 3 14 1'), F('R 13 5 3 14 1')])
icon('stop', 'awesome', 'stop', 0xF04D, 'panel', 'Stop', [F('R 7 7 10 10 2')])

icon('undo', 'awesome', 'undo', 0xF0E2, 'panel', 'Undo', _undo(False))
icon('redo', 'awesome', 'redo', 0xF01E, 'panel', 'Redo', _undo(True))

icon('trash', 'awesome', 'trash', 0xF1F8, 'panel', 'Delete', TRASH)

icon('cloud', 'awesome', 'cloud', 0xF0C2, 'panel', 'Cloud',
     [S('M6.5,18 C3.5,18 2.5,14 5.5,12.8 C5.5,8.5 11,7 13,10.5 '
        'C16,8.5 20.5,11 20,14.5 C19.7,16.7 18.4,18 17,18 Z')])

icon('server', 'awesome', 'server', 0xF233, 'panel', 'Server',
     [S('R 3 4 18 5 2'), S('R 3 10 18 5 2'), S('R 3 16 18 5 2'),
      F('O 6.5 6.5 0.9'), F('O 6.5 12.5 0.9'), F('O 6.5 18.5 0.9')])

icon('networkwired', 'awesome', 'networkwired', 0xF6FF, 'panel', 'Connection',
     [S('R 9 3 6 5 1'), S('R 3 16 6 5 1'), S('R 15 16 6 5 1'),
      S('M12,8 L12,12'), S('M6,12 L18,12'),
      S('M6,12 L6,16'), S('M18,12 L18,16')])

icon('sun', 'awesome', 'sun', 0xF185, 'panel', 'Daylight', _sun())

icon('palette', 'awesome', 'palette', 0xF53F, 'panel', 'Palette',
     [S('M12,3 C18,3 21,7 21,12 C21,15 18,17 16,16.5 C14.5,16.2 14,17.5 14.5,18.5 '
        'C15,19.7 14,21 12,21 C7,21 3,17 3,12 C3,7 6.5,3 12,3 Z'),
      F('O 8 9 0.9'), F('O 12 7 0.9'), F('O 16 8 0.9'), F('O 18.5 12 0.9')])

icon('volumeup', 'awesome', 'volumeup', 0xF028, 'panel', 'Sound', _volume())

icon('running', 'awesome', 'running', 0xF70C, 'panel', 'Simulate',
     [F('O 15 5 2'), S('M14,8 L10,13'), S('M10,13 L13,19'), S('M10,13 L5,17'),
      S('M13,9 L18,11'), S('M13,9 L8,7')])

icon('caretdown', 'awesome', 'caretdown', 0xF0D7, 'panel', 'Expand',
     [F('M7,10 L17,10 L12,16 Z')])
icon('caretright', 'awesome', 'caretright', 0xF0DA, 'panel', 'Collapse',
     [F('M10,7 L16,12 L10,17 Z')])
icon('chevrondown', 'awesome', 'chevrondown', 0xF078, 'panel', 'Down',
     [S('M6,9 L12,16 L18,9')])
icon('chevronup', 'awesome', 'chevronup', 0xF077, 'panel', 'Up',
     [S('M6,15 L12,8 L18,15')])
icon('angledoubleleft', 'awesome', 'angledoubleleft', 0xF100, 'panel', 'Previous',
     [S('M12,6 L7,12 L12,18'), S('M19,6 L14,12 L19,18')])
icon('angledoubleright', 'awesome', 'angledoubleright', 0xF101, 'panel', 'Next',
     [S('M12,6 L17,12 L12,18'), S('M5,6 L10,12 L5,18')])

icon('windowminimize', 'awesome', 'windowminimize', 0xF2D1, 'panel', 'Minimise',
     [S('M5,17 L19,17')])
icon('windowmaximize', 'awesome', 'windowmaximize', 0xF2D0, 'panel', 'Maximise',
     [S('R 5 5 14 14 1'), F('R 5 5 14 4 1')])

# ---- GROUP 6: new affordances (no call site yet) ------------------------------
icon('reset', None, None, None, 'affordance', 'Reset',
     [S(arc(12, 12, 7, -60, 240)), S(arc_head(12, 12, 7, 240, True)),
      F('O 12 12 1.3')])

# Shortening the shackle's sweep by 30 degrees made these two pixel-identical at
# 96 px. The open state instead moves the whole shackle off-centre and lifts its
# free leg clear of the body, so the difference survives down to 16 px.
icon('lock', None, None, None, 'affordance', 'Locked',
     [S('R 4 11 16 10 2'), S(arc(12, 11, 4.5, 180, 360)),
      F('O 12 15 1.3'), S('M12,16 L12,19')])

icon('unlock', None, None, None, 'affordance', 'Unlocked',
     [S('R 4 11 16 10 2'), S(arc(16, 8, 4.5, 180, 350)), S('M11.5,8 L11.5,11'),
      F('O 12 15 1.3'), S('M12,16 L12,19')])

# ---- aliases: same drawing, second name --------------------------------------
# Two drawings of a waste bin, or two of a gamepad, in one family is noise.
ALIASES = [
    ('trashalt', 'awesome', 'trashalt', 0xF2ED, 'trash', 'Delete'),
    ('lightbulb', 'awesome', 'lightbulb', 0xF0EB, 'tool_light', 'Light'),
    ('gamepad', 'awesome', 'gamepad', 0xF11B, 'view_mode_game', 'Gamepad'),
    ('info_fa', 'awesome', 'info', 0xF129, 'info', 'Information'),
    ('windowclose', 'awesome', 'windowclose', 0xF410, 'times', 'Close window'),
    ('visibility_animation_2', 'noggit', 'VISIBILITY_ANIMATION_2', 0xF8B8,
     'visibility_animation', 'Animations'),
    ('show_minimap', 'noggit', 'TOOL_MINIMAP_EDITOR', 0xF8A5,
     'tool_minimap_editor', 'Show minimap'),
]


# ------------------------------------------------------------ contact sheet ---
PANEL = (0x1B, 0x1E, 0x24)      # bg.base
DIM = (0x8A, 0x93, 0xA0)        # text.dim -- the Normal-state icon colour


def contact_sheet(path, rendered, cols=12, cell=40, size=24):
    rows = (len(rendered) + cols - 1) // cols
    w, h = cols * cell, rows * cell
    buf = bytearray()
    for _ in range(w * h):
        buf += bytes((PANEL[0], PANEL[1], PANEL[2], 255))
    off = (cell - size) // 2
    for idx, cov in enumerate(rendered):
        ox = (idx % cols) * cell + off
        oy = (idx // cols) * cell + off
        for y in range(size):
            for x in range(size):
                a = _clamp01(cov[y * size + x])
                if a <= 0.0:
                    continue
                i = ((oy + y) * w + (ox + x)) * 4
                for c in range(3):
                    buf[i + c] = int(round(DIM[c] * a + buf[i + c] * (1.0 - a)))
    write_png(path, w, h, buf)
    return w, h


# ------------------------------------------------------------------- verify ---
def verify():
    """Measure crispness instead of asserting it.

    Every probe below crosses a 2-unit stroke (or a fill edge) whose centreline
    sits on an integer grid coordinate, sampling *perpendicular* to it. If the
    alignment is right the alpha profile is a clean 0,255,255,0 at 24 px -- two
    fully covered device rows and nothing partial. Any value strictly between 0
    and 255 means the centreline drifted onto a half pixel and the stroke will
    read as mud rather than as a line.

    'h' = horizontal stroke at grid y=`line`, sampled down column x=`through`.
    'v' = vertical stroke at grid x=`line`, sampled across row y=`through`.
    """
    by_slug = dict((e['slug'], e) for e in ICONS)
    probes = [
        ('tool_raise_lower', 'h', 15, 12, 'strip: the baseline'),
        ('visibility_lines', 'h', 12, 12, 'toolbar: centre rule'),
        ('visibility_lines', 'h', 7, 12, 'toolbar: upper rule'),
        ('visibility_fog', 'h', 11, 12, 'fog: second rule'),
        ('minus', 'h', 12, 12, 'panel: the bar'),
        ('windowminimize', 'h', 17, 12, 'chrome: the bar'),
        # 'through' has to miss every other stroke in the icon, or the profile
        # is measuring the crossing member instead of the one named.
        ('plus', 'v', 12, 7, 'panel: vertical bar'),
        ('tool_chunk', 'v', 9, 6, 'chunk grid: first vertical'),
        ('tool_chunk', 'h', 15, 6, 'chunk grid: last horizontal'),
        ('time_pause', 'v', 10, 12, 'a FILL edge pair (bar x=9..11)'),
        ('windowmaximize', 'h', 17, 12, 'rounded-rect edge, r=1'),
        ('visibility_wireframe', 'v', 4, 12, 'wireframe: left edge'),
        ('falloff_flat', 'v', 4, 12, 'falloff frame: the axis'),
        ('view_axis', 'v', 12, 10, 'axis gizmo: the up ray'),
    ]
    print('--- crispness probes: alpha 0-255 sampled ACROSS the stroke ---')
    ok = True
    for slug, kind, line, through, note in probes:
        for size in SIZES:
            cov = render(by_slug[slug]['subpaths'], size)
            k = size // 24
            lo, hi = (line - 3) * k, (line + 3) * k
            vals = []
            for i in range(lo, hi):
                c = cov[i * size + through * k] if kind == 'h' \
                    else cov[through * k * size + i]
                vals.append(int(round(_clamp01(c) * 255)))
            solid = sum(1 for v in vals if v == 255)
            partial = sum(1 for v in vals if 0 < v < 255)
            good = solid == 2 * k and partial == 0
            ok = ok and good
            print('%-4s %-22s %2dpx  %-46s  %s'
                  % ('OK' if good else 'SOFT', slug, size,
                     ','.join('%3d' % v for v in vals), note))
    print('--- every probe crisp: %s ---' % ok)
    return ok


# --------------------------------------------------------------------- main ---
def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    manifest = dict(version=1, grid=int(GRID), stroke_units=STROKE_UNITS,
                    sizes={'1x': SIZES[0], '2x': SIZES[1]},
                    generator='dist/noggit-themes/CrimsonSlate/icons/'
                              'generate_icon_set.py',
                    tint_note=('PNGs are white (#FFFFFF) straight-alpha masks. '
                               'Tint with QPainter::CompositionMode_SourceIn; '
                               'do not use them as-is except for a quick check.'),
                    path_syntax='absolute M/L/C/Q/Z plus O cx cy r and '
                                'R x y w h r; no relative commands, no arcs',
                    icons={}, by_codepoint={}, needs_enumerator=[])

    rendered = []
    for e in ICONS:
        slug = e['slug']
        with open(os.path.join(outdir, slug + '.svg'), 'w') as fh:
            fh.write(to_svg(slug, e['subpaths']))
        for size in SIZES:
            cov = render(e['subpaths'], size)
            suffix = '.png' if size == SIZES[0] else '@2x.png'
            write_png(os.path.join(outdir, slug + suffix), size, size,
                      cov_to_rgba(cov))
            if size == SIZES[0]:
                rendered.append(cov)
        manifest['icons'][slug] = dict(
            svg='%s.svg' % slug, png_1x='%s.png' % slug, png_2x='%s@2x.png' % slug,
            font=e['font'], enumerator=e['enum'],
            codepoint=('0x%04x' % e['cp']) if e['cp'] else None,
            group=e['group'], tooltip=e['tip'], alias_of=None,
            subpaths=[dict(d=s['d'], op=s['op'],
                           **({'dash': list(s['dash'])} if s.get('dash') else {}),
                           **({'width': s['w']} if s.get('w') else {}))
                      for s in e['subpaths']])
        if e['cp']:
            manifest['by_codepoint'].setdefault('0x%04x' % e['cp'], slug)
        if e['font'] and not e['enum']:
            manifest['needs_enumerator'].append(slug)

    for slug, font, enum, cp, target, tip in ALIASES:
        src = manifest['icons'][target]
        manifest['icons'][slug] = dict(
            svg=src['svg'], png_1x=src['png_1x'], png_2x=src['png_2x'],
            font=font, enumerator=enum, codepoint='0x%04x' % cp,
            group=src['group'], tooltip=tip, alias_of=target,
            subpaths=src['subpaths'])
        manifest['by_codepoint'].setdefault('0x%04x' % cp, target)

    w, h = contact_sheet(os.path.join(outdir, 'contact_sheet.png'), rendered)

    with open(os.path.join(outdir, 'manifest.json'), 'w') as fh:
        json.dump(manifest, fh, indent=2, sort_keys=True)
        fh.write('\n')

    print('drawings      : %d' % len(ICONS))
    print('aliases       : %d' % len(ALIASES))
    print('manifest keys : %d' % len(manifest['icons']))
    print('files         : %d svg + %d png (+ contact_sheet %dx%d, manifest)'
          % (len(ICONS), len(ICONS) * len(SIZES), w, h))
    print('no enumerator : %s' % ', '.join(manifest['needs_enumerator']))
    return manifest


if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else '.'
    if '--verify' in sys.argv:
        sys.exit(0 if verify() else 1)
    main(target)
