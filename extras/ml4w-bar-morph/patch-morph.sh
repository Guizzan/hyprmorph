#!/usr/bin/env bash
# The ML4W bar and dock show / hide as a pixel morph (hyprmorph theme): their
# pill is cut into cells that fly in from / out to the screen edge instead of
# sliding (PixelMorph.qml + morph.frag). Idempotent; ML4W updates overwrite
# StatusbarWindow.qml / DockWindow.qml, so the installer's user post.sh re-runs
# this. Usage: patch-morph.sh [path/to/StatusbarWindow.qml]
src="${MORPH_SRC:-$(cd "$(dirname "$0")" && pwd)}" # PixelMorph.qml, morph.frag.qsb (from morph.frag)
bar="${1:-$HOME/.config/quickshell/StatusbarApp/StatusbarWindow.qml}"
dock="$HOME/.local/share/ml4w-dock/DockApp/DockWindow.qml"

patch() { # file offsetProperty dir travel
    cp -f "$src/PixelMorph.qml" "$src/morph.frag.qsb" "$(dirname "$1")/"
    python3 -I - "$@" <<'PY'
import sys
path, prop, d, travel = sys.argv[1:5]
s = open(path).read()
if "PIXEL-MORPH" in s:
    print("patch-morph: already patched", path); sys.exit(0)
anchor = "    Item {\n        id: pill\n"
bind = "        property real %s: root.revealed\n" % prop
beh = "        Behavior on %s {\n" % prop
for a in (anchor, bind, beh):
    if s.count(a) != 1:
        print("patch-morph: anchor not found in", path, repr(a[:40]), file=sys.stderr); sys.exit(1)
block = """    // PIXEL-MORPH (patch-morph.sh): shows / hides as a pixel morph instead of sliding
    PixelMorph {
        id: pixelMorph
        source: pill
        shown: root.revealed
        dir: %s
        travel: %s
    }

""" % (d, travel)
s = s.replace(anchor, block + anchor, 1)
# the pill stays in place while its cells are on screen, and does not slide
s = s.replace(bind, "        property real %s: (root.revealed || (pixelMorph.morphOn && pixelMorph.t > 0)) // PIXEL-MORPH\n" % prop, 1)
s = s.replace(beh, beh + "            enabled: !pixelMorph.morphOn // PIXEL-MORPH\n", 1)
open(path, "w").write(s)
print("patch-morph: patched", path)
PY
}

[ -f "$bar" ] && patch "$bar" revealShift -1 110
[ -f "$dock" ] && patch "$dock" revealOffset 1 150
