// PixelTrail — pixels flying off a bar / dock as it slides (hyprmorph theme).
// fly(rect, dir, n): a spray of n pixels from rect (in this item's coordinates)
// in the direction of the slide (dir +1 down, -1 up), speeding away and fading.
// A 1x1 item at the window origin (never under the pointer); emitters and
// particles use window coordinates and are drawn unclipped, so they fly free.
import QtQuick
import QtQuick.Particles

Item {
    id: root

    property color colorA: "#ffffff"
    property color colorB: "#ffffff"
    property bool active: true
    property real dirAngle: 90

    function fly(rect, dir, n) {
        if (!active || rect.width <= 0)
            return
        root.dirAngle = dir > 0 ? 90 : 270
        sprayA.x = rect.x; sprayA.y = rect.y; sprayA.width = rect.width; sprayA.height = rect.height
        sprayB.x = rect.x; sprayB.y = rect.y; sprayB.width = rect.width; sprayB.height = rect.height
        sprayA.burst(Math.round(n * 0.65))
        sprayB.burst(Math.round(n * 0.35))
        shrink.restart()
    }

    // The emitters are items too: left at the pill's size they would sit under
    // the pointer and take the hover the bar / dock autohide relies on. Shrink
    // them back once the burst is out.
    Timer {
        id: shrink
        interval: 80
        onTriggered: {
            sprayA.width = 1; sprayA.height = 1
            sprayB.width = 1; sprayB.height = 1
        }
    }

    ParticleSystem {
        id: sys
        anchors.fill: parent
    }

    ImageParticle {
        system: sys
        groups: ["a"]
        source: Qt.resolvedUrl("pixel.png")
        color: root.colorA
        colorVariation: 0.06
        alpha: 0.95
        entryEffect: ImageParticle.Fade
    }
    ImageParticle {
        system: sys
        groups: ["b"]
        source: Qt.resolvedUrl("pixel.png")
        color: root.colorB
        colorVariation: 0.08
        alpha: 0.8
        entryEffect: ImageParticle.Fade
    }

    Emitter {
        id: sprayA
        system: sys
        group: "a"
        enabled: false
        lifeSpan: 900; lifeSpanVariation: 450
        size: 2.2; sizeVariation: 0.6; endSize: 0.8
        velocity: AngleDirection { angle: root.dirAngle; angleVariation: 32; magnitude: 260; magnitudeVariation: 150 }
        acceleration: AngleDirection { angle: root.dirAngle; magnitude: 420 }
    }
    Emitter {
        id: sprayB
        system: sys
        group: "b"
        enabled: false
        lifeSpan: 1100; lifeSpanVariation: 500
        size: 1.6; sizeVariation: 0.5; endSize: 0.6
        velocity: AngleDirection { angle: root.dirAngle; angleVariation: 55; magnitude: 170; magnitudeVariation: 110 }
        acceleration: AngleDirection { angle: root.dirAngle; magnitude: 300 }
    }
}
