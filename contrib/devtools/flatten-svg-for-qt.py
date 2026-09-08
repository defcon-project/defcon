#!/usr/bin/env python3
"""Rewrite the logo so Qt's SVG renderer can draw it.

Qt 5's SVG module ignores `clip-path`. The artwork is built entirely out of
full-canvas rectangles clipped to five shapes, so under Qt every group paints
the whole 1024x1024 square and the mark disappears into a wash of gradient --
measured, not assumed: 100% canvas coverage and an opaque corner pixel.

The transformation here is exactly equivalent and needs no clipping: filling a
full-canvas rect and clipping it to shape S paints the same pixels as filling S
itself. So each `<rect .../>` inside a clipped group becomes a `<path d="...">`
carrying that group's shape, with the same fill and in the same order.

`<use>` is replaced by the path it references for the same reason -- one less
feature to depend on. Nothing about the geometry, the colours or the stacking
order changes.
"""
import re
import sys

source, target = sys.argv[1], sys.argv[2]
svg = open(source, encoding="utf-8").read()

# The shape definitions: <path id="shape-x" d="..."/>
shapes = dict(re.findall(r'<path id="(shape-[^"]+)" d="([^"]+)"/>', svg))
if not shapes:
    sys.exit("no shape definitions found -- the file is not shaped as expected")

# Which clip id maps to which shape: <clipPath id="clip-x"><use xlink:href="#shape-x"/></clipPath>
clip_to_shape = dict(re.findall(
    r'<clipPath id="([^"]+)"><use xlink:href="#(shape-[^"]+)"/></clipPath>', svg))

# Drop the clipPath definitions; nothing refers to them any more.
svg = re.sub(r'<clipPath id="[^"]+"><use xlink:href="#shape-[^"]+"/></clipPath>\n?', '', svg)

def flatten_group(match):
    head, clip_id, body = match.group(1), match.group(2), match.group(3)
    shape = clip_to_shape.get(clip_id)
    if shape is None:
        return match.group(0)
    d = shapes[shape]
    out = [head.replace(f' clip-path="url(#{clip_id})"', '')]
    for fill in re.findall(r'<rect width="1024" height="1024" fill="([^"]+)"/>', body):
        out.append(f'<path d="{d}" fill="{fill}"/>')
    out.append('</g>')
    return "\n".join(out)

svg, groups = re.subn(
    r'(<g id="[^"]+" clip-path="url\((#[^)]+)\)">)\n(.*?)</g>',
    lambda m: flatten_group(re.match(r'(<g id="[^"]+" clip-path="url\(#([^)]+)\)">)\n(.*?)</g>',
                                     m.group(0), re.S)),
    svg, flags=re.S)

# The outline group strokes the shapes through <use>; inline those too.
def inline_use(match):
    shape, rest = match.group(1), match.group(2)
    return f'<path d="{shapes[shape]}"{rest}/>'

svg, uses = re.subn(r'<use xlink:href="#(shape-[^"]+)"([^/]*)/>', inline_use, svg)

open(target, "w", encoding="utf-8").write(svg)
print(f"shapes: {len(shapes)}, clipped groups flattened: {groups}, <use> inlined: {uses}")
print(f"wrote {target}")
