#!/usr/bin/env bash
# The ML4W bar and dock pills made of particles (hyprmorph theme): hides the
# pill background (and its shadow) and puts a PixelField - a dense particle body
# in the theme's background colours, behind the content - into
# StatusbarWindow.qml and DockWindow.qml, plus a dark halo around text and
# icons. Idempotent; ML4W updates overwrite those files, so the installer's
# user post.sh re-runs this.
# Usage: patch-pixels.sh [path/to/StatusbarWindow.qml]
src="${PIXELS_SRC:-$(cd "$(dirname "$0")" && pwd)}" # PixelField.qml, dots.frag.qsb (built from dots.frag)
bar="${1:-$HOME/.config/quickshell/StatusbarApp/StatusbarWindow.qml}"
dock="$HOME/.local/share/ml4w-dock/DockApp/DockWindow.qml"

install_component() { # dir
    cp -f "$src/PixelField.qml" "$src/dots.frag.qsb" "$1/"
}

patch() { # file anchor block
    python3 -I - "$@" <<'PY'
import sys
path, anchor, block = sys.argv[1:4]
s = open(path).read()
if "PIXEL-FIELD-3" in s:
    print("patch-pixels: already patched", path); sys.exit(0)
if "PIXEL-FIELD" in s:
    print("patch-pixels: an older pixel patch is in", path, "- restore the original first", file=sys.stderr); sys.exit(1)
if s.count(anchor) != 1:
    print("patch-pixels: anchor not found in", path, "(upstream changed?)", file=sys.stderr); sys.exit(1)
open(path, "w").write(s.replace(anchor, block + anchor, 1))
print("patch-pixels: patched", path)
PY
}

# Only particles: hide the pill background and its shadow.
hide_pill() { # file
    python3 -I - "$1" <<'PY'
import sys
path = sys.argv[1]
s = open(path).read()
if "PIXEL-NO-PILL" in s:
    sys.exit(0)
a = """            id: pillBg
            anchors.fill: parent
"""
b = """        RectangularShadow {
            anchors.fill: pillBg
"""
if s.count(a) != 1 or s.count(b) != 1:
    print("patch-pixels: pill anchors not found in", path, file=sys.stderr); sys.exit(1)
s = s.replace(a, a + "            visible: false // PIXEL-NO-PILL: the particles are the background\n", 1)
s = s.replace(b, b + "            visible: false // PIXEL-NO-PILL\n", 1)
open(path, "w").write(s)
print("patch-pixels: pill hidden in", path)
PY
}

# A soft dark halo around text and icons.
halo() { # file id...
    python3 -I - "$@" <<'PY'
import sys
path, ids = sys.argv[1], sys.argv[2:]
s = open(path).read()
if "PIXEL-HALO" in s:
    sys.exit(0)
for i in ids:
    a = "            id: %s\n" % i
    if s.count(a) != 1:
        print("patch-pixels: halo anchor", i, "not found in", path, file=sys.stderr); sys.exit(1)
    s = s.replace(a, a + """            // PIXEL-HALO: a soft dark halo so text / icons stay crisp over the particles
            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: "black"
                shadowBlur: 0.55
                shadowOpacity: 0.9
                shadowHorizontalOffset: 0
                shadowVerticalOffset: 0
                blurMax: 20
            }
""", 1)
open(path, "w").write(s)
print("patch-pixels: halos in", path)
PY
}

# block COLOR_BG COLOR_A COLOR_B
block() {
    printf '%s\n' \
'        // PIXEL-FIELD-3 (patch-pixels.sh): the pill background made of particles, behind the content' \
'        HoverHandler { id: pixelHover }' \
'        PixelField {' \
'            id: pixelField' \
'            z: -1' \
'            anchors.fill: pillBg' \
'            radius: pillBg.radius' \
"            colorBg: $1" \
"            colorA: $2" \
"            colorB: $3" \
'            hovered: pixelHover.hovered' \
'            cursor: pixelHover.hovered ? pixelHover.point.position : Qt.point(-1000, -1000)' \
'        }' \
''
}

if [ -f "$bar" ]; then
    install_component "$(dirname "$bar")"
    patch "$bar" '        // ==========================================
        // LEFT AREA (only visible when expanded)' "$(block Theme.background Theme.primary Theme.tertiary)
        Connections {
            target: pill
            function onExpandedChanged() { if (pill.expanded) pixelField.burst(60) }
        }

" && hide_pill "$bar" && halo "$bar" leftArea centerArea rightArea
fi

if [ -f "$dock" ]; then
    install_component "$(dirname "$dock")"
    patch "$dock" '        RowLayout {
            id: dockRow' "$(block DockTheme.background DockTheme.primary DockTheme.on_surface_variant)

" && hide_pill "$dock" && halo "$dock" dockRow
fi
