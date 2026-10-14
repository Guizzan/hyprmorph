// PixelField — the bar / dock background as a living dot-matrix (hyprmorph theme).
// A GPU shader (dots.frag) draws a fine grid of round, anti-aliased dots in the
// theme's background colour; they dissolve softly toward the pill's edge, a
// slow wave of light flows across them, a few twinkle in the accent colours,
// and around the cursor they light up and bulge outward like under a lens.
// burst() sends a ripple ring through the dots (the bar expanding).
// Settings (live): ~/.config/hyprmorph/settings.conf
//   bar_pixels = true | false
//   bar_pixel_density = 0..3   (finer / coarser grid)
//   bar_pixel_react   = 0..3   (cursor spotlight + lens)
// Installed into the bar and the dock by ~/.config/ml4w-statusbar/patch-pixels.sh.
import QtQuick
import Quickshell
import Quickshell.Io

Item {
    id: root

    property color colorBg: "#101014"
    property color colorA: "#ffffff"
    property color colorB: "#ffffff"
    property real radius: 12
    property point cursor: Qt.point(-1000, -1000)
    property bool hovered: false

    property bool enabledSetting: true
    property real density: 1.0
    property real react: 1.0

    readonly property bool active: enabledSetting && width > 8 && height > 8 && visible

    // smoothed state fed to the shader
    property point smoothCursor: Qt.point(width / 2, height / 2)
    property real smoothHover: 0
    property real clock: 0
    property real rippleR: -1
    property real rippleStrength: 0
    property point rippleCentre: Qt.point(width / 2, height / 2)

    function burst(n) {
        rippleCentre = hovered ? cursor : Qt.point(width / 2, height / 2)
        rippleR = 0
        rippleStrength = 1
    }

    FrameAnimation {
        running: root.active
        onTriggered: {
            const dt = Math.min(frameTime, 0.05)
            root.clock += dt
            // the spotlight glides after the cursor and fades in / out
            const k = 1 - Math.exp(-dt * 14)
            if (root.hovered && root.cursor.x > -999)
                root.smoothCursor = Qt.point(root.smoothCursor.x + (root.cursor.x - root.smoothCursor.x) * k,
                                             root.smoothCursor.y + (root.cursor.y - root.smoothCursor.y) * k)
            root.smoothHover += ((root.hovered ? 1 : 0) - root.smoothHover) * (1 - Math.exp(-dt * 6))
            if (root.rippleR >= 0) {
                root.rippleR += dt * 900
                root.rippleStrength = Math.max(0, root.rippleStrength - dt * 1.1)
                if (root.rippleStrength <= 0 || root.rippleR > root.width + 200)
                    root.rippleR = -1
            }
        }
    }

    FileView {
        path: Quickshell.env("HOME") + "/.config/hyprmorph/settings.conf"
        watchChanges: true
        printErrors: false
        onFileChanged: reload()
        onLoaded: {
            const kv = {}
            for (const line of text().split("\n")) {
                const i = line.indexOf("=")
                if (i < 0 || line.trim().startsWith("#"))
                    continue
                kv[line.slice(0, i).trim()] = line.slice(i + 1).trim()
            }
            root.enabledSetting = kv["bar_pixels"] === undefined ? true : (kv["bar_pixels"] === "true" || kv["bar_pixels"] === "1")
            root.density = Math.max(0, Math.min(3, parseFloat(kv["bar_pixel_density"] ?? "1") || 0))
            root.react = Math.max(0, Math.min(3, parseFloat(kv["bar_pixel_react"] ?? "1") || 0))
        }
    }

    ShaderEffect {
        anchors.fill: parent
        visible: root.active
        blending: true
        fragmentShader: Qt.resolvedUrl("dots.frag.qsb")

        property point size: Qt.point(width, height)
        property point cursor: root.smoothCursor
        property real hover: root.smoothHover
        property real time: root.clock
        property real radius: root.radius
        // Amount 1 = 4.2 px pitch; more = finer and denser, less = coarser
        property real spacing: 4.2 / Math.sqrt(Math.max(0.35, Math.min(3, root.density)))
        property real react: root.react
        property real ripple: root.rippleR
        property real rippleA: root.rippleStrength
        property point rippleC: root.rippleCentre
        property color colorBg: root.colorBg
        property color colorA: root.colorA
        property color colorB: root.colorB
    }
}
