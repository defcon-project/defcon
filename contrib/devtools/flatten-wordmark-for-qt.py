#!/usr/bin/env python3
"""Make the DeFCoN wordmark drawable by Qt.

Qt 5's SVG renderer ignores `clip-path`, and this drawing puts four highlight
layers inside one clipped group, so under Qt they paint the whole 616x140 box
and the lettering drowns in a wash of light blue. Measured: 88% of the canvas
covered, corner pixel not transparent.

Three of the four layers convert exactly. Filling the letters with a paint
gives the same pixels as filling the whole box with it and clipping to the
letters, so:

    <rect .../> clipped to the letters   ->   <use href="#wordmark-shape" fill="..."/>

The fourth is not an equivalence and is called out here rather than hidden: the
sheen is a wavy band across the upper third, and band-intersect-letters cannot
be written without clipping. It is painted with `surface-light`, a vertical
gradient that runs 4% -> 16% -> 0% opacity over y 0..64, and the band spans
roughly y 4..78 across the full width -- so filling the letters with that same
gradient differs only along the band's curved edge, at no more than 16% alpha.

The inner edge stroke keeps its width but is no longer clipped to the letters,
so it now straddles the outline instead of sitting inside it: about 0.6 px of
soft edge on the outside, which at the size the navigation draws this is a
fraction of a pixel.
"""
import re
import sys

source, target = sys.argv[1], sys.argv[2]
svg = open(source, encoding="utf-8").read()

# Everything the clipped group painted, in order, now painted on the shape.
replacement = """<use xlink:href="#wordmark-shape" fill="url(#top-light)"/>
<use xlink:href="#wordmark-shape" fill="url(#right-light)"/>
<use xlink:href="#wordmark-shape" fill="url(#surface-light)"/>
<use xlink:href="#wordmark-shape" fill="none" stroke="url(#edge-light)" stroke-width="1.25" stroke-linejoin="round"/>"""

svg, groups = re.subn(
    r'<g clip-path="url\(#wordmark-clip\)">.*?</g>',
    replacement, svg, flags=re.S)
if groups != 1:
    sys.exit(f"expected exactly one clipped group, found {groups} -- the file is not shaped as expected")

# The clip path is unreferenced now.
svg, clips = re.subn(r'<clipPath id="wordmark-clip">.*?</clipPath>\n?', '', svg, flags=re.S)

if "clip-path" in svg:
    sys.exit("a clip-path survived the rewrite")

open(target, "w", encoding="utf-8").write(svg)
print(f"clipped groups rewritten: {groups}, clipPath definitions removed: {clips}")
print(f"wrote {target}")
