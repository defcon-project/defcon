#!/usr/bin/env python3
"""Make a DeFCoN wordmark drawable by Qt, whatever its ids are called.

Qt 5's SVG renderer ignores `clip-path`, and these drawings put their highlight
layers inside one clipped group -- so Qt paints them across the whole box and
the lettering drowns in a wash. The rewrite is the same equivalence every time:
filling the letters paints the same pixels as filling the box and clipping it
to the letters.

    <rect .../>  inside the clipped group   ->   <use href="#shape" fill="..."/>
    <path .../>  inside the clipped group   ->   <use href="#shape" fill="..."/>

The second line is an approximation and the reason this prints what it did: a
path in there is a shape intersected with the letters, which cannot be written
without clipping. In these drawings it is a sheen band whose own gradient never
exceeds 16% opacity, so applying that gradient to the letters differs only along
the band's edge. Anything else in the group -- the edge stroke -- is kept as it
is, and loses only its clipping, so it straddles the outline instead of sitting
inside it.

Ids are discovered rather than assumed, so a differently-prefixed file works.
"""
import re
import sys

source, target = sys.argv[1], sys.argv[2]
svg = open(source, encoding="utf-8").read()

clipped = re.search(r'<g clip-path="url\(#([^)]+)\)"\s*>(.*?)</g>', svg, re.S)
if not clipped:
    sys.exit("no clipped group found -- nothing to rewrite, or the file is shaped differently")
clip_id, body = clipped.group(1), clipped.group(2)

# The shape the highlights are meant to be confined to: whatever the group's
# own <use> elements refer to. Taking it from the file rather than from the
# clipPath's copy of the outlines keeps this to one source of truth.
shape = re.search(r'<use\s+xlink:href="#([^"]+)"', body) or re.search(r'<use\s+xlink:href="#([^"]+)"', svg)
if not shape:
    sys.exit("could not find the shape the wordmark is drawn from")
shape_id = shape.group(1)

rewritten, kept, approximated = [], 0, 0
for element in re.findall(r'<(?:rect|path|use)\b[^>]*/>', body):
    fill = re.search(r'fill="(url\(#[^)]+\)|#[0-9a-fA-F]+)"', element)
    if element.startswith("<rect"):
        rewritten.append(f'<use xlink:href="#{shape_id}" fill="{fill.group(1)}"/>')
    elif element.startswith("<path"):
        rewritten.append(f'<use xlink:href="#{shape_id}" fill="{fill.group(1)}"/>')
        approximated += 1
    else:
        rewritten.append(element.strip())
        kept += 1

svg = svg[:clipped.start()] + "\n".join(rewritten) + svg[clipped.end():]
svg, clips = re.subn(rf'<clipPath id="{re.escape(clip_id)}">.*?</clipPath>\n?', '', svg, flags=re.S)

if "clip-path" in svg:
    sys.exit("a clip-path survived the rewrite")

open(target, "w", encoding="utf-8").write(svg)
print(f"shape: #{shape_id}, clip: #{clip_id}")
print(f"  layers rewritten exactly: {len(rewritten) - approximated - kept}")
print(f"  layers approximated (a clipped path): {approximated}")
print(f"  layers kept as they were (unclipped now): {kept}")
print(f"wrote {target}")
