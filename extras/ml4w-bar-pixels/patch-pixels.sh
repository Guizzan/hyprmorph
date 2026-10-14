#!/usr/bin/env bash
# Adds the floating PixelField (hyprmorph theme) behind the content of the ML4W
# bar and dock pills. Idempotent; ML4W updates overwrite those files, so the
# installer's user post.sh re-runs this. Component: ~/.config/ml4w-statusbar/pixels/
src="${PIXELS_SRC:-$(cd "$(dirname "$0")" && pwd)}" # PixelField.qml + pixel.png
bar="${1:-$HOME/.config/quickshell/StatusbarApp/StatusbarWindow.qml}" # (the installer passes the fresh copy)
dock="$HOME/.local/share/ml4w-dock/DockApp/DockWindow.qml"

install_component() { # dir
    cp -f "$src/PixelField.qml" "$src/pixel.png" "$1/"
}

patch() { # file anchor block
    python3 -I - "$@" <<'PY'
import sys
path, anchor, block = sys.argv[1:4]
s = open(path).read()
if "PIXEL-FIELD" in s:
    print("patch-pixels: already patched", path); sys.exit(0)
if s.count(anchor) != 1:
    print("patch-pixels: anchor not found in", path, "(upstream changed?)", file=sys.stderr); sys.exit(1)
open(path, "w").write(s.replace(anchor, block + anchor, 1))
print("patch-pixels: patched", path)
PY
}

if [ -f "$bar" ]; then
    install_component "$(dirname "$bar")"
    patch "$bar" '        // ==========================================
        // LEFT AREA (only visible when expanded)' '        // PIXEL-FIELD (patch-pixels.sh): floating pixels behind the modules
        HoverHandler { id: pixelHover }
        PixelField {
            id: pixelField
            anchors.fill: pillBg
            anchors.margins: root.settings.border.width
            radius: Math.max(0, pillBg.radius - root.settings.border.width)
            colorA: Theme.primary
            colorB: Theme.tertiary
            hovered: pixelHover.hovered
            cursor: pixelHover.hovered
                ? Qt.point(pixelHover.point.position.x - anchors.margins, pixelHover.point.position.y - anchors.margins)
                : Qt.point(-1000, -1000)
        }
        Connections {
            target: pill
            function onExpandedChanged() { if (pill.expanded) pixelField.burst(80) }
        }

'
fi

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
[ -f "$bar" ] && hide_pill "$bar"

if [ -f "$dock" ]; then
    install_component "$(dirname "$dock")"
    patch "$dock" '        RowLayout {
            id: dockRow' '        // PIXEL-FIELD (patch-pixels.sh): floating pixels behind the icons
        HoverHandler { id: pixelHover }
        PixelField {
            id: pixelField
            anchors.fill: pillBg
            anchors.margins: root.settings.border.width
            radius: Math.max(0, pillBg.radius - root.settings.border.width)
            colorA: DockTheme.primary
            colorB: DockTheme.on_surface_variant
            hovered: pixelHover.hovered
            cursor: pixelHover.hovered
                ? Qt.point(pixelHover.point.position.x - anchors.margins, pixelHover.point.position.y - anchors.margins)
                : Qt.point(-1000, -1000)
        }
        Connections {
            target: root
            function onRevealedChanged() { if (root.revealed) pixelField.burst(60) }
        }

'
    hide_pill "$dock"
fi
