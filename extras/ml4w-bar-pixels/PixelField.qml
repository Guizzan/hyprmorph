// PixelField — the bar / dock pill drawn in pixels (hyprmorph theme).
// The pill background is hidden; instead a shimmering line of pixels traces
// its edges, with a light dust of pixels inside, behind the content (icons and
// text stay clear). The cursor pushes pixels away and sheds sparks; burst()
// throws a puff (bar expanding).
// Settings (live): ~/.config/hyprmorph/settings.conf
//   bar_pixels = true | false, bar_pixel_density = 0..3, bar_pixel_react = 0..3
// Installed into the bar and the dock by ~/.config/ml4w-statusbar/patch-pixels.sh.
import QtQuick
import QtQuick.Particles
import QtQuick.Effects
import Quickshell
import Quickshell.Io

Item {
    id: root

    property color colorA: "#ffffff"
    property color colorB: "#ffffff"
    property real radius: 12
    // cursor, in this item's coordinates, from a HoverHandler on an ancestor
    property point cursor: Qt.point(-1000, -1000)
    property bool hovered: false

    // settings
    property bool enabledSetting: true
    property real density: 1.0
    property real react: 1.0

    readonly property bool active: enabledSetting && width > 8 && height > 8 && visible
    readonly property real edge: 3 // thickness of the pixel outline band

    function burst(n) {
        if (!active)
            return
        dust.burst(n)
    }

    // --- settings file ----------------------------------------------------
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

    // cursor speed, for the spark trail
    property point lastCursor: cursor
    property real speed: 0
    onCursorChanged: {
        const dx = cursor.x - lastCursor.x, dy = cursor.y - lastCursor.y
        speed = Math.min(60, Math.sqrt(dx * dx + dy * dy))
        lastCursor = cursor
        speedDecay.restart()
    }
    Timer {
        id: speedDecay
        interval: 60
        onTriggered: root.speed = 0
    }

    // --- particles (drawn into a layer, masked to the pill's rounded shape) --
    Item {
        id: field
        anchors.fill: parent
        visible: false
        layer.enabled: true

        ParticleSystem {
            id: sys
            anchors.fill: parent
            running: root.active
        }

        ImageParticle {
            system: sys
            groups: ["edge"]
            source: Qt.resolvedUrl("pixel.png")
            color: root.colorA
            colorVariation: 0.06
            alpha: 0.9
            alphaVariation: 0.3
            entryEffect: ImageParticle.Fade
        }
        ImageParticle {
            system: sys
            groups: ["dust"]
            source: Qt.resolvedUrl("pixel.png")
            color: root.colorB
            colorVariation: 0.08
            alpha: 0.35
            alphaVariation: 0.25
            entryEffect: ImageParticle.Fade
        }
        ImageParticle {
            system: sys
            groups: ["spark"]
            source: Qt.resolvedUrl("pixel.png")
            color: Qt.lighter(root.colorA, 1.35)
            alpha: 0.95
            entryEffect: ImageParticle.Fade
        }

        // the outline: top, bottom and the two rounded ends
        Emitter {
            system: sys
            group: "edge"
            x: root.radius * 0.6; y: 0
            width: root.width - root.radius * 1.2; height: root.edge
            emitRate: root.active ? root.width / 5 * root.density : 0
            lifeSpan: 2600; lifeSpanVariation: 1200
            size: 3; sizeVariation: 1
            velocity: AngleDirection { angle: 0; angleVariation: 180; magnitude: 5; magnitudeVariation: 5 }
        }
        Emitter {
            system: sys
            group: "edge"
            x: root.radius * 0.6; y: root.height - root.edge
            width: root.width - root.radius * 1.2; height: root.edge
            emitRate: root.active ? root.width / 5 * root.density : 0
            lifeSpan: 2600; lifeSpanVariation: 1200
            size: 3; sizeVariation: 1
            velocity: AngleDirection { angle: 0; angleVariation: 180; magnitude: 5; magnitudeVariation: 5 }
        }
        Emitter {
            system: sys
            group: "edge"
            x: 0; y: 0; width: root.edge + 1; height: root.height
            emitRate: root.active ? root.height / 4 * root.density : 0
            lifeSpan: 2600; lifeSpanVariation: 1200
            size: 3; sizeVariation: 1
            velocity: AngleDirection { angle: 90; angleVariation: 180; magnitude: 4; magnitudeVariation: 4 }
        }
        Emitter {
            system: sys
            group: "edge"
            x: root.width - root.edge - 1; y: 0; width: root.edge + 1; height: root.height
            emitRate: root.active ? root.height / 4 * root.density : 0
            lifeSpan: 2600; lifeSpanVariation: 1200
            size: 3; sizeVariation: 1
            velocity: AngleDirection { angle: 90; angleVariation: 180; magnitude: 4; magnitudeVariation: 4 }
        }
        // light dust inside
        Emitter {
            id: dust
            system: sys
            group: "dust"
            anchors.fill: parent
            emitRate: root.active ? root.width * root.height / 2600 * root.density : 0
            lifeSpan: 3800; lifeSpanVariation: 1600
            size: 2; sizeVariation: 1
            velocity: AngleDirection { angleVariation: 360; magnitude: 4; magnitudeVariation: 4 }
        }

        // sparks shed by the moving cursor
        Emitter {
            system: sys
            group: "spark"
            x: root.cursor.x - 2
            y: root.cursor.y - 2
            width: 4
            height: 4
            enabled: root.active && root.hovered && root.react > 0
            emitRate: root.speed * 3 * root.react
            lifeSpan: 650
            lifeSpanVariation: 300
            size: 3
            endSize: 1
            velocity: AngleDirection { angleVariation: 360; magnitude: 45; magnitudeVariation: 30 }
        }

        Wander {
            system: sys
            groups: ["edge"]
            anchors.fill: parent
            xVariance: 10
            yVariance: 2
            pace: 20
        }
        Wander {
            system: sys
            groups: ["dust"]
            anchors.fill: parent
            xVariance: 12
            yVariance: 8
            pace: 25
        }
        // the cursor pushes pixels away
        Attractor {
            system: sys
            groups: ["edge", "dust"]
            anchors.fill: parent
            enabled: root.active && root.hovered && root.react > 0
            pointX: root.cursor.x
            pointY: root.cursor.y
            strength: -9000 * root.react
            affectedParameter: Attractor.Velocity
            proportionalToDistance: Attractor.InverseQuadratic
        }
        Friction {
            system: sys
            groups: ["edge", "dust", "spark"]
            anchors.fill: parent
            factor: 2.2
        }
    }

    Rectangle {
        id: shape
        anchors.fill: parent
        radius: root.radius
        visible: false
        layer.enabled: true
    }

    MultiEffect {
        anchors.fill: parent
        source: field
        maskEnabled: true
        maskSource: shape
        visible: root.active
    }
}
