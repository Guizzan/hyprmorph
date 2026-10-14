// PixelField — a little physics world inside the bar / dock (hyprmorph theme).
// Two kinds of particles, each a couple of real pixels:
//   * grains: heavy, fall, bounce off the walls and off each other and pile up
//     along the bottom like sand;
//   * dust: nearly weightless, drifts with Brownian jitter.
// The cursor stirs them (its velocity is transferred to nearby particles and it
// pushes them aside); when the bar / dock slides or resizes the particles feel
// the acceleration and slosh like sand in a moving box. burst() kicks them up.
// Drawn behind the content. Settings (live): ~/.config/hyprmorph/settings.conf
//   bar_pixels = true | false, bar_pixel_density = 0..3, bar_pixel_react = 0..3
// Installed into the bar and the dock by ~/.config/ml4w-statusbar/patch-pixels.sh.
import QtQuick
import QtQuick.Window
import Quickshell
import Quickshell.Io

Item {
    id: root

    property color colorA: "#ffffff"
    property color colorB: "#ffffff"
    property real radius: 12
    property point cursor: Qt.point(-1000, -1000)
    property bool hovered: false

    property bool enabledSetting: true
    property real density: 1.0
    property real react: 1.0

    readonly property bool active: enabledSetting && width > 8 && height > 8 && visible
    // one real pixel in logical units (the bar is drawn at the monitor scale)
    readonly property real px: 1 / Math.max(1, Screen.devicePixelRatio)

    // --- physics state (flat arrays) --------------------------------------
    property var xs: []
    property var ys: []
    property var vxs: []
    property var vys: []
    property var kind: [] // 1 = grain, 0 = dust
    property var col: [] // colour bucket 0..3
    property int count: 0
    property point lastScene: Qt.point(0, 0)
    property point lastVel: Qt.point(0, 0)
    property point lastCursorPos: Qt.point(-1000, -1000)
    property bool primed: false
    property real spawnWidth: 0
    property real lastWidth: 0

    function burst(n) {
        for (let i = 0; i < root.count; i++) {
            root.vys[i] -= (60 + Math.random() * 160) * (root.kind[i] ? 1 : 0.4)
            root.vxs[i] += (Math.random() - 0.5) * 120
        }
    }

    function respawn() {
        const W = width, H = height
        if (W < 8 || H < 8) {
            count = 0
            return
        }
        const grains = Math.round(W / 4.5 * density)
        const dust = Math.round(W * H / 140 * density)
        const n = grains + dust
        const x = [], y = [], vx = [], vy = [], k = [], c = []
        for (let i = 0; i < n; i++) {
            x.push(4 + Math.random() * (W - 8))
            y.push(4 + Math.random() * (H - 8))
            vx.push(0)
            vy.push(0)
            k.push(i < grains ? 1 : 0)
            c.push(Math.floor(Math.random() * 4))
        }
        xs = x; ys = y; vxs = vx; vys = vy; kind = k; col = c
        count = n
        spawnWidth = W
        lastWidth = W
    }

    onDensityChanged: respawn()
    // the pill grows / shrinks (bar expanding, dock gaining icons): the
    // particles stretch with it; much wider than when spawned -> more of them
    onWidthChanged: {
        if (count === 0 || width > spawnWidth * 1.3 + 20) {
            respawn()
            return
        }
        if (lastWidth > 0 && width > 0) {
            const f = width / lastWidth
            for (let i = 0; i < count; i++)
                xs[i] *= f
        }
        lastWidth = width
    }
    Component.onCompleted: respawn()

    function step(dt) {
        if (count === 0)
            respawn()
        dt = Math.min(dt, 1 / 30)
        const W = width, H = height
        // the container's own acceleration (sliding, resizing): felt as a pseudo-force
        const sp = root.mapToItem(null, 0, 0)
        let ax = 0, ay = 0
        if (primed) {
            const vx = (sp.x - lastScene.x) / dt, vy = (sp.y - lastScene.y) / dt
            ax = Math.max(-6000, Math.min(6000, (vx - lastVel.x) / dt))
            ay = Math.max(-6000, Math.min(6000, (vy - lastVel.y) / dt))
            lastVel = Qt.point(vx, vy)
        }
        lastScene = Qt.point(sp.x, sp.y)
        primed = true

        // the cursor's velocity, for stirring
        let cvx = 0, cvy = 0
        const cx = cursor.x, cy = cursor.y
        if (hovered && lastCursorPos.x > -999) {
            cvx = Math.max(-3000, Math.min(3000, (cx - lastCursorPos.x) / dt))
            cvy = Math.max(-3000, Math.min(3000, (cy - lastCursorPos.y) / dt))
        }
        lastCursorPos = hovered ? Qt.point(cx, cy) : Qt.point(-1000, -1000)
        const R = 34, R2 = R * R
        const stir = react

        const G = 520 // grains' gravity, px/s^2
        const sub = 2, h = dt / sub
        const r = 1.1 // grain radius (logical px)
        for (let s = 0; s < sub; s++) {
            for (let i = 0; i < count; i++) {
                const heavy = kind[i] === 1
                let vx = vxs[i], vy = vys[i]
                vx -= ax * h * (heavy ? 0.9 : 0.35)
                vy -= ay * h * (heavy ? 0.9 : 0.35)
                if (heavy) {
                    vy += G * h
                    vx *= 1 - 0.6 * h
                    vy *= 1 - 0.3 * h
                } else {
                    // buoyant dust: Brownian jitter, strong air drag
                    vx += (Math.random() - 0.5) * 220 * h * 6
                    vy += (Math.random() - 0.5) * 220 * h * 6 + 8 * h
                    vx *= 1 - 2.5 * h
                    vy *= 1 - 2.5 * h
                }
                if (stir > 0 && hovered) {
                    const dx = xs[i] - cx, dy = ys[i] - cy, d2 = dx * dx + dy * dy
                    if (d2 < R2) {
                        const d = Math.sqrt(d2) + 0.001, f = 1 - d / R
                        // dragged along with the cursor, and pushed aside
                        vx += (cvx - vx) * f * 0.35 * stir
                        vy += (cvy - vy) * f * 0.35 * stir
                        vx += dx / d * f * 900 * stir * h
                        vy += dy / d * f * 900 * stir * h
                    }
                }
                let x = xs[i] + vx * h, y = ys[i] + vy * h
                // walls (the pill, its rounded ends approximated by the corner radius)
                const rr = Math.min(radius, H / 2, W / 2)
                const lo = r, hiX = W - r, hiY = H - r
                if (y > hiY) { y = hiY; vy = -vy * (heavy ? 0.28 : 0.5); vx *= heavy ? 0.82 : 0.95 }
                if (y < lo) { y = lo; vy = -vy * 0.5 }
                if (x < lo) { x = lo; vx = -vx * 0.4 }
                if (x > hiX) { x = hiX; vx = -vx * 0.4 }
                // rounded corners: keep inside the corner circles
                const ccx = x < rr ? rr : (x > W - rr ? W - rr : x)
                const ccy = y < rr ? rr : (y > H - rr ? H - rr : y)
                if (ccx !== x && ccy !== y) {
                    const ex = x - ccx, ey = y - ccy, ed = Math.sqrt(ex * ex + ey * ey), lim = rr - r
                    if (ed > lim && ed > 0) {
                        const nx = ex / ed, ny = ey / ed
                        x = ccx + nx * lim
                        y = ccy + ny * lim
                        const vn = vx * nx + vy * ny
                        if (vn > 0) { vx -= 1.4 * vn * nx; vy -= 1.4 * vn * ny }
                    }
                }
                xs[i] = x; ys[i] = y; vxs[i] = vx; vys[i] = vy
            }
            // grains collide with each other (spatial hash): they stack into piles
            const cell = 2.4 * r
            const grid = new Map()
            for (let i = 0; i < count; i++) {
                if (kind[i] !== 1)
                    continue
                const key = (Math.floor(xs[i] / cell) << 12) | Math.floor(ys[i] / cell)
                const a = grid.get(key)
                if (a) a.push(i); else grid.set(key, [i])
            }
            const minD = 2 * r, minD2 = minD * minD
            for (const [key, list] of grid) {
                const gx = key >> 12, gy = key & 4095
                for (let ox = -1; ox <= 1; ox++) {
                    for (let oy = -1; oy <= 1; oy++) {
                        const other = grid.get(((gx + ox) << 12) | (gy + oy))
                        if (!other)
                            continue
                        for (const i of list) {
                            for (const j of other) {
                                if (j <= i)
                                    continue
                                const dx = xs[j] - xs[i], dy = ys[j] - ys[i], d2 = dx * dx + dy * dy
                                if (d2 >= minD2 || d2 === 0)
                                    continue
                                const d = Math.sqrt(d2), push = (minD - d) * 0.5, nx = dx / d, ny = dy / d
                                xs[i] -= nx * push; ys[i] -= ny * push
                                xs[j] += nx * push; ys[j] += ny * push
                                const rv = (vxs[j] - vxs[i]) * nx + (vys[j] - vys[i]) * ny
                                if (rv < 0) {
                                    const imp = -rv * 0.6
                                    vxs[i] -= imp * nx; vys[i] -= imp * ny
                                    vxs[j] += imp * nx; vys[j] += imp * ny
                                }
                            }
                        }
                    }
                }
            }
        }
        canvas.requestPaint()
    }

    FrameAnimation {
        running: root.active
        onTriggered: root.step(frameTime)
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

    // --- drawing ----------------------------------------------------------
    function shade(c, k, a) {
        return Qt.rgba(Math.min(1, c.r * k), Math.min(1, c.g * k), Math.min(1, c.b * k), a)
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        visible: root.active
        renderStrategy: Canvas.Cooperative
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.clearRect(0, 0, width, height)
            const s = 2 // a particle: 2x2 logical px (3x3 real at 1.5x)
            const fills = [root.shade(root.colorA, 1.15, 0.95), root.shade(root.colorA, 0.8, 0.8),
                           root.shade(root.colorB, 1.0, 0.75), root.shade(root.colorB, 0.7, 0.55)]
            for (let b = 0; b < 4; b++) {
                ctx.fillStyle = fills[b]
                for (let i = 0; i < root.count; i++) {
                    if (root.col[i] !== b)
                        continue
                    const dust = root.kind[i] === 0
                    ctx.globalAlpha = dust ? 0.55 : 1
                    ctx.fillRect(root.xs[i] - s / 2, root.ys[i] - s / 2, s, s)
                }
            }
        }
    }
}
