#!/usr/bin/env bash
# The ML4W bar and dock pills drawn in pixels (hyprmorph theme): hides the pill
# background and puts a PixelField (pixel outline + dust, behind the content)
# and a PixelTrail (pixels flying off in the direction of each hide / show
# slide) into StatusbarWindow.qml and DockWindow.qml. Idempotent; ML4W updates
# overwrite those files, so the installer's user post.sh re-runs this.
# Usage: patch-pixels.sh [path/to/StatusbarWindow.qml]
src="${PIXELS_SRC:-$(cd "$(dirname "$0")" && pwd)}" # PixelField.qml, PixelTrail.qml, pixel.png
bar="${1:-$HOME/.config/quickshell/StatusbarApp/StatusbarWindow.qml}"
dock="$HOME/.local/share/ml4w-dock/DockApp/DockWindow.qml"

install_component() { # dir
    cp -f "$src/PixelField.qml" "$src/PixelTrail.qml" "$src/pixel.png" "$1/"
}

patch() { # file anchor block
    python3 -I - "$@" <<'PY'
import sys
path, anchor, block = sys.argv[1:4]
s = open(path).read()
if "PIXEL-FIELD-2" in s:
    print("patch-pixels: already patched", path); sys.exit(0)
if "PIXEL-FIELD" in s:
    print("patch-pixels: an older pixel patch is in", path, "- restore the original first", file=sys.stderr); sys.exit(1)
if s.count(anchor) != 1:
    print("patch-pixels: anchor not found in", path, "(upstream changed?)", file=sys.stderr); sys.exit(1)
open(path, "w").write(s.replace(anchor, block + anchor, 1))
print("patch-pixels: patched", path)
PY
}

# Only pixels: hide the pill background and its shadow.
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
s = s.replace(a, a + "            visible: false // PIXEL-NO-PILL: only pixels\n", 1)
s = s.replace(b, b + "            visible: false // PIXEL-NO-PILL\n", 1)
open(path, "w").write(s)
print("patch-pixels: pill hidden in", path)
PY
}

# block COLOR_A COLOR_B HIDE_DIR SHOW_DIR EXPAND_HOOK
block() {
cat <<EOF
        // PIXEL-FIELD-2 (patch-pixels.sh): the pill drawn in pixels, behind the content
        HoverHandler { id: pixelHover }
        PixelField {
            id: pixelField
            z: -1
            anchors.fill: pillBg
            radius: pillBg.radius
            colorA: $1
            colorB: $2
            hovered: pixelHover.hovered
            cursor: pixelHover.hovered ? pixelHover.point.position : Qt.point(-1000, -1000)
        }
        // pixels flying off in the direction of each slide, free over the whole window
        PixelTrail {
            id: pixelTrail
            parent: pill.parent
            // a 1x1 item: its pixels fly over the whole window, but it is never
            // under the pointer, so the bar / dock hover (autohide) is untouched
            x: 0; y: 0; width: 1; height: 1
            z: 100
            enabled: false
            colorA: $1
            colorB: $2
            active: pixelField.enabledSetting
        }
        Timer {
            id: pixelShow
            interval: 160
            onTriggered: pixelTrail.fly(pill.mapToItem(pill.parent, 0, 0, pill.width, pill.height), $4, 110)
        }
        Connections {
            target: root
            function onRevealedChanged() {
                if (root.revealed)
                    pixelShow.restart()
                else
                    pixelTrail.fly(pill.mapToItem(pill.parent, 0, 0, pill.width, pill.height), $3, 180)
            }
        }
$5
EOF
}

# Readable on anything without a pill: a soft dark halo around text and icons.
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
    s = s.replace(a, a + """            // PIXEL-HALO: a soft dark halo so text / icons read without a pill
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

if [ -f "$bar" ]; then
    install_component "$(dirname "$bar")"
    patch "$bar" '        // ==========================================
        // LEFT AREA (only visible when expanded)' "$(block Theme.primary Theme.tertiary -1 1 '        Connections {
            target: pill
            function onExpandedChanged() { if (pill.expanded) pixelField.burst(60) }
        }')

" && hide_pill "$bar" && halo "$bar" leftArea centerArea rightArea
fi

if [ -f "$dock" ]; then
    install_component "$(dirname "$dock")"
    patch "$dock" '        RowLayout {
            id: dockRow' "$(block DockTheme.primary DockTheme.on_surface_variant 1 -1 '')

" && hide_pill "$dock" && halo "$dock" dockRow
fi
