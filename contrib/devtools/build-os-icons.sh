#!/usr/bin/env bash
# Regenerate the operating-system icon files from the logo SVG.
#
# Windows and macOS read these containers themselves rather than through Qt, and
# both are raster formats -- this is the one place the artwork cannot stay
# vector. Generating them from the same SVG is what stops them drifting into a
# second, older drawing, which is how a wallet ends up with one logo in the
# window and a different one on the executable.
#
# The rasterising step needs a renderer that writes 8-bit RGBA PNGs at an exact
# size and honours Qt's reading of the file. Pass one as the first argument;
# make-os-icons.py documents what it has to accept.
#
#   contrib/devtools/build-os-icons.sh [path/to/renderer]
set -eu

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
RENDER="${1:-svgrender}"
SVG="$REPO/src/qt/res/images/defcon_logo.svg"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

export QT_QPA_PLATFORM=offscreen

# NetworkStyle's own transform for test networks: hue +190, saturation -20.
# Kept in step with src/qt/networkstyle.cpp so the file icon matches the running
# application rather than approximating it.
HUE=190
SAT=20

for size in 16 24 32 48 64 128 256 512 1024; do
    "$RENDER" "$SVG" "$WORK/main-$size.png" "$size" >/dev/null
done
for size in 16 24 32 48 64 128 256; do
    "$RENDER" "$SVG" "$WORK/test-$size.png" "$size" "$HUE" "$SAT" >/dev/null
done

MAKE_ICONS="$REPO/contrib/devtools/make-os-icons.py"

python3 "$MAKE_ICONS" ico "$REPO/src/qt/res/icons/dash.ico" \
    "$WORK"/main-16.png "$WORK"/main-24.png "$WORK"/main-32.png \
    "$WORK"/main-48.png "$WORK"/main-64.png "$WORK"/main-128.png "$WORK"/main-256.png

python3 "$MAKE_ICONS" ico "$REPO/src/qt/res/icons/dash_testnet.ico" \
    "$WORK"/test-16.png "$WORK"/test-24.png "$WORK"/test-32.png \
    "$WORK"/test-48.png "$WORK"/test-64.png "$WORK"/test-128.png "$WORK"/test-256.png

python3 "$MAKE_ICONS" icns "$REPO/src/qt/res/icons/dash.icns" \
    "$WORK"/main-32.png "$WORK"/main-64.png "$WORK"/main-128.png \
    "$WORK"/main-256.png "$WORK"/main-512.png "$WORK"/main-1024.png

# The installer reads its own copy.
cp "$REPO/src/qt/res/icons/dash.ico" "$REPO/share/pixmaps/dash.ico"
# The Linux desktop icon is one that is allowed to stay vector.
cp "$SVG" "$REPO/share/pixmaps/dash-hicolor-scalable.svg"

echo
file "$REPO/src/qt/res/icons/dash.ico" "$REPO/src/qt/res/icons/dash_testnet.ico" \
     "$REPO/src/qt/res/icons/dash.icns"
