// PixelField — floating pixels behind the bar / dock content (hyprmorph theme).
// Square pixels in the theme colours drift and twinkle inside the pill; the
// cursor pushes them away like a force field and sheds sparks as it moves;
// burst() throws a puff of pixels (used when the bar expands).
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

    function burst(n) {
        if (!active)
            return
        floatA.burst(Math.round(n * 0.6))
        floatB.burst(Math.round(n * 0.4))
    }

    // --- settings file ----------------------------------------------------
    FileView {
        id: conf
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
            groups: ["a"]
            source: Qt.resolvedUrl("pixel.png")
            color: root.colorA
            colorVariation: 0.08
            alpha: 0.55
            alphaVariation: 0.35
            entryEffect: ImageParticle.Fade
        }
        ImageParticle {
            system: sys
            groups: ["b"]
            source: Qt.resolvedUrl("pixel.png")
            color: root.colorB
            colorVariation: 0.08
            alpha: 0.45
            alphaVariation: 0.35
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

        // drifting pixels all over the pill
        Emitter {
            id: floatA
            system: sys
            group: "a"
            anchors.fill: parent
            emitRate: root.active ? Math.max(0.5, root.width * root.height / 1400 * root.density) : 0
            lifeSpan: 4200
            lifeSpanVariation: 1800
            size: 3
            sizeVariation: 1
            velocity: AngleDirection { angleVariation: 360; magnitude: 4; magnitudeVariation: 4 }
        }
        Emitter {
            id: floatB
            system: sys
            group: "b"
            anchors.fill: parent
            emitRate: root.active ? Math.max(0.3, root.width * root.height / 2400 * root.density) : 0
            lifeSpan: 4800
            lifeSpanVariation: 2000
            size: 2
            sizeVariation: 1
            velocity: AngleDirection { angleVariation: 360; magnitude: 3; magnitudeVariation: 3 }
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

        // floating: a lazy random walk
        Wander {
            system: sys
            groups: ["a", "b"]
            anchors.fill: parent
            xVariance: 14
            yVariance: 8
            pace: 25
        }
        // the cursor pushes pixels away
        Attractor {
            system: sys
            groups: ["a", "b"]
            anchors.fill: parent
            enabled: root.active && root.hovered && root.react > 0
            pointX: root.cursor.x
            pointY: root.cursor.y
            strength: -9000 * root.react
            affectedParameter: Attractor.Velocity
            proportionalToDistance: Attractor.InverseQuadratic
        }
        // and they settle down again
        Friction {
            system: sys
            groups: ["a", "b", "spark"]
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
