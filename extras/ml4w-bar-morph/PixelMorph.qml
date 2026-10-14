// PixelMorph — the ML4W bar / dock appearing and disappearing as a pixel morph
// (hyprmorph theme). While the pill is shown or hidden its live picture is cut
// into cells that fly in from / out to the screen edge (morph.frag), like the
// window morphs; the real pill is hidden meanwhile and takes over when the
// cells have landed. Put it next to the pill (window level); the window patch
// keeps the pill in place while `t > 0` instead of sliding it.
// Settings (live): ~/.config/hyprmorph/settings.conf
//   bar_morph = true | false, bar_morph_duration = seconds, bar_morph_pixel = cell px
// Installed by ~/.config/ml4w-statusbar/patch-morph.sh.
import QtQuick
import Quickshell
import Quickshell.Io

Item {
    id: root

    property Item source            // the pill
    property bool shown: true       // reveal state of the bar / dock
    property real dir: 1            // +1: cells come from / leave below, -1: above
    property real travel: 160       // how far they fly (logical px)

    property bool morphOn: true
    property real duration: 0.45
    property real cellSize: 2

    // 0 = gone, 1 = assembled; the pill stays in place while t > 0
    property real t: shown ? 1 : 0
    readonly property bool running: morphOn && t > 0.0001 && t < 0.9999

    Behavior on t {
        enabled: root.morphOn
        NumberAnimation { duration: root.duration * 1000; easing.type: Easing.Linear }
    }

    enabled: false // never takes input

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
            root.morphOn = kv["bar_morph"] === undefined ? true : (kv["bar_morph"] === "true" || kv["bar_morph"] === "1")
            root.duration = Math.max(0.15, Math.min(2, parseFloat(kv["bar_morph_duration"] ?? "0.45") || 0.45))
            root.cellSize = Math.max(1, Math.min(8, parseFloat(kv["bar_morph_pixel"] ?? "2") || 2))
        }
    }

    ShaderEffectSource {
        id: tex
        sourceItem: root.source
        hideSource: root.running
        live: true
        visible: false
    }

    ShaderEffect {
        visible: root.running && !!root.source
        // the pill's place, stretched along the flight path
        x: root.source ? root.source.x : 0
        y: root.source ? root.source.y - (root.dir < 0 ? root.travel : 0) : 0
        width: root.source ? root.source.width : 0
        height: root.source ? root.source.height + root.travel : 0
        blending: true
        fragmentShader: Qt.resolvedUrl("morph.frag.qsb")

        property point area: Qt.point(width, height)
        property point pillPos: Qt.point(0, root.dir < 0 ? root.travel : 0)
        property point pillSize: Qt.point(root.source ? root.source.width : 1, root.source ? root.source.height : 1)
        property real t: root.t
        property real cell: root.cellSize
        property real travel: root.travel
        property real dir: root.dir
        property real stagger: 0.35
        property var src: tex
    }
}
