#!/usr/bin/env python3
#
#  SPDX-License-Identifier: GPL-3.0-or-later
#
"""Builds the Comic branding icons: the default Krita icon with the
speech-bubble badge of krita-speech-bubble.svg on top.

Run from this folder: python generate_icons.py   (needs Pillow)

Writes <size>-apps-krita.png, krita.ico and sc-apps-krita.svgz. The badge
uses only M, L, H, V, A (circular) and Z path commands, which is all this
script draws; a badge with curves needs a real SVG renderer instead.
"""
import gzip
import math
import os
import re
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(HERE, '..', 'default')
SIZES = [16, 22, 24, 32, 48, 64, 128, 256, 512, 1024]
SS = 4  # supersampling of the 1024 px master


def arc(p0, p1, r, sweep):
    """Points of the small circular arc from p0 to p1 (p0 excluded)."""
    mx, my = (p0[0] + p1[0]) / 2, (p0[1] + p1[1]) / 2
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    d = math.hypot(dx, dy)
    r = max(r, d / 2)
    h = math.sqrt(max(0.0, r * r - d * d / 4))
    for sign in (1, -1):
        cx, cy = mx - sign * h * dy / d, my + sign * h * dx / d
        cross = (p0[0] - cx) * (p1[1] - cy) - (p0[1] - cy) * (p1[0] - cx)
        if (cross > 0) == bool(sweep):
            break
    a0 = math.atan2(p0[1] - cy, p0[0] - cx)
    a1 = math.atan2(p1[1] - cy, p1[0] - cx)
    if sweep and a1 < a0:
        a1 += 2 * math.pi
    if not sweep and a1 > a0:
        a1 -= 2 * math.pi
    n = 24
    return [(cx + r * math.cos(a0 + (a1 - a0) * i / n), cy + r * math.sin(a0 + (a1 - a0) * i / n)) for i in range(1, n + 1)]


def subpaths(d):
    """The closed subpaths of path data \\p d, as lists of points."""
    tokens = re.findall(r'[MLHVAZ]|-?\d+(?:\.\d+)?', d)
    out, cur, pos, i = [], [], (0.0, 0.0), 0

    def num():
        nonlocal i
        i += 1
        return float(tokens[i - 1])

    while i < len(tokens):
        c = tokens[i]
        i += 1
        if c == 'M':
            pos = (num(), num())
            cur = [pos]
        elif c == 'L':
            pos = (num(), num())
            cur.append(pos)
        elif c == 'H':
            pos = (num(), pos[1])
            cur.append(pos)
        elif c == 'V':
            pos = (pos[0], num())
            cur.append(pos)
        elif c == 'A':
            rx, _ry, _rot, _large, sweep = num(), num(), num(), num(), num()
            end = (num(), num())
            cur.extend(arc(pos, end, rx, sweep))
            pos = end
        elif c == 'Z':
            out.append(cur)
            cur = []
        else:
            raise ValueError('unsupported path token ' + c)
    return out


def badge_master(svg):
    """The badge on a transparent 1024 px square."""
    size = 1024 * SS
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    for m in re.finditer(r'<path\b[^>]*>', svg):
        tag = m.group(0)
        fill = re.search(r'fill:(#[0-9a-fA-F]{6})', tag).group(1)
        stroke = re.search(r'stroke:(#[0-9a-fA-F]{6})', tag)
        width = re.search(r'stroke-width:([\d.]+)', tag)
        paths = subpaths(re.search(r'\bd="([^"]+)"', tag).group(1))
        # The badge's second subpath is the bubble-shaped hole (evenodd);
        # the bubble path itself fills it, so only the outline is drawn here.
        for pts in paths[:1]:
            scaled = [(x * SS, y * SS) for x, y in pts]
            draw.polygon(scaled, fill=fill)
            if stroke and width:
                draw.line(scaled + scaled[:2], fill=stroke.group(1), width=round(float(width.group(1)) * SS), joint='curve')
    return img.resize((1024, 1024), Image.LANCZOS)


def main():
    svg = open(os.path.join(HERE, 'krita-speech-bubble.svg'), encoding='utf-8').read()
    paths = re.findall(r'<path\b[^>]*>', svg)
    # draw the dark badge first, the white bubble on top of it
    ordered = ''.join(sorted(paths, key=lambda t: 'id="badge"' not in t))
    badge = badge_master(ordered)

    icons = {}
    for s in SIZES:
        base = Image.open(os.path.join(DEFAULT, '%d-apps-krita.png' % s)).convert('RGBA')
        small = badge if s == 1024 else badge.resize((s, s), Image.LANCZOS)
        icons[s] = Image.alpha_composite(base, small)
        icons[s].save(os.path.join(HERE, '%d-apps-krita.png' % s), optimize=True)

    icons[256].save(os.path.join(HERE, 'krita.ico'), sizes=[(n, n) for n in (256, 128, 64, 48, 32, 24, 16)])

    base_svg = gzip.open(os.path.join(DEFAULT, 'sc-apps-krita.svgz')).read().decode('utf-8')
    end = base_svg.rindex('</svg>')
    merged = base_svg[:end] + '  ' + '\n  '.join(sorted(paths, key=lambda t: 'id="badge"' not in t)) + '\n' + base_svg[end:]
    with open(os.path.join(HERE, 'sc-apps-krita.svgz'), 'wb') as raw:
        # mtime=0 and no file name: the same input gives the same bytes
        with gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0) as f:
            f.write(merged.encode('utf-8'))


if __name__ == '__main__':
    main()
