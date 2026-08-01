#!/usr/bin/env python3
# This file is part of Noggit3, licensed under GNU General Public License (version 3).
"""Static checker for a Qt 5.15 style sheet.

Qt silently ignores properties, pseudo-states and sub-controls it does not
understand, so an invalid one is an invisible failure -- the sheet still loads,
the app still starts, and the rule simply never fires. This walks the sheet and
compares every identifier against the lists in the Qt 5.15 "Qt Style Sheets
Reference" (List of Properties / List of Pseudo-States / List of Sub-Controls),
checks colour literals and rgba() arity, resolves every url() against the
filesystem, and reports WCAG contrast for the palette.

Usage:  python check_qss.py theme.qss .
        (second argument is the directory url() paths resolve against; for this
         theme that is the parent of themes/, i.e. the build output directory,
         but "." works because the checker also tries the theme's own folder)

Pure stdlib. No Qt, no network.

EXTENDED beyond the first revision:
  * the palette is PARSED OUT OF THE SHEET'S OWN COMMENT HEADER rather than
    hardcoded here, so the contrast table can never drift from the tokens
  * rgba()/rgb() argument counts and ranges are validated
  * every hex literal in the body is checked for membership of the declared
    palette, which catches a stray colour that was never in the token table
  * a --coverage MODE diffs the selector set against another sheet, so a
    rewrite can prove it did not drop a rule
"""
import io
import os
import re
import sys

# --- Qt 5.15 "List of Properties" ------------------------------------------
PROPS = set("""
alternate-background-color background background-color background-image
background-repeat background-position background-attachment background-clip
background-origin border border-top border-right border-bottom border-left
border-color border-top-color border-right-color border-bottom-color
border-left-color border-image border-radius border-top-left-radius
border-top-right-radius border-bottom-right-radius border-bottom-left-radius
border-style border-top-style border-right-style border-bottom-style
border-left-style border-width border-top-width border-right-width
border-bottom-width border-left-width bottom button-layout color
dialogbuttonbox-buttons-have-icons font font-family font-size font-style
font-weight gridline-color height icon icon-size image image-position left
lineedit-password-character lineedit-password-mask-delay margin margin-top
margin-right margin-bottom margin-left max-height max-width
messagebox-text-interaction-flags min-height min-width opacity outline
outline-color outline-offset outline-style outline-radius
outline-bottom-left-radius outline-bottom-right-radius outline-top-left-radius
outline-top-right-radius padding padding-top padding-right padding-bottom
padding-left paint-alternating-row-colors-for-empty-area position right
selection-background-color selection-color show-decoration-selected spacing
subcontrol-origin subcontrol-position titlebar-show-tooltips-on-buttons
text-align text-decoration top width widget-animation-duration
titlebar-close-icon titlebar-normal-icon
-qt-background-role -qt-style-features
""".split())

# --- Qt 5.15 "List of Pseudo-States" ---------------------------------------
PSEUDO = set("""
active adjoins-item alternate bottom checked closable closed default disabled
editable edit-focus enabled exclusive first flat floatable focus has-children
has-siblings horizontal hover indeterminate last left maximized middle
minimized movable no-frame non-exclusive off on only-one open next-selected
pressed previous-selected read-only right selected top unchecked vertical
window
""".split())

# --- Qt 5.15 "List of Sub-Controls" ----------------------------------------
SUBCONTROLS = set("""
add-line add-page branch chunk close-button corner down-arrow down-button
drop-down float-button groove indicator handle icon item left-arrow
left-corner menu-arrow menu-button menu-indicator right-arrow pane right-corner
scroller section separator sub-line sub-page tab tab-bar tear tearoff text
title up-arrow up-button
""".split())

NAMED_COLOURS = set("""
transparent none black white red green blue cyan magenta yellow gray grey
darkgray darkgrey lightgray lightgrey darkred darkgreen darkblue darkcyan
darkmagenta darkyellow orange
""".split())


def strip_comments(text):
    return re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'),
                  text, flags=re.S)


def parse_palette(src):
    """Pull the token table out of the sheet's own comment header.

    Recognises lines of the shape
        *   token.name       #RRGGBB   optional trailing prose
    between the "PALETTE." and "METRICS." banners of the leading /* ... */
    block, so the contrast report below always describes the colours this file
    actually declares. Bounded to that range on purpose: the header also lists
    3D-viewport colours that are NOT reachable from QSS, and those must not be
    mistaken for chrome tokens.
    """
    head = src.split('*/', 1)[0] if src.lstrip().startswith('/*') else ''
    lines = head.splitlines()
    start = next((i for i, l in enumerate(lines) if 'PALETTE.' in l), None)
    if start is None:
        return []
    end = next((i for i, l in enumerate(lines[start:], start)
                if 'METRICS.' in l), len(lines))
    out = []
    for line in lines[start:end]:
        m = re.match(r'\s*\*\s+([a-z][a-z.+]*)\s+(#[0-9A-Fa-f]{6})(?=\s|$)',
                     line)
        if m:
            out.append((m.group(1), m.group(2).upper()))
    return out


def lin(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def lum(h):
    r, g, b = int(h[1:3], 16), int(h[3:5], 16), int(h[5:7], 16)
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def ratio(a, b):
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def selectors_of(path):
    body = strip_comments(io.open(path, encoding='utf-8').read())
    out = set()
    for sel, _ in re.findall(r'([^{}]+)\{([^{}]*)\}', body):
        for one in sel.split(','):
            one = ' '.join(one.split())
            if one:
                out.add(one)
    return out


def main(path, image_root):
    src = io.open(path, encoding='utf-8').read()
    body = strip_comments(src)

    errors, warnings = [], []
    palette = parse_palette(src)
    declared = set(h for _, h in palette)

    if not palette:
        warnings.append('no palette table found in the header comment; the '
                        'contrast section below is empty')

    if body.count('{') != body.count('}'):
        errors.append('unbalanced braces: %d "{" vs %d "}"'
                      % (body.count('{'), body.count('}')))

    rules = re.findall(r'([^{}]+)\{([^{}]*)\}', body)
    if not rules:
        errors.append('no rules parsed at all')

    consumed = sum(len(a) + len(b) + 2 for a, b in rules)
    leftover = len(body) - consumed
    if leftover > body.count('\n') + 40:
        warnings.append('%d characters sit outside any rule block' % leftover)

    line_of = {}
    for m in re.finditer(r'([^{}]+)\{', body):
        line_of[' '.join(m.group(1).split())] = body[:m.start()].count('\n') + 1

    seen_props, seen_pseudo, seen_sub, urls = set(), set(), set(), []
    seen_hex, decl_count, rgba_count = set(), 0, 0

    for selector, decls in rules:
        sel = ' '.join(selector.split())
        ln = line_of.get(sel, 0)

        for sub in re.findall(r'::([a-zA-Z-]+)', selector):
            seen_sub.add(sub)
            if sub not in SUBCONTROLS:
                errors.append('line %d: unknown sub-control "::%s" in %r'
                              % (ln, sub, sel))
        for ps in re.findall(r'(?<!:):(?!:)!?([a-zA-Z-]+)', selector):
            seen_pseudo.add(ps)
            if ps not in PSEUDO:
                errors.append('line %d: unknown pseudo-state ":%s" in %r'
                              % (ln, ps, sel))

        for raw in decls.split(';'):
            if not raw.strip():
                continue
            if ':' not in raw:
                errors.append('line %d: declaration without ":" -> %r'
                              % (ln, raw.strip()))
                continue
            decl_count += 1
            name, value = raw.split(':', 1)
            name, value = name.strip(), value.strip()
            seen_props.add(name)
            if not name.startswith('qproperty-') and name not in PROPS:
                errors.append('line %d: unknown property %r in %r'
                              % (ln, name, sel))

            for u in re.findall(r'url\(([^)]*)\)', value):
                urls.append((ln, u.strip().strip('"\'')))

            # rgb()/rgba() arity and range
            for fn, args in re.findall(r'\b(rgba?)\(([^)]*)\)', value):
                rgba_count += 1
                parts = [a.strip() for a in args.split(',')]
                want = 4 if fn == 'rgba' else 3
                if len(parts) != want:
                    errors.append('line %d: %s() takes %d arguments, got %d '
                                  '-> %r' % (ln, fn, want, len(parts), value))
                    continue
                for i, p in enumerate(parts[:3]):
                    if not re.fullmatch(r'\d+%?', p) or (
                            not p.endswith('%') and int(p) > 255):
                        errors.append('line %d: %s() channel %d is %r, not a '
                                      '0-255 number' % (ln, fn, i + 1, p))
                if want == 4:
                    a = parts[3]
                    ok = (re.fullmatch(r'\d+%', a)
                          or re.fullmatch(r'\d+', a) and int(a) <= 255
                          or re.fullmatch(r'0?\.\d+|[01](\.0+)?', a))
                    if not ok:
                        errors.append('line %d: rgba() alpha %r is not a '
                                      '0-1 float, 0-255 int or percentage'
                                      % (ln, a))

            for col in re.findall(r'#[0-9a-fA-F]*', value):
                if len(col) - 1 not in (3, 6, 8, 9, 12):
                    errors.append('line %d: bad colour literal %r' % (ln, col))
                if len(col) - 1 == 8:
                    warnings.append(
                        'line %d: %s is 8-digit #AARRGGBB -- Qt reads the '
                        'first pair as ALPHA, which is almost never what was '
                        'meant' % (ln, col))
                if len(col) - 1 == 6:
                    seen_hex.add(col.upper())

        tail = decls.rstrip()
        if tail and not tail.endswith(';'):
            errors.append('line %d: last declaration in %r has no ";"'
                          % (ln, sel))

    # ---- every colour used must be one the header declared -----------------
    if declared:
        stray = sorted(seen_hex - declared)
        if stray:
            warnings.append('colours used but not in the header palette: %s'
                            % ', '.join(stray))
        unused = sorted(declared - seen_hex)
        if unused:
            warnings.append('palette tokens declared but never used: %s'
                            % ', '.join(unused))

    # ---- url() resolution --------------------------------------------------
    referenced = set()
    for ln, u in urls:
        referenced.add(os.path.basename(u))
        candidates = [os.path.join(image_root, u),
                      os.path.join(os.path.dirname(os.path.abspath(path)),
                                   os.path.basename(os.path.dirname(u)),
                                   os.path.basename(u))]
        if not any(os.path.isfile(c) for c in candidates):
            errors.append('line %d: url(%s) resolves to no file (tried %s)'
                          % (ln, u, ' and '.join(candidates)))

    img_dir = os.path.join(os.path.dirname(os.path.abspath(path)), 'images')
    on_disk = set(os.listdir(img_dir)) if os.path.isdir(img_dir) else set()
    orphans = sorted(on_disk - referenced)

    print('=' * 74)
    print('QSS STATIC CHECK  --  %s' % path)
    print('=' * 74)
    print('rules parsed          : %d' % len(rules))
    print('declarations parsed   : %d' % decl_count)
    print('distinct properties   : %d' % len(seen_props))
    print('distinct pseudo-states: %d  %s' % (len(seen_pseudo),
                                              ' '.join(sorted(seen_pseudo))))
    print('distinct sub-controls : %d  %s' % (len(seen_sub),
                                              ' '.join(sorted(seen_sub))))
    print('rgb()/rgba() calls    : %d' % rgba_count)
    print('url() references      : %d  (%d distinct files)'
          % (len(urls), len(referenced)))
    print('images on disk        : %d' % len(on_disk))
    print('palette tokens parsed : %d' % len(palette))
    print()

    if palette:
        base = dict(palette).get('bg.base')
        if base:
            print('-- contrast against bg.base %s (WCAG 2.1) --' % base)
            for tok, hexv in palette:
                if tok.startswith('bg.'):
                    continue
                r = ratio(hexv, base)
                verdict = ('AA body' if r >= 4.5 else
                           ('AA large' if r >= 3.0 else 'sub-AA'))
                print('   %-13s %s  %5.2f:1  %s' % (tok, hexv, r, verdict))
            print()

        accent = dict(palette).get('accent')
        sunken = dict(palette).get('bg.sunken')
        hi = dict(palette).get('text.hi')
        if accent and sunken and hi:
            print('-- ink on a filled accent --')
            print('   %-13s %s  %5.2f:1  (the sheet uses this one)'
                  % ('bg.sunken', sunken, ratio(sunken, accent)))
            print('   %-13s %s  %5.2f:1  (rejected)'
                  % ('text.hi', hi, ratio(hi, accent)))
            print()

        print('-- adjacent surface separation --')
        surfaces = [(t, h) for t, h in palette if t.startswith('bg.')]
        for i in range(len(surfaces) - 1):
            (an, a), (bn, b) = surfaces[i], surfaces[i + 1]
            print('   %-11s -> %-11s %5.3f' % (an, bn, ratio(a, b)))
        print()

    if orphans:
        warnings.append('images present but never referenced: %s'
                        % ', '.join(orphans))

    for w in warnings:
        print('WARN  %s' % w)
    for e in errors:
        print('FAIL  %s' % e)

    print()
    print('RESULT: %d error(s), %d warning(s)' % (len(errors), len(warnings)))
    return 1 if errors else 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--coverage':
        old, new = selectors_of(sys.argv[2]), selectors_of(sys.argv[3])
        dropped = sorted(old - new)
        added = sorted(new - old)
        print('SELECTOR COVERAGE  %s -> %s' % (sys.argv[2], sys.argv[3]))
        print('  old selectors : %d' % len(old))
        print('  new selectors : %d' % len(new))
        print('  dropped       : %d' % len(dropped))
        for s in dropped:
            print('      MISSING  %s' % s)
        print('  added         : %d' % len(added))
        for s in added:
            print('      NEW      %s' % s)
        sys.exit(1 if dropped else 0)
    sys.exit(main(sys.argv[1], sys.argv[2]))
