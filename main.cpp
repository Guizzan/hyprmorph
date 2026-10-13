// hyprmorph — the arctis-hub OLED page morph (arctishub/oled.py, class Morph),
// ported to Hyprland workspace switches.
//
// The deck works on 1-bit pages: lit pixels are the content. Here "lit" is the
// content of the windows: pixels that stand out from their surroundings (text,
// lines, borders, icons). Exactly as on the deck:
//   * pixels lit on both workspaces stay put;
//   * pixels lit only before (src) and only after (dst) are each ordered along
//     a Hilbert curve and paired in that order, the smaller side resampled;
//   * every particle has its own random take-off delay (stagger) and a random
//     sideways bow, eased in-out cubic, over MORPH_SECONDS;
//   * with nothing to fly from (or to), particles come in from (or leave
//     through) the nearest screen edge (the deck grows from the centre; the
//     edges were asked for on the desktop).
// Everything that is not "lit" (window backgrounds, wallpaper) cross-fades
// underneath, as the deck's black background stays black.
//
// Rendering: every frame the damaged part of the composited windows is copied
// into `last` (render buffers only hold valid pixels inside the damage). On a
// switch `last` freezes as the old picture; each morph frame copies the new
// frame to `neu` and the background (pre-windows) to `bg`, then draws a base
// pass (cross-faded backgrounds with the lit pixels taken out) and the
// particles. Hyprland's own workspace animation must be disabled.

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>

#include <GLES3/gl32.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>

static HANDLE PHANDLE = nullptr; // not inline: a GNU-unique symbol would pin the .so on unload

namespace {
    // the deck's constants (oled.py)
    // Tunables, read from ~/.config/hyprmorph/settings.conf (see loadSettings).
    float MORPH_SECONDS = 0.3f;
    float STAGGER       = 0.35f; // fraction of the run over which particles set off
    float CURL          = 0.18f; // sideways bow of each flight path, per pixel travelled

    // desktop port
    int              CELL_PX       = 3;     // one deck "pixel" = CELL_PX x CELL_PX physical pixels
    constexpr int    LOCAL_RADIUS  = 6;     // cells; neighbourhood a pixel must stand out from to be "lit"
    float            LIT_CONTRAST  = 0.10f; // luma difference from the neighbourhood that counts as lit
    constexpr int    LOW_DIV       = 12;    // background estimate resolution (cells per texel)
    constexpr double EDGE_LOGICAL  = 8.0;   // window boxes grown by this to take borders along
    int              BLOB_CELL_PX  = 3;     // particle size of a window being dragged (same pixels as the morph)
    constexpr float  BLOB_RAMP     = 0.25f; // pixels peel off over 1.5x this (staggered)
    double           BLOB_HALF     = 160.0;  // dragged window blob: half its longer side, logical px
    // settings.conf only
    bool  MORPH_WORKSPACES = true, MORPH_WINDOWS = true, DRAG_BLOB = true;
    int   COLOR_MODE       = 0;    // 0 the windows' own colours, 1 theme (wallpaper) colours, 2 custom colour
    float TINT             = 0.7f; // how strongly pixels take the colour (modes 1, 2)
    float TINT_A[3] = {1, 1, 1}, TINT_B[3] = {1, 1, 1}; // gradient across the pixels
    float BLOB_TRAIL  = 1.6f;      // tail length, in blob sizes
    float BLOB_WOBBLE = 1.0f;      // outline waves and rim fizz
    constexpr int    SAME_DIFF     = 6;     // a cell whose pixels all differ by at most this (of 255) did not change

    using Clock = std::chrono::steady_clock;

    struct SInstance {
        float srcX, srcY, dstX, dstY, delay, bow;
    };

    struct SWinBox {
        uintptr_t id; // the window, so a layout change knows which window went where
        CBox      box;
    };

    struct SMonitorState {
        SP<Render::IFramebuffer> last, bg, neu, lowOld, lowNew, cells, diff, shown; // shown: last morph frame on screen
        bool                     morphing = false, paired = false, uploaded = false, maskUploaded = false, restart = false;
        Clock::time_point        start;
        std::vector<SInstance>   instances;
        std::vector<uint8_t>     mask; // per cell: 1 lit before, 2 lit after, 4 window before, 8 window after (rows bottom-up)
        uint32_t                 cols = 0, rows = 0;
        GLuint                   vbo = 0, maskTex = 0;
        PHLWORKSPACEREF          lastWorkspace;
        std::vector<CBox>        fromBoxes, toBoxes;
        double                   scale = 1.0;
        Vector2D                 monPos;
        // geometry watcher: any change of the visible windows morphs
        std::vector<SWinBox>     cachedBoxes;
        std::vector<SWinBox>     stableBoxes; // layout before a run of changing frames
        // layout morph (same windows, new places): pixels fly window to window
        bool                     layoutMode = false;
        float                    dir        = 1.f; // workspace switch: +1 = the desktop moves right (higher workspace)
        std::vector<std::pair<CBox, CBox>> layoutPairs; // a window's old box -> its new box
        std::vector<CBox>        layoutGone, layoutNew;
        std::vector<CBox>        layoutClear; // shows the background (the spot a dragged window left)
        bool                     dragFirst = false; // first drag frame: keep `last` from before the drag
        std::optional<CBox>      dragLeft;          // the dragged window's box before the drag, until the others close the gap
        int                      changingFrames = 0;
        WORKSPACEID              cachedWs = WORKSPACE_INVALID;
        // drag blob
        PHLWINDOWREF             dragWin;
        Clock::time_point        dragStart;
        std::vector<float>       blobCells; // per particle: snapshot x, y, angle, radius fraction, speed, phase
        SP<Render::IFramebuffer> blobSnap;  // the window as it looked when the drag started
        CBox                     blobStart; // its box then (GL px)
        float                    blobCell = 6.f, blobRadius = 100.f, blobBlock = 3.f;
        Vector2D                 blobHalf;
        bool                     blobReady = false;
        std::vector<float>       trail;     // cursor path (GL px pairs), newest first
        Clock::time_point        trailLast;
        PHLWINDOWREF             hiddenWin; // the real window is hidden while it is a blob
        GLuint                   blobVbo = 0;
        bool                     blobUploaded = false;
    };

    std::unordered_map<MONITORID, SMonitorState> g_mon;
    std::vector<CHyprSignalListener>            g_listeners;

    // ---------------------------------------------------------------- GL ---

    const char* PART_VERT = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 corner;
layout(location = 1) in vec2 src;
layout(location = 2) in vec2 dst;
layout(location = 3) in vec2 delayBow;
uniform float t;
uniform vec2  fbSize;
uniform float cell;
uniform float stagger;
out vec2  uvOld;
out vec2  uvNew;
out float mixAmt;
out float flight;
flat out float seed;
void main() {
    float local = clamp((t - delayBow.x) / (1.0 - stagger), 0.0, 1.0);
    float e = local < 0.5 ? 4.0 * local * local * local
                          : 1.0 - pow(-2.0 * local + 2.0, 3.0) / 2.0;   // ease in-out cubic
    vec2 d = dst - src;
    vec2 n = vec2(-d.y, d.x);
    vec2 p = src + d * e + n * abs(delayBow.y) * sin(3.14159265 * e);
    p = floor(p / cell + 0.5) * cell;                                  // land on the pixel grid, like the deck
    vec2 px = p + corner * cell;
    gl_Position = vec4(px / fbSize * 2.0 - 1.0, 0.0, 1.0);
    uvOld  = (src + corner * cell) / fbSize;
    uvNew  = (dst + corner * cell) / fbSize;
    mixAmt = delayBow.y < 0.0 ? 1.0 : e; // from off screen: new colour all the way
    flight = sqrt(sin(3.14159265 * local)); // tinted while flying, true colour at both ends
    seed   = fract(delayBow.x * 91.7 + abs(delayBow.y) * 13.3);
}
)";

    const char* PART_FRAG = R"(#version 300 es
precision highp float;
in vec2  uvOld;
in vec2  uvNew;
in float mixAmt;
in float flight;
flat in float seed;
uniform sampler2D oldTex;
uniform sampler2D newTex;
uniform vec3  tintA;
uniform vec3  tintB;
uniform float tint;
out vec4 frag;
void main() {
    vec4 c = mix(texture(oldTex, uvOld), texture(newTex, uvNew), mixAmt);
    // optional colour: the pixel keeps its brightness, takes the theme / custom hue
    float l = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
    vec3  t = mix(tintA, tintB, seed) * (0.35 + 0.9 * l);
    frag = vec4(mix(c.rgb, t, tint * flight), c.a);
}
)";

    const char* BASE_VERT = R"(#version 300 es
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
)";

    // Under the particles: the old picture cross-fades into the new one, with
    // every lit pixel replaced by its neighbourhood (the low-res copy), so text
    // and lines only exist as particles while they move. Pixels lit on both
    // sides stay as they are.
    const char* BASE_FRAG = R"(#version 300 es
precision highp float;
uniform sampler2D oldTex;
uniform sampler2D newTex;
uniform sampler2D lowOld;
uniform sampler2D lowNew;
uniform sampler2D mask;
uniform sampler2D bgTex;
uniform vec2  fbSize;
uniform float cell;
uniform float fade;
uniform float t;
out vec4 frag;
void main() {
    ivec2 px  = ivec2(gl_FragCoord.xy);
    vec2  uv  = gl_FragCoord.xy / fbSize;
    float m   = texelFetch(mask, ivec2(gl_FragCoord.xy / cell), 0).r * 255.0;
    int   bits = int(m + 0.5);
    bool  litBefore = (bits & 1) != 0;
    bool  litAfter  = (bits & 2) != 0;
    bool  winBefore = (bits & 4) != 0;
    bool  winAfter  = (bits & 8) != 0;
    if (winBefore != winAfter) {
        // a window only on one side: its body exists only as flying pixels
        frag = texelFetch(bgTex, px, 0);
        return;
    }
    vec4  o = texelFetch(oldTex, px, 0);
    vec4  n = texelFetch(newTex, px, 0);
    if (litBefore && litAfter) {
        frag = mix(o, n, fade); // the deck's "still" pixels
        return;
    }
    if (litBefore)
        o = texture(lowOld, uv);
    if (litAfter)
        n = texture(lowNew, uv);
    if (winBefore && winAfter) {
        // the old window dissolves first, the new one only forms as its pixels land
        vec4 b = texelFetch(bgTex, px, 0);
        frag = t < 0.5 ? mix(o, b, smoothstep(0.0, 0.4, t)) : mix(b, n, smoothstep(0.6, 1.0, t));
        return;
    }
    frag = mix(o, n, fade);
}
)";

    // Per cell: the largest change of any of its pixels between the two
    // pictures. Cells that did not change stay put instead of morphing.
    const char* DIFF_FRAG = R"(#version 300 es
precision highp float;
uniform sampler2D oldTex;
uniform sampler2D newTex;
uniform int   cell;
uniform ivec2 fbSize;
out vec4 frag;
void main() {
    ivec2 base = ivec2(gl_FragCoord.xy) * cell;
    float d    = 0.0;
    for (int y = 0; y < cell; y++)
        for (int x = 0; x < cell; x++) {
            ivec2 px = min(base + ivec2(x, y), fbSize - 1);
            vec3  c  = abs(texelFetch(oldTex, px, 0).rgb - texelFetch(newTex, px, 0).rgb);
            d        = max(d, max(c.r, max(c.g, c.b)));
        }
    frag = vec4(d, d, d, 1.0);
}
)";

    // A window being dragged becomes a "liquid window": a fixed-size blob made
    // of the window's own pixels, still laid out like the window (so you see
    // what you carry) but with a living, wobbling outline, a stretch along the
    // movement and a few pixels trailing behind.
    const char* BLOB_VERT = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 corner;
layout(location = 1) in vec2 rel;      // block of the snapshot this pixel shows (top-left)
layout(location = 2) in vec4 orbit;    // wobble angle, trail lag (0 = in the blob), wobble speed, phase
uniform vec2  startOrigin;  // window top-left when the drag began, framebuffer px
uniform vec2  snapSize;
uniform vec2  cursor;       // GL px
uniform vec2  trail[48];    // recent cursor path, [0] newest
uniform int   trailCount;
uniform vec2  halfSize;     // blob half extents (the window's aspect, fixed size)
uniform float block;        // snapshot px per blob pixel
uniform vec2  fbSize;
uniform float cell;
uniform float time;
uniform float amount;       // 0..1.5 since the drag began (pixels peel off staggered)
uniform float wobble;
uniform float trailLen;
flat out vec2 uvC;
flat out float seed;
void main() {
    vec2  c   = rel + 0.5 * block;
    vec2  q   = (c / snapSize * 2.0 - 1.0) * halfSize;
    float len = length(q);
    vec2  dir = len > 0.001 ? q / len : vec2(1.0, 0.0);
    float a   = atan(dir.y, dir.x);
    // window rectangle -> rounded organic outline, the layout inside kept
    float rb    = min(halfSize.x / max(abs(dir.x), 1e-4), halfSize.y / max(abs(dir.y), 1e-4));
    float f     = len / rb;                                   // 0 centre .. 1 rim
    float re    = halfSize.x * halfSize.y / length(vec2(halfSize.y * dir.x, halfSize.x * dir.y));
    float edge  = f * f * f * f;
    float shape = 1.0 + wobble * (0.10 * sin(3.0 * a + time * 1.3) + 0.07 * sin(5.0 * a - time * 2.1)
                      + 0.05 * sin(2.0 * a + time * 0.7 + 1.7));
    float rr    = mix(rb, re, 0.55) * mix(1.0, shape, 0.35 + 0.65 * edge);
    vec2  off   = dir * f * rr;
    // living pixels: a small shimmer inside, a loose fizzing rim
    float ph = orbit.w * 6.2831853;
    off += vec2(cos(orbit.x + time * orbit.z), sin(orbit.x + time * orbit.z)) * cell * (0.25 + 2.5 * edge) * wobble;
    off += dir * cell * 3.0 * edge * (0.5 + 0.5 * sin(time * 2.3 + ph)) * wobble;
    // trailing pixels follow the cursor's path with a delay and thin out
    float lag = orbit.y;
    float fi  = lag * float(max(trailCount - 1, 0));
    int   i0  = int(floor(fi));
    int   i1  = min(i0 + 1, max(trailCount - 1, 0));
    vec2  at  = trailCount > 0 ? mix(trail[i0], trail[i1], fract(fi)) : cursor;
    // a short tail, as wide as the blob itself
    vec2  back    = at - (trailCount > 0 ? trail[0] : cursor);
    float maxBack = trailLen * max(halfSize.x, halfSize.y);
    if (length(back) > maxBack)
        at = (trailCount > 0 ? trail[0] : cursor) + back / length(back) * maxBack;
    off      *= 1.0 - 0.12 * lag;
    // stretch along the movement like a liquid drop
    vec2  vel   = trailCount > 6 ? trail[0] - trail[6] : vec2(0.0);
    float speed = length(vel);
    if (speed > 1.0) {
        vec2  dirV    = vel / speed;
        vec2  perp    = vec2(-dirV.y, dirV.x);
        float stretch = 1.0 + min(speed / max(max(halfSize.x, halfSize.y), 1.0), 1.5) * 0.5;
        off           = dirV * dot(off, dirV) * stretch + perp * dot(off, perp) / sqrt(stretch);
    }
    // pixels peel off the window one by one, like the morph
    float d  = fract(orbit.w * 5.3) * 0.5;
    float k  = clamp((amount - d) / 0.5, 0.0, 1.0);
    k        = k * k * (3.0 - 2.0 * k);
    float sz = mix(max(block, cell), cell, k);
    vec2  p  = mix(startOrigin + rel, at + off - 0.5 * cell, k);
    p        = floor(p / cell + 0.5) * cell;
    gl_Position = vec4((p + corner * sz) / fbSize * 2.0 - 1.0, 0.0, 1.0);
    uvC         = c / snapSize;
    seed        = orbit.w;
}
)";

    const char* BLOB_FRAG = R"(#version 300 es
precision highp float;
flat in vec2 uvC;
flat in float seed;
uniform sampler2D tex;
uniform vec2 blockUv;     // one blob pixel in snapshot uv
uniform vec3  tintA;
uniform vec3  tintB;
uniform float tint;
out vec4 frag;
void main() {
    // the average of the block this pixel stands for, so the shrunk window stays readable
    vec3 c = vec3(0.0);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            c += texture(tex, uvC + vec2(x, y) * blockUv * 0.33).rgb;
    c /= 9.0;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    frag = vec4(mix(c, mix(tintA, tintB, seed) * (0.35 + 0.9 * l), tint), 1.0);
}
)";

    struct SGL {
        GLuint part = 0, base = 0, blob = 0, diff = 0, vao = 0, quad = 0;
        GLint  dOld = -1, dNew = -1, dCell = -1, dFb = -1;
        GLint  oTrail = -1, oTrailCount = -1, oHalf = -1, oBlock = -1, oBlockUv = -1;
        GLint  oOrigin = -1, oSnap = -1, oCursor = -1, oRadius = -1, oFb = -1, oCell = -1, oTime = -1, oAmount = -1, oTex = -1;
        GLint  pT = -1, pFb = -1, pCell = -1, pStagger = -1, pOld = -1, pNew = -1, pTintA = -1, pTintB = -1, pTint = -1;
        GLint  oTintA = -1, oTintB = -1, oTint = -1, oWobble = -1, oTrailLen = -1;
        GLint  bOld = -1, bNew = -1, bLowOld = -1, bLowNew = -1, bMask = -1, bBg = -1, bFb = -1, bCell = -1, bFade = -1, bT = -1;
        bool   failed = false;
    } g_gl;

    GLuint compile(GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            HyprlandAPI::addNotification(PHANDLE, std::string("[hyprmorph] shader: ") + log, CHyprColor{1, 0.2, 0.2, 1}, 8000);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    GLuint link(const char* vsrc, const char* fsrc) {
        GLuint vs = compile(GL_VERTEX_SHADER, vsrc), fs = compile(GL_FRAGMENT_SHADER, fsrc);
        if (!vs || !fs) {
            if (vs)
                glDeleteShader(vs);
            if (fs)
                glDeleteShader(fs);
            return 0;
        }
        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            glDeleteProgram(p);
            return 0;
        }
        return p;
    }

    bool ensureGL() {
        if (g_gl.part)
            return true;
        if (g_gl.failed)
            return false;
        g_gl.part = link(PART_VERT, PART_FRAG);
        g_gl.base = link(BASE_VERT, BASE_FRAG);
        g_gl.blob = link(BLOB_VERT, BLOB_FRAG);
        g_gl.diff  = link(BASE_VERT, DIFF_FRAG);
        if (!g_gl.part || !g_gl.base || !g_gl.blob || !g_gl.diff) {
            g_gl.failed = true;
            return false;
        }
        g_gl.oOrigin  = glGetUniformLocation(g_gl.blob, "startOrigin");
        g_gl.oSnap    = glGetUniformLocation(g_gl.blob, "snapSize");
        g_gl.oCursor  = glGetUniformLocation(g_gl.blob, "cursor");
        g_gl.oTrail   = glGetUniformLocation(g_gl.blob, "trail[0]");
        g_gl.oTrailCount = glGetUniformLocation(g_gl.blob, "trailCount");
        g_gl.dOld     = glGetUniformLocation(g_gl.diff, "oldTex");
        g_gl.dNew     = glGetUniformLocation(g_gl.diff, "newTex");
        g_gl.dCell    = glGetUniformLocation(g_gl.diff, "cell");
        g_gl.dFb      = glGetUniformLocation(g_gl.diff, "fbSize");
        g_gl.oHalf     = glGetUniformLocation(g_gl.blob, "halfSize");
        g_gl.oBlock    = glGetUniformLocation(g_gl.blob, "block");
        g_gl.oBlockUv  = glGetUniformLocation(g_gl.blob, "blockUv");
        g_gl.oTintA    = glGetUniformLocation(g_gl.blob, "tintA");
        g_gl.oTintB    = glGetUniformLocation(g_gl.blob, "tintB");
        g_gl.oTint     = glGetUniformLocation(g_gl.blob, "tint");
        g_gl.oWobble   = glGetUniformLocation(g_gl.blob, "wobble");
        g_gl.oTrailLen = glGetUniformLocation(g_gl.blob, "trailLen");
        g_gl.pTintA    = glGetUniformLocation(g_gl.part, "tintA");
        g_gl.pTintB    = glGetUniformLocation(g_gl.part, "tintB");
        g_gl.pTint     = glGetUniformLocation(g_gl.part, "tint");
        g_gl.oFb      = glGetUniformLocation(g_gl.blob, "fbSize");
        g_gl.oCell    = glGetUniformLocation(g_gl.blob, "cell");
        g_gl.oTime    = glGetUniformLocation(g_gl.blob, "time");
        g_gl.oAmount  = glGetUniformLocation(g_gl.blob, "amount");
        g_gl.oTex     = glGetUniformLocation(g_gl.blob, "tex");
        g_gl.pT       = glGetUniformLocation(g_gl.part, "t");
        g_gl.pFb      = glGetUniformLocation(g_gl.part, "fbSize");
        g_gl.pCell    = glGetUniformLocation(g_gl.part, "cell");
        g_gl.pStagger = glGetUniformLocation(g_gl.part, "stagger");
        g_gl.pOld     = glGetUniformLocation(g_gl.part, "oldTex");
        g_gl.pNew     = glGetUniformLocation(g_gl.part, "newTex");
        g_gl.bOld     = glGetUniformLocation(g_gl.base, "oldTex");
        g_gl.bNew     = glGetUniformLocation(g_gl.base, "newTex");
        g_gl.bLowOld  = glGetUniformLocation(g_gl.base, "lowOld");
        g_gl.bLowNew  = glGetUniformLocation(g_gl.base, "lowNew");
        g_gl.bMask    = glGetUniformLocation(g_gl.base, "mask");
        g_gl.bBg      = glGetUniformLocation(g_gl.base, "bgTex");
        g_gl.bFb      = glGetUniformLocation(g_gl.base, "fbSize");
        g_gl.bCell    = glGetUniformLocation(g_gl.base, "cell");
        g_gl.bFade    = glGetUniformLocation(g_gl.base, "fade");
        g_gl.bT       = glGetUniformLocation(g_gl.base, "t");

        GLint prevVao = 0, prevBuf = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
        glGenVertexArrays(1, &g_gl.vao);
        glGenBuffers(1, &g_gl.quad);
        glBindVertexArray(g_gl.vao);
        const float corners[] = {0, 0, 1, 0, 0, 1, 1, 1};
        glBindBuffer(GL_ARRAY_BUFFER, g_gl.quad);
        glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(prevVao);
        glBindBuffer(GL_ARRAY_BUFFER, prevBuf);
        return true;
    }

    std::optional<GLuint> fbID(const SP<Render::IFramebuffer>& fb) {
        auto* gl = dynamic_cast<Render::GL::CGLFramebuffer*>(fb.get());
        if (!gl)
            return std::nullopt;
        return gl->getFBID();
    }

    bool ensureFB(SP<Render::IFramebuffer>& fb, int w, int h, DRMFormat fmt, const char* name) {
        if (fb && fb->m_size == Vector2D{(double)w, (double)h} && fb->m_drmFormat == fmt)
            return true;
        if (!fb)
            fb = g_pHyprRenderer->createFB(name);
        return fb->alloc(w, h, fmt);
    }

    bool ensureFB(SP<Render::IFramebuffer>& fb, const SP<Render::IFramebuffer>& like, const char* name) {
        return ensureFB(fb, (int)like->m_size.x, (int)like->m_size.y, like->m_drmFormat, name);
    }

    // Copy (and optionally scale) `from` into `to`. Leaves bindings and scissor as they were.
    void blit(const SP<Render::IFramebuffer>& from, const SP<Render::IFramebuffer>& to, bool linear = false, const CRegion* rects = nullptr) {
        const auto A = fbID(from), B = fbID(to);
        if (!A || !B)
            return;
        GLint prevRead = 0, prevDraw = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
        const GLboolean SCISSOR = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, *A);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, *B);
        if (rects) {
            for (const auto& r : rects->getRects())
                glBlitFramebuffer(r.x1, r.y1, r.x2, r.y2, r.x1, r.y1, r.x2, r.y2, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        } else
            glBlitFramebuffer(0, 0, (GLint)from->m_size.x, (GLint)from->m_size.y, 0, 0, (GLint)to->m_size.x, (GLint)to->m_size.y, GL_COLOR_BUFFER_BIT,
                              linear ? GL_LINEAR : GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
        if (SCISSOR)
            glEnable(GL_SCISSOR_TEST);
    }

    // The render buffer only holds valid pixels inside this frame's damage, so
    // copy just those rects (rotated into the framebuffer's orientation, as
    // Hyprland does) into a persistent buffer.
    void blitDamage(const PHLMONITOR& mon, const SP<Render::IFramebuffer>& from, const SP<Render::IFramebuffer>& to) {
        CRegion region = g_pHyprRenderer->m_renderData.damage.copy();
        region.intersect(CBox{{}, mon->m_transformedSize});
        region.transform(Math::wlTransformToHyprutils(Math::invertTransform(mon->m_transform)), mon->m_transformedSize.x, mon->m_transformedSize.y);
        region.intersect(CBox{{}, from->m_size});
        if (!region.empty())
            blit(from, to, false, &region);
    }

    bool readPixels(const SP<Render::IFramebuffer>& fb, std::vector<uint8_t>& out) {
        const auto A = fbID(fb);
        if (!A)
            return false;
        GLint prevRead = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, *A);
        out.resize((size_t)fb->m_size.x * fb->m_size.y * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, (GLsizei)fb->m_size.x, (GLsizei)fb->m_size.y, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
        return true;
    }

    // 1 for every cell where the old and new picture differ, empty on failure.
    std::vector<uint8_t> changedCells(SMonitorState& st, uint32_t cols, uint32_t rows) {
        std::vector<uint8_t> out;
        if (!ensureGL() || !ensureFB(st.diff, (int)cols, (int)rows, DRM_FORMAT_ABGR8888, "hyprmorph-diff"))
            return out;
        const auto TARGET = fbID(st.diff);
        if (!TARGET)
            return out;
        GLint prevProg = 0, prevVao = 0, prevFb = 0, prevActive = 0, prevTex[2] = {}, vp[4];
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFb);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
        for (int u = 0; u < 2; ++u) {
            glActiveTexture(GL_TEXTURE0 + u);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex[u]);
        }
        glGetIntegerv(GL_VIEWPORT, vp);
        const GLboolean BLEND = glIsEnabled(GL_BLEND), SCISSOR = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, *TARGET);
        glViewport(0, 0, (GLsizei)cols, (GLsizei)rows);
        glBindVertexArray(g_gl.vao);
        glActiveTexture(GL_TEXTURE0);
        st.last->getTexture()->bind();
        glActiveTexture(GL_TEXTURE1);
        st.neu->getTexture()->bind();
        glUseProgram(g_gl.diff);
        glUniform1i(g_gl.dOld, 0);
        glUniform1i(g_gl.dNew, 1);
        glUniform1i(g_gl.dCell, CELL_PX);
        glUniform2i(g_gl.dFb, (GLint)st.last->m_size.x, (GLint)st.last->m_size.y);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        for (int u = 1; u >= 0; --u) {
            glActiveTexture(GL_TEXTURE0 + u);
            glBindTexture(GL_TEXTURE_2D, prevTex[u]);
        }
        glActiveTexture(prevActive);
        glUseProgram(prevProg);
        glBindVertexArray(prevVao);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFb);
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        if (BLEND)
            glEnable(GL_BLEND);
        if (SCISSOR)
            glEnable(GL_SCISSOR_TEST);

        std::vector<uint8_t> rgba;
        if (!readPixels(st.diff, rgba))
            return out;
        out.resize((size_t)cols * rows);
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = rgba[i * 4] > SAME_DIFF;
        return out;
    }

    // --------------------------------------------------------- the morph ---

    uint64_t hilbert(uint32_t x, uint32_t y, uint32_t order) {
        uint64_t d = 0;
        for (uint32_t s = order / 2; s > 0; s /= 2) {
            const uint32_t RX = (x & s) > 0, RY = (y & s) > 0;
            d += (uint64_t)s * s * ((3 * RX) ^ RY);
            if (RY == 0) {
                if (RX == 1) {
                    x = s - 1 - x;
                    y = s - 1 - y;
                }
                std::swap(x, y);
            }
        }
        return d;
    }

    // LSD radix sort on the upper 32 bits (key), 2 passes of 16 bits.
    void radixSort(std::vector<uint64_t>& v) {
        std::vector<uint64_t> tmp(v.size());
        for (int shift = 32; shift < 64; shift += 16) {
            std::vector<size_t> count(65537, 0);
            for (auto x : v)
                ++count[((x >> shift) & 0xFFFF) + 1];
            for (size_t k = 1; k < count.size(); ++k)
                count[k] += count[k - 1];
            for (auto x : v)
                tmp[count[(x >> shift) & 0xFFFF]++] = x;
            v.swap(tmp);
        }
    }

    // "lit" cells of a picture: inside the given windows and standing out from
    // their neighbourhood (summed-area table box mean)
    std::vector<uint8_t> litCells(const std::vector<uint8_t>& rgba, uint32_t cols, uint32_t rows, const std::vector<uint8_t>& inWindows) {
        std::vector<float> luma((size_t)cols * rows);
        for (size_t i = 0; i < luma.size(); ++i)
            luma[i] = (0.2126f * rgba[i * 4] + 0.7152f * rgba[i * 4 + 1] + 0.0722f * rgba[i * 4 + 2]) / 255.f;
        std::vector<float> sat((size_t)(cols + 1) * (rows + 1), 0.f);
        for (uint32_t y = 0; y < rows; ++y) {
            float run = 0;
            for (uint32_t x = 0; x < cols; ++x) {
                run += luma[(size_t)y * cols + x];
                sat[(size_t)(y + 1) * (cols + 1) + x + 1] = sat[(size_t)y * (cols + 1) + x + 1] + run;
            }
        }
        std::vector<uint8_t> lit((size_t)cols * rows, 0);
        const int            R = LOCAL_RADIUS;
        for (uint32_t y = 0; y < rows; ++y) {
            for (uint32_t x = 0; x < cols; ++x) {
                const size_t I = (size_t)y * cols + x;
                if (!inWindows[I])
                    continue;
                const int    X0 = std::max(0, (int)x - R), X1 = std::min((int)cols, (int)x + R + 1);
                const int    Y0 = std::max(0, (int)y - R), Y1 = std::min((int)rows, (int)y + R + 1);
                const float  S  = sat[(size_t)Y1 * (cols + 1) + X1] - sat[(size_t)Y0 * (cols + 1) + X1] - sat[(size_t)Y1 * (cols + 1) + X0] + sat[(size_t)Y0 * (cols + 1) + X0];
                const float  MEAN = (float)(S / ((X1 - X0) * (Y1 - Y0)));
                lit[I]            = std::abs(luma[I] - MEAN) > LIT_CONTRAST;
            }
        }
        return lit;
    }

    // Build the particles on the first morph frame (both pictures exist then).
    void pairLayoutParticles(SMonitorState& st);

    void pairParticles(SMonitorState& st) {
        if (st.layoutMode) {
            pairLayoutParticles(st);
            return;
        }
        const Vector2D FB   = st.last->m_size;
        const float    CELL = (float)CELL_PX;
        const uint32_t COLS = (uint32_t)std::ceil(FB.x / CELL), ROWS = (uint32_t)std::ceil(FB.y / CELL);
        uint32_t       order = 1;
        while (order < std::max(COLS, ROWS))
            order *= 2;

        st.instances.clear();
        st.cols = COLS;
        st.rows = ROWS;
        st.mask.assign((size_t)COLS * ROWS, 0);
        st.maskUploaded = false;

        // window areas -> cells (GL rows bottom-up)
        auto cover = [&](const std::vector<CBox>& boxes) {
            std::vector<uint8_t> m((size_t)COLS * ROWS, 0);
            for (const auto& b : boxes) {
                const double X0 = (b.x - st.monPos.x) * st.scale, X1 = (b.x + b.w - st.monPos.x) * st.scale;
                // framebuffer rows run top-down, like logical coordinates
                const double Y0 = (b.y - st.monPos.y) * st.scale, Y1 = (b.y + b.h - st.monPos.y) * st.scale;
                const int    CX0 = std::clamp((int)std::floor(X0 / CELL), 0, (int)COLS), CX1 = std::clamp((int)std::ceil(X1 / CELL), 0, (int)COLS);
                const int    CY0 = std::clamp((int)std::floor(Y0 / CELL), 0, (int)ROWS), CY1 = std::clamp((int)std::ceil(Y1 / CELL), 0, (int)ROWS);
                for (int cy = CY0; cy < CY1; ++cy)
                    std::fill_n(m.begin() + (size_t)cy * COLS + CX0, std::max(0, CX1 - CX0), (uint8_t)1);
            }
            return m;
        };

        // both pictures at cell resolution
        std::vector<uint8_t> A, B;
        if (!ensureFB(st.cells, (int)COLS, (int)ROWS, DRM_FORMAT_ABGR8888, "hyprmorph-cells"))
            return;
        blit(st.last, st.cells, true);
        if (!readPixels(st.cells, A))
            return;
        blit(st.neu, st.cells, true);
        if (!readPixels(st.cells, B))
            return;


        const auto WIN0 = cover(st.fromBoxes), WIN1 = cover(st.toBoxes);
        const auto CHANGED = changedCells(st, COLS, ROWS);
        const auto LIT0 = litCells(A, COLS, ROWS, WIN0);
        const auto LIT1 = litCells(B, COLS, ROWS, WIN1);

        // Two deck morphs at once, each paired on its own:
        //  * content: cells lit only before -> cells lit only after (lit on both stay put);
        //  * bodies: where a window exists on one side only, its whole body is
        //    pixels - an old window flies apart, a new one is built from pixels.
        std::vector<uint64_t> litSrc, litDst, bodySrc, bodyDst; // (Hilbert << 32) | cell
        for (uint32_t cy = 0; cy < ROWS; ++cy) {
            for (uint32_t cx = 0; cx < COLS; ++cx) {
                const size_t I = (size_t)cy * COLS + cx;
                if (!CHANGED.empty() && !CHANGED[I])
                    continue; // same pixels before and after: it stays where it is
                st.mask[I]     = (uint8_t)(LIT0[I] | (LIT1[I] << 1) | (WIN0[I] << 2) | (WIN1[I] << 3));
                if (LIT0[I] == LIT1[I] && WIN0[I] == WIN1[I])
                    continue;
                const uint64_t KEY = ((hilbert(cx, cy, order) & 0xFFFFFFFFull) << 32) | I;
                if (WIN0[I] != WIN1[I])
                    (WIN0[I] ? bodySrc : bodyDst).push_back(KEY);
                else
                    (LIT0[I] ? litSrc : litDst).push_back(KEY);
            }
        }
        if (litSrc.empty() && litDst.empty() && bodySrc.empty() && bodyDst.empty())
            return;

        std::mt19937                          rng(0x93);
        std::uniform_real_distribution<float> uni(0.f, 1.f);
        auto                                  cellPos = [&](uint64_t v, float& x, float& y) {
            const uint32_t C = (uint32_t)v;
            x                = (C % COLS) * CELL;
            y                = (C / COLS) * CELL;
        };
        // nothing on one side: in from / out through the nearest screen edge
        // Nothing on one side: the other desktop lies off to the side the switch
        // goes - moving right, new pixels come in from the right edge and old
        // ones leave through the left edge (and the other way round).
        auto edgePos = [&](float px, float py, float& x, float& y, bool incoming) {
            const float W = (float)FB.x, H = (float)FB.y, OUT = 4.f * CELL;
            const bool  RIGHT = incoming ? st.dir > 0 : st.dir < 0;
            x                 = RIGHT ? W + OUT + uni(rng) * 0.15f * W : -OUT - uni(rng) * 0.15f * W;
            y                 = std::clamp(py + (uni(rng) - 0.5f) * 0.25f * H, 0.f, H);
            (void)px;
        };
        auto morph = [&](std::vector<uint64_t>& src, std::vector<uint64_t>& dst) {
            radixSort(src);
            radixSort(dst);
            const size_t N = src.size(), M = dst.size(), K = std::max(N, M);
            for (size_t k = 0; k < K; ++k) {
                SInstance P{};
                if (N)
                    cellPos(src[k * N / K], P.srcX, P.srcY);
                if (M)
                    cellPos(dst[k * M / K], P.dstX, P.dstY);
                const bool FROM_EDGE = !N;
                if (!N)
                    edgePos(P.dstX, P.dstY, P.srcX, P.srcY, true);
                if (!M)
                    edgePos(P.srcX, P.srcY, P.dstX, P.dstY, false);
                P.delay = uni(rng) * STAGGER;             // the deck: own take-off time ...
                P.bow   = (0.5f + 0.5f * uni(rng)) * CURL; // ... and own bow
                if (FROM_EDGE)
                    P.bow = -P.bow; // marks "new colour all the way" for the shader
                st.instances.push_back(P);
            }
        };
        st.instances.reserve(std::max(litSrc.size(), litDst.size()) + std::max(bodySrc.size(), bodyDst.size()));
        morph(bodySrc, bodyDst);
        morph(litSrc, litDst); // drawn last: content flies over bodies
    }

    // Layout change (move, swap, resize, drop, open, close): every pixel of a
    // window flies from its spot in the window's old box to the same relative
    // spot in its new box; the area under moving windows shows the background
    // until they land. Windows that appear/disappear come from/go to the edge.
    void pairLayoutParticles(SMonitorState& st) {
        const Vector2D FB   = st.last->m_size;
        const float    CELL = (float)CELL_PX;
        const uint32_t COLS = (uint32_t)std::ceil(FB.x / CELL), ROWS = (uint32_t)std::ceil(FB.y / CELL);
        st.instances.clear();
        st.cols = COLS;
        st.rows = ROWS;
        st.mask.assign((size_t)COLS * ROWS, 0);
        st.maskUploaded = false;

        // only what actually changed moves; e.g. a title bar that keeps its place stays
        const auto CHANGED = changedCells(st, COLS, ROWS);
        auto       changed = [&](float x, float y) {
            const int CX = (int)(x / CELL), CY = (int)(y / CELL);
            if (CHANGED.empty() || CX < 0 || CY < 0 || CX >= (int)COLS || CY >= (int)ROWS)
                return true;
            return CHANGED[(size_t)CY * COLS + CX] != 0;
        };
        auto px = [&](const CBox& b) { // logical -> framebuffer px (rows top-down)
            return CBox{(b.x - st.monPos.x) * st.scale, (b.y - st.monPos.y) * st.scale, b.w * st.scale, b.h * st.scale};
        };
        auto mark = [&](const CBox& b, bool all) { // base pass shows the background here
            const int X0 = std::clamp((int)std::floor(b.x / CELL), 0, (int)COLS), X1 = std::clamp((int)std::ceil((b.x + b.w) / CELL), 0, (int)COLS);
            const int Y0 = std::clamp((int)std::floor(b.y / CELL), 0, (int)ROWS), Y1 = std::clamp((int)std::ceil((b.y + b.h) / CELL), 0, (int)ROWS);
            for (int y = Y0; y < Y1; ++y)
                for (int x = X0; x < X1; ++x)
                    if (all || CHANGED.empty() || CHANGED[(size_t)y * COLS + x])
                        st.mask[(size_t)y * COLS + x] = 4;
        };
        std::mt19937                          rng(0x93);
        std::uniform_real_distribution<float> uni(0.f, 1.f);
        auto edgePos = [&](float x, float y, float& ox, float& oy) {
            const float W = (float)FB.x, H = (float)FB.y, OUT = 4.f * CELL, CX = W * 0.5f, CY = H * 0.5f;
            const float ANG = std::atan2(y - CY, x - CX) + (uni(rng) - 0.5f) * 0.6f, DX = std::cos(ANG), DY = std::sin(ANG);
            const float TX = DX > 0 ? (W + OUT - CX) / DX : DX < 0 ? (-OUT - CX) / DX : 1e9f;
            const float TY = DY > 0 ? (H + OUT - CY) / DY : DY < 0 ? (-OUT - CY) / DY : 1e9f;
            ox = CX + DX * std::min(TX, TY);
            oy = CY + DY * std::min(TX, TY);
        };
        auto push = [&](float sx, float sy, float dx, float dy, bool newColour) {
            SInstance P{sx, sy, dx, dy, uni(rng) * STAGGER, (0.5f + 0.5f * uni(rng)) * CURL * 0.5f};
            if (newColour)
                P.bow = -P.bow; // wears the new colour all the way
            st.instances.push_back(P);
        };

        for (const auto& [FROM, TO] : st.layoutPairs) {
            const CBox A = px(FROM), B = px(TO);
            mark(A, false);
            mark(B, false);
            for (float y = 0; y < B.h; y += CELL)
                for (float x = 0; x < B.w; x += CELL)
                    if (changed((float)(B.x + x), (float)(B.y + y)))
                        push((float)(A.x + x * A.w / B.w), (float)(A.y + y * A.h / B.h), (float)(B.x + x), (float)(B.y + y), false);
        }
        for (const auto& C : st.layoutClear) {
            const CBox A = px(C);
            const int  X0 = std::clamp((int)std::floor(A.x / CELL), 0, (int)COLS), X1 = std::clamp((int)std::ceil((A.x + A.w) / CELL), 0, (int)COLS);
            const int  Y0 = std::clamp((int)std::floor(A.y / CELL), 0, (int)ROWS), Y1 = std::clamp((int)std::ceil((A.y + A.h) / CELL), 0, (int)ROWS);
            for (int y = Y0; y < Y1; ++y)
                std::fill_n(st.mask.begin() + (size_t)y * COLS + X0, std::max(0, X1 - X0), (uint8_t)4);
        }
        for (const auto& G : st.layoutGone) {
            const CBox A = px(G);
            mark(A, true); // a window that goes away leaves as a whole
            for (float y = 0; y < A.h; y += CELL)
                for (float x = 0; x < A.w; x += CELL) {
                    float ex, ey;
                    edgePos((float)(A.x + x), (float)(A.y + y), ex, ey);
                    push((float)(A.x + x), (float)(A.y + y), ex, ey, false);
                }
        }
        for (const auto& N : st.layoutNew) {
            const CBox B = px(N);
            mark(B, true); // a new window is built whole (it may not be drawn yet on this frame)
            for (float y = 0; y < B.h; y += CELL)
                for (float x = 0; x < B.w; x += CELL) {
                    float ex, ey;
                    edgePos((float)(B.x + x), (float)(B.y + y), ex, ey);
                    push(ex, ey, (float)(B.x + x), (float)(B.y + y), true);
                }
        }
    }

    void drawMorph(SMonitorState& st, const SP<Render::IFramebuffer>& target, float t) {
        if (!ensureGL() || st.mask.empty() || !st.bg)
            return;
        const auto TARGET = fbID(target);
        if (!TARGET)
            return;

        GLint prevProg = 0, prevVao = 0, prevBuf = 0, prevFb = 0, prevActive = 0, vp[4], prevTex[6] = {};
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFb);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
        for (int u = 0; u < 6; ++u) {
            glActiveTexture(GL_TEXTURE0 + u);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex[u]);
        }
        glGetIntegerv(GL_VIEWPORT, vp);
        const GLboolean BLEND = glIsEnabled(GL_BLEND), SCISSOR = glIsEnabled(GL_SCISSOR_TEST);

        glBindFramebuffer(GL_FRAMEBUFFER, *TARGET);
        glViewport(0, 0, (GLsizei)target->m_size.x, (GLsizei)target->m_size.y);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glBindVertexArray(g_gl.vao);

        // textures: 0 old, 1 new, 2 low old, 3 low new, 4 mask
        glActiveTexture(GL_TEXTURE0);
        st.last->getTexture()->bind();
        glActiveTexture(GL_TEXTURE1);
        st.neu->getTexture()->bind();
        glActiveTexture(GL_TEXTURE2);
        st.lowOld->getTexture()->bind();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glActiveTexture(GL_TEXTURE3);
        st.lowNew->getTexture()->bind();
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glActiveTexture(GL_TEXTURE4);
        if (!st.maskTex) {
            glGenTextures(1, &st.maskTex);
            st.maskUploaded = false;
        }
        glBindTexture(GL_TEXTURE_2D, st.maskTex);
        if (!st.maskUploaded) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)st.cols, (GLsizei)st.rows, 0, GL_RED, GL_UNSIGNED_BYTE, st.mask.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            st.maskUploaded = true;
        }

        glActiveTexture(GL_TEXTURE5);
        st.bg->getTexture()->bind();

        // base: backgrounds cross-fade, lit pixels taken out
        const float FADE = t * t * (3.f - 2.f * t);
        glUseProgram(g_gl.base);
        glUniform1i(g_gl.bOld, 0);
        glUniform1i(g_gl.bNew, 1);
        glUniform1i(g_gl.bLowOld, 2);
        glUniform1i(g_gl.bLowNew, 3);
        glUniform1i(g_gl.bMask, 4);
        glUniform1i(g_gl.bBg, 5);
        glUniform2f(g_gl.bFb, (float)target->m_size.x, (float)target->m_size.y);
        glUniform1f(g_gl.bCell, (float)CELL_PX);
        glUniform1f(g_gl.bFade, FADE);
        glUniform1f(g_gl.bT, t);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // particles
        if (!st.instances.empty()) {
            if (!st.vbo)
                glGenBuffers(1, &st.vbo);
            glBindBuffer(GL_ARRAY_BUFFER, st.vbo);
            if (!st.uploaded) {
                glBufferData(GL_ARRAY_BUFFER, st.instances.size() * sizeof(SInstance), st.instances.data(), GL_STATIC_DRAW);
                st.uploaded = true;
            }
            for (GLuint a = 1; a <= 3; ++a) {
                glEnableVertexAttribArray(a);
                glVertexAttribPointer(a, 2, GL_FLOAT, GL_FALSE, sizeof(SInstance), (void*)(uintptr_t)((a - 1) * 2 * sizeof(float)));
                glVertexAttribDivisor(a, 1);
            }
            glUseProgram(g_gl.part);
            glUniform1f(g_gl.pT, t);
            glUniform2f(g_gl.pFb, (float)target->m_size.x, (float)target->m_size.y);
            glUniform1f(g_gl.pCell, (float)CELL_PX);
            glUniform1f(g_gl.pStagger, STAGGER);
            glUniform1i(g_gl.pOld, 0);
            glUniform1i(g_gl.pNew, 1);
            glUniform3fv(g_gl.pTintA, 1, TINT_A);
            glUniform3fv(g_gl.pTintB, 1, TINT_B);
            glUniform1f(g_gl.pTint, COLOR_MODE == 0 ? 0.f : TINT);
            glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)st.instances.size());
            for (GLuint a = 1; a <= 3; ++a) {
                glVertexAttribDivisor(a, 0);
                glDisableVertexAttribArray(a);
            }
        }

        for (int u = 5; u >= 0; --u) {
            glActiveTexture(GL_TEXTURE0 + u);
            glBindTexture(GL_TEXTURE_2D, prevTex[u]);
        }
        glActiveTexture(prevActive);
        glUseProgram(prevProg);
        glBindVertexArray(prevVao);
        glBindBuffer(GL_ARRAY_BUFFER, prevBuf);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFb);
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        if (BLEND)
            glEnable(GL_BLEND);
        if (SCISSOR)
            glEnable(GL_SCISSOR_TEST);
    }

    // framebuffer pixel box of a logical box on this monitor (rows run top-down)
    CBox glBox(const CBox& b, const PHLMONITOR& mon, const Vector2D& fb) {
        const double S = mon->m_scale;
        return CBox{(b.x - mon->m_position.x) * S, (b.y - mon->m_position.y) * S, b.w * S, b.h * S}; // rows run top-down
    }

    // First drag frame: snapshot the window, build the particles, hide the real window.
    void beginBlob(SMonitorState& st, const PHLMONITOR& mon, const PHLWINDOW& w, const SP<Render::IFramebuffer>& cur) {
        st.blobStart = glBox(CBox{w->positionAnimation()->value(), w->sizeAnimation()->value()}, mon, cur->m_size);
        const int X0 = std::max(0, (int)st.blobStart.x), Y0 = std::max(0, (int)st.blobStart.y);
        const int X1 = std::min((int)cur->m_size.x, (int)(st.blobStart.x + st.blobStart.w)), Y1 = std::min((int)cur->m_size.y, (int)(st.blobStart.y + st.blobStart.h));
        if (X1 <= X0 || Y1 <= Y0 || !ensureFB(st.blobSnap, X1 - X0, Y1 - Y0, cur->m_drmFormat, "hyprmorph-blob"))
            return;
        st.blobStart = CBox{(double)X0, (double)Y0, (double)(X1 - X0), (double)(Y1 - Y0)};
        const auto A = fbID(cur), B = fbID(st.blobSnap);
        if (!A || !B)
            return;
        GLint prevRead = 0, prevDraw = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
        const GLboolean SCISSOR = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, *A);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, *B);
        glBlitFramebuffer(X0, Y0, X1, Y1, 0, 0, X1 - X0, Y1 - Y0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDraw);
        if (SCISSOR)
            glEnable(GL_SCISSOR_TEST);

        // the same size for every window: the longer side BLOB_HALF across, the
        // window's aspect kept (but never thinner than half of that)
        const float R   = (float)(BLOB_HALF * mon->m_scale);
        const float MX  = (float)std::max(st.blobStart.w, st.blobStart.h);
        st.blobHalf     = Vector2D{std::max(R * (float)st.blobStart.w / MX, 0.5f * R), std::max(R * (float)st.blobStart.h / MX, 0.5f * R)};
        st.blobCell     = (float)BLOB_CELL_PX;
        st.blobBlock    = MX / (2.f * R / st.blobCell); // snapshot px per blob pixel
        st.blobRadius   = R;
        st.blobCells.clear();
        std::mt19937                          rng(0x51);
        std::uniform_real_distribution<float> uni(0.f, 1.f);
        for (float y = 0; y < st.blobStart.h; y += st.blobBlock)
            for (float x = 0; x < st.blobStart.w; x += st.blobBlock) {
                const float SPEED = (1.2f + 2.4f * uni(rng)) * (uni(rng) < 0.5f ? 1.f : -1.f);
                st.blobCells.insert(st.blobCells.end(), {x, y, uni(rng) * 6.2831853f, 0.f, SPEED, uni(rng)});
                if (uni(rng) < 0.22f) // a copy that trails behind
                    st.blobCells.insert(st.blobCells.end(), {x, y, uni(rng) * 6.2831853f, 0.08f + 0.92f * std::pow(uni(rng), 1.5f), SPEED, uni(rng)});
            }
        st.blobUploaded = false;
        st.blobReady    = true;
        st.trail.assign(2, 0.f); // starts where the window was
        st.trail[0] = (float)(st.blobStart.x + st.blobStart.w * 0.5);
        st.trail[1] = (float)(st.blobStart.y + st.blobStart.h * 0.5);
        st.trailLast = Clock::now();

        w->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setValueAndWarp(0.f); // the blob stands in for it
        st.hiddenWin = w;
    }

    void endBlob(SMonitorState& st) {
        if (const auto W = st.hiddenWin.lock())
            W->alpha(Desktop::View::WINDOW_ALPHA_LAYOUT)->setValueAndWarp(1.f);
        st.hiddenWin.reset();
        st.blobReady = false;
    }

    void drawBlob(SMonitorState& st, const PHLMONITOR& mon, const SP<Render::IFramebuffer>& target) {
        if (!ensureGL() || !st.blobReady || !st.blobSnap)
            return;
        const auto TARGET = fbID(target);
        if (!TARGET)
            return;
        // cursor in GL px
        const Vector2D CUR = Pointer::mgr()->position();
        const float    CXP = (float)((CUR.x - mon->m_position.x) * mon->m_scale), CYP = (float)((CUR.y - mon->m_position.y) * mon->m_scale);

        GLint prevProg = 0, prevVao = 0, prevBuf = 0, prevFb = 0, prevActive = 0, prevTex0 = 0, vp[4];
        glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFb);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex0);
        glGetIntegerv(GL_VIEWPORT, vp);
        const GLboolean BLEND = glIsEnabled(GL_BLEND), SCISSOR = glIsEnabled(GL_SCISSOR_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);

        glBindFramebuffer(GL_FRAMEBUFFER, *TARGET);
        glViewport(0, 0, (GLsizei)target->m_size.x, (GLsizei)target->m_size.y);
        glBindVertexArray(g_gl.vao);

        const float SECS = std::chrono::duration<float>(Clock::now() - st.dragStart).count();

        if (!st.blobVbo)
            glGenBuffers(1, &st.blobVbo);
        glBindBuffer(GL_ARRAY_BUFFER, st.blobVbo);
        if (!st.blobUploaded) {
            glBufferData(GL_ARRAY_BUFFER, st.blobCells.size() * sizeof(float), st.blobCells.data(), GL_STATIC_DRAW);
            st.blobUploaded = true;
        }
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
        glVertexAttribDivisor(1, 1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(2 * sizeof(float)));
        glVertexAttribDivisor(2, 1);
        glUseProgram(g_gl.blob);
        glUniform2f(g_gl.oHalf, st.blobHalf.x, st.blobHalf.y);
        glUniform1f(g_gl.oBlock, st.blobBlock);
        glUniform2f(g_gl.oBlockUv, st.blobBlock / (float)st.blobStart.w, st.blobBlock / (float)st.blobStart.h);
        glUniform2f(g_gl.oOrigin, (float)st.blobStart.x, (float)st.blobStart.y);
        glUniform2f(g_gl.oSnap, (float)st.blobStart.w, (float)st.blobStart.h);
        glUniform2f(g_gl.oCursor, CXP, CYP);
        // cursor path, one sample per ~8 ms, ~0.4 s long
        if (std::chrono::duration<float>(Clock::now() - st.trailLast).count() > 0.008f || st.trail.size() < 4) {
            st.trail.insert(st.trail.begin(), {CXP, CYP});
            if (st.trail.size() > 40) // ~0.16 s of path
                st.trail.resize(40);
            st.trailLast = Clock::now();
        } else {
            st.trail[0] = CXP;
            st.trail[1] = CYP;
        }
        glUniform2fv(g_gl.oTrail, (GLsizei)(st.trail.size() / 2), st.trail.data());
        glUniform1i(g_gl.oTrailCount, (GLint)(st.trail.size() / 2));

        glUniform2f(g_gl.oFb, (float)target->m_size.x, (float)target->m_size.y);
        glUniform1f(g_gl.oCell, st.blobCell);
        glUniform1f(g_gl.oTime, SECS);
        glUniform1f(g_gl.oAmount, std::min(1.5f, SECS / BLOB_RAMP));
        glUniform3fv(g_gl.oTintA, 1, TINT_A);
        glUniform3fv(g_gl.oTintB, 1, TINT_B);
        glUniform1f(g_gl.oTint, COLOR_MODE == 0 ? 0.f : TINT);
        glUniform1f(g_gl.oWobble, BLOB_WOBBLE);
        glUniform1f(g_gl.oTrailLen, BLOB_TRAIL);
        glUniform1i(g_gl.oTex, 0);
        glActiveTexture(GL_TEXTURE0);
        st.blobSnap->getTexture()->bind();
        GLint minF = 0, magF = 0;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minF);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magF);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)(st.blobCells.size() / 6));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, minF);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, magF);
        glVertexAttribDivisor(1, 0);
        glVertexAttribDivisor(2, 0);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);


        glBindTexture(GL_TEXTURE_2D, prevTex0);
        glActiveTexture(prevActive);
        glUseProgram(prevProg);
        glBindVertexArray(prevVao);
        glBindBuffer(GL_ARRAY_BUFFER, prevBuf);
        glBindFramebuffer(GL_FRAMEBUFFER, prevFb);
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        if (BLEND)
            glEnable(GL_BLEND);
        if (SCISSOR)
            glEnable(GL_SCISSOR_TEST);
    }

    // ------------------------------------------------------- pass elements ---

    bool ownMonitorFrame() {
        const auto& rd = g_pHyprRenderer->m_renderData;
        if (g_pHyprRenderer->m_bRenderingSnapshot || rd.projectionType != Render::RPT_MONITOR)
            return false;
        if (rd.mainFB && rd.currentFB != rd.mainFB)
            return false;
        return !(rd.renderModif.enabled && !rd.renderModif.modifs.empty());
    }

    class CElement : public IPassElement {
      public:
        enum eKind : uint8_t {
            BG,
            POST
        };
        explicit CElement(eKind k) : m_kind(k) {}

        std::vector<UP<IPassElement>> draw() override {
            const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
            const auto CUR = g_pHyprRenderer->m_renderData.currentFB;
            if (!MON || !CUR || CUR->m_size.x <= 0)
                return {};
            auto& st = g_mon[MON->m_id];

            if (m_kind == BG) {
                if ((st.morphing || st.dragWin.lock()) && ensureFB(st.bg, CUR, "hyprmorph-bg"))
                    blit(CUR, st.bg);
                return {};
            }

            if (!st.morphing) {
                // keep a complete picture of the workspace: a fresh buffer
                // needs one full redraw before it holds everything
                const bool FRESH = !st.last || st.last->m_size != CUR->m_size || st.last->m_drmFormat != CUR->m_drmFormat;
                if (!ensureFB(st.last, CUR, "hyprmorph-last"))
                    return {};
                if (FRESH)
                    g_pHyprRenderer->damageMonitor(MON);
                if (const auto DW = st.dragWin.lock()) {
                    if (!st.blobReady)
                        beginBlob(st, MON, DW, CUR);
                    drawBlob(st, MON, CUR);
                }
                if (st.dragFirst)
                    st.dragFirst = false; // the window was still drawn: keep the picture from before the drag
                else if (st.changingFrames == 0) // while the layout is still moving keep the picture from before
                    blitDamage(MON, CUR, st.last); // after the blob: a drop morphs from what was on screen
                return {};
            }

            if (!st.last || st.last->m_size != CUR->m_size || !ensureFB(st.neu, CUR, "hyprmorph-new")) {
                st.morphing = false;
                return {};
            }
            if (!st.paired && st.restart && st.shown && st.shown->m_size == st.last->m_size)
                blit(st.shown, st.last); // switched mid-morph: carry on from what was on screen
            st.restart = false;
            blit(CUR, st.neu); // the new workspace as it is right now (the deck's live destination)

            const int LW = std::max(1, (int)(CUR->m_size.x / (CELL_PX * LOW_DIV))), LH = std::max(1, (int)(CUR->m_size.y / (CELL_PX * LOW_DIV)));
            if (!ensureFB(st.lowOld, LW, LH, DRM_FORMAT_ABGR8888, "hyprmorph-low-old") || !ensureFB(st.lowNew, LW, LH, DRM_FORMAT_ABGR8888, "hyprmorph-low-new")) {
                st.morphing = false;
                return {};
            }
            blit(st.neu, st.lowNew, true);

            if (!st.paired) {
                blit(st.last, st.lowOld, true);
                pairParticles(st);
                st.paired = true;
                st.start  = Clock::now(); // the run starts once the particles exist
            }

            const float T = std::chrono::duration<float>(Clock::now() - st.start).count() / MORPH_SECONDS;
            if (T >= 1.f) {
                st.morphing = false;
                st.layoutClear.clear();
                if (st.dragWin.lock() && st.blobReady)
                    drawBlob(st, MON, CUR);
                return {}; // the frame already holds the new workspace
            }
            drawMorph(st, CUR, T);
            if (ensureFB(st.shown, CUR, "hyprmorph-shown"))
                blit(CUR, st.shown);
            if (st.dragWin.lock() && st.blobReady)
                drawBlob(st, MON, CUR); // the blob floats over the morph (not part of `shown`)
            return {};
        }

        bool needsLiveBlur() override {
            return false;
        }
        bool needsPrecomputeBlur() override {
            return false;
        }
        const char* passName() override {
            return "CHyprmorphElement";
        }
        ePassElementType type() override {
            return EK_CUSTOM;
        }
        std::optional<CBox> boundingBox() override {
            return std::nullopt;
        }
        bool disableSimplification() override {
            return true;
        }
        bool undiscardable() override {
            return true;
        }

      private:
        eKind m_kind;
    };

    // -------------------------------------------------------------- events ---

    std::vector<CBox> windowBoxes(const PHLWORKSPACE& ws) {
        std::vector<CBox> out;
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w || !w->m_isMapped || w->isHidden() || w->m_workspace != ws)
                continue;
            out.emplace_back(CBox{w->positionAnimation()->goal(), w->sizeAnimation()->goal()}.expand(EDGE_LOGICAL));
        }
        return out;
    }

    void startMorphBoxes(const PHLMONITOR& mon, std::vector<CBox> fromBoxes, std::vector<CBox> toBoxes);

    void startMorph(const PHLMONITOR& mon, const PHLWORKSPACE& from, const PHLWORKSPACE& to) {
        g_mon[mon->m_id].layoutMode = false;
        g_mon[mon->m_id].dir        = to->m_id >= from->m_id ? 1.f : -1.f;
        startMorphBoxes(mon, windowBoxes(from), windowBoxes(to));
    }

    bool sameBox(const CBox& a, const CBox& b) {
        return std::abs(a.x - b.x) <= 1 && std::abs(a.y - b.y) <= 1 && std::abs(a.w - b.w) <= 1 && std::abs(a.h - b.h) <= 1;
    }

    const SWinBox* findWin(const std::vector<SWinBox>& v, uintptr_t id) {
        for (const auto& w : v)
            if (w.id == id)
                return &w;
        return nullptr;
    }

    bool sameBoxes(const std::vector<SWinBox>& a, const std::vector<SWinBox>& b) {
        if (a.size() != b.size())
            return false;
        for (const auto& w : a) {
            const auto O = findWin(b, w.id);
            if (!O || !sameBox(O->box, w.box))
                return false;
        }
        return true;
    }

    std::vector<SWinBox> windowBoxesId(const PHLWORKSPACE& ws) {
        std::vector<SWinBox> out;
        for (const auto& w : Desktop::windowState()->windows()) {
            if (!w || !w->m_isMapped || w->isHidden() || w->m_workspace != ws)
                continue;
            out.push_back({(uintptr_t)w.get(), CBox{w->positionAnimation()->goal(), w->sizeAnimation()->goal()}.expand(EDGE_LOGICAL)});
        }
        return out;
    }

    // A layout change morphs window by window; windows that stayed put are left alone.
    void startLayoutMorph(const PHLMONITOR& mon, const std::vector<SWinBox>& before, const std::vector<SWinBox>& after) {
        if (!MORPH_WINDOWS)
            return;
        auto& st = g_mon[mon->m_id];
        st.layoutPairs.clear();
        st.layoutGone.clear();
        st.layoutNew.clear();
        std::vector<CBox> from, to;
        for (const auto& b : before) {
            const auto A = findWin(after, b.id);
            if (A && sameBox(A->box, b.box))
                continue;
            if (A)
                st.layoutPairs.emplace_back(b.box, A->box);
            else
                st.layoutGone.push_back(b.box);
            from.push_back(b.box);
        }
        for (const auto& a : after)
            if (!findWin(before, a.id)) {
                st.layoutNew.push_back(a.box);
                to.push_back(a.box);
            } else if (!sameBox(findWin(before, a.id)->box, a.box))
                to.push_back(a.box);
        if (from.empty() && to.empty())
            return;
        st.layoutMode = true;
        startMorphBoxes(mon, std::move(from), std::move(to));
    }

    // The window being moved/resized with the mouse, if it is on this monitor.
    PHLWINDOW draggedWindow(const PHLMONITOR& mon) {
        if (!g_layoutManager)
            return nullptr;
        const auto& DC = g_layoutManager->dragController();
        if (!DRAG_BLOB || !DC || DC->mode() != MBIND_MOVE) // (the threshold flag stays false with drag_threshold = 0)
            return nullptr;
        const auto T = DC->target();
        const auto W = T ? T->window() : nullptr;
        if (!W || !W->m_workspace || W->m_workspace != mon->m_activeWorkspace)
            return nullptr;
        return W;
    }

    // Geometry watcher: open, close, fullscreen, layout resizes, a dropped
    // window snapping into place - any change of the visible windows morphs from
    // the previous layout to the new one. While a window is dragged it is a blob
    // and changes are only remembered; the drop then morphs like any change.
    void watchGeometry(const PHLMONITOR& mon) {
        auto&      st = g_mon[mon->m_id];
        const auto WS = mon->m_activeWorkspace;
        if (!WS || WS->m_isSpecialWorkspace)
            return;
        const auto BOXES = windowBoxesId(WS);
        const auto DW    = draggedWindow(mon);
        if (DW && !st.dragWin.lock()) {
            st.dragWin   = DW;
            st.dragStart = Clock::now();
            st.blobReady = false;
            st.dragFirst = true;
            st.dragLeft.reset();
        } else if (!DW && st.dragWin.lock()) {
            st.dragWin.reset();
            endBlob(st); // the real window comes back; the change below morphs the blob into it
        }

        if (WS->m_id != st.cachedWs) {
            st.cachedWs    = WS->m_id; // workspace switches morph through their own event
            st.cachedBoxes = BOXES;
            return;
        }
        if (DW) {
            // the other windows rearranging (closing the gap at drag start, making
            // room while dragging) morph like any layout change; the dragged
            // window itself is the blob. The first drag frame only starts the
            // blob (the window is hidden from the next frame on).
            if (!st.dragFirst) {
                std::vector<SWinBox> before, after;
                for (const auto& b : st.cachedBoxes)
                    if (b.id != (uintptr_t)DW.get())
                        before.push_back(b);
                for (const auto& b : BOXES)
                    if (b.id != (uintptr_t)DW.get())
                        after.push_back(b);
                if (!sameBoxes(before, after)) {
                    st.layoutClear.clear();
                    if (st.dragLeft) // the gap the window left: background until the others fill it
                        st.layoutClear.push_back(*st.dragLeft);
                    st.dragLeft.reset();
                    startLayoutMorph(mon, before, after);
                }
            }
            // while dragging remember the layout with the blob in place of the window
            auto           boxes = BOXES;
            const Vector2D C     = Pointer::mgr()->position();
            const double   R     = st.blobRadius / mon->m_scale * 1.15;
            for (auto& b : boxes)
                if (b.id == (uintptr_t)DW.get())
                    b.box = CBox{C.x - R, C.y - R, 2 * R, 2 * R}; // the blob stands where the window will fly from
            if (st.dragFirst) {
                // keep the layout from before the drag; the next frame compares against it
                if (const auto OLD = findWin(st.cachedBoxes, (uintptr_t)DW.get()))
                    st.dragLeft = OLD->box;
                return;
            }
            st.cachedBoxes = std::move(boxes);
            return;
        }
        // Morph once the layout settles: something changing frame after frame
        // (an undetected drag, an animated resize, an app resizing itself) must
        // not start a morph every frame.
        if (!sameBoxes(st.cachedBoxes, BOXES)) {
            st.layoutClear.clear();
            if (st.changingFrames++ == 0) {
                // a one-off change (open, close, fullscreen...): morph right away
                st.stableBoxes = st.cachedBoxes;
                startLayoutMorph(mon, st.stableBoxes, BOXES);
            } else if (st.changingFrames == 2)
                st.morphing = false; // it keeps changing: wait for it to settle (`last` stays frozen)
            st.cachedBoxes = BOXES;
            return;
        }
        if (st.changingFrames > 0) {
            const bool CONTINUOUS = st.changingFrames >= 2;
            st.changingFrames     = 0;
            if (CONTINUOUS && !sameBoxes(st.stableBoxes, BOXES))
                startLayoutMorph(mon, st.stableBoxes, BOXES);
        }
    }

    void startMorphBoxes(const PHLMONITOR& mon, std::vector<CBox> fromBoxes, std::vector<CBox> toBoxes) {
        auto& st = g_mon[mon->m_id];
        if (!st.last || st.last->m_size.x <= 0)
            return; // nothing captured yet
        st.restart   = st.morphing; // switched again mid-morph: start from the latest picture
        st.fromBoxes = std::move(fromBoxes);
        st.toBoxes   = std::move(toBoxes);
        if (st.fromBoxes.empty() && st.toBoxes.empty())
            return;
        st.scale    = mon->m_scale;
        st.monPos   = mon->m_position;
        st.paired   = false;
        st.uploaded = false;
        st.start    = Clock::now();
        st.morphing = true;
        g_pHyprRenderer->damageMonitor(mon);
    }

    void onWorkspaceActive(PHLWORKSPACE ws) {
        if (!ws || ws->m_isSpecialWorkspace)
            return;
        const auto MON = ws->m_monitor.lock();
        if (!MON)
            return;
        auto&      st   = g_mon[MON->m_id];
        const auto PREV = st.lastWorkspace.lock();
        st.lastWorkspace = ws;
        if (PREV && PREV != ws && !PREV->m_isSpecialWorkspace && PREV->m_monitor.lock() == MON)
            if (MORPH_WORKSPACES)
                startMorph(MON, PREV, ws);
        st.cachedWs    = ws->m_id;
        st.cachedBoxes = windowBoxesId(ws);
    }

    // ----------------------------------------------------------- settings ---
    // ~/.config/hyprmorph/settings.conf, `key = value` lines (the hyprmorph-settings
    // app writes it). Re-read when it or the theme colours change; applied only
    // between morphs so buffers sized by the pixel size stay consistent.
    std::string homePath(const char* rel) {
        const char* H = getenv("HOME");
        return std::string(H ? H : "") + rel;
    }

    bool parseHex(const std::string& v, float out[3]) {
        std::string h = v;
        h.erase(std::remove_if(h.begin(), h.end(), [](char c) { return c == '#' || c == '"' || c == ' '; }), h.end());
        if (h.size() < 6)
            return false;
        try {
            for (int i = 0; i < 3; ++i)
                out[i] = std::stoi(h.substr(i * 2, 2), nullptr, 16) / 255.f;
        } catch (...) { return false; }
        return true;
    }

    std::string themeColor(const std::string& json, const char* key) {
        const auto K = json.find(std::string("\"") + key + "\"");
        if (K == std::string::npos)
            return "";
        const auto H = json.find('#', K);
        return H == std::string::npos ? "" : json.substr(H, 7);
    }

    std::filesystem::file_time_type g_setTime{}, g_themeTime{};
    Clock::time_point               g_lastCheck{};

    void loadSettings() {
        if (std::chrono::duration<float>(Clock::now() - g_lastCheck).count() < 0.5f)
            return;
        g_lastCheck = Clock::now();
        for (const auto& [id, st] : g_mon)
            if (st.morphing || st.blobReady)
                return; // not mid-morph
        std::error_code ec;
        const auto      SET = homePath("/.config/hyprmorph/settings.conf"), THEME = homePath("/.config/ml4w/colors/colors.json");
        const auto      ST = std::filesystem::last_write_time(SET, ec);
        const auto      TT = std::filesystem::last_write_time(THEME, ec);
        if (ST == g_setTime && TT == g_themeTime)
            return;
        g_setTime   = ST;
        g_themeTime = TT;

        std::unordered_map<std::string, std::string> kv;
        std::ifstream                                 f(SET);
        for (std::string line; std::getline(f, line);) {
            if (const auto C = line.find('#'); C != std::string::npos && line.find('=') > C)
                continue;
            const auto EQ = line.find('=');
            if (EQ == std::string::npos)
                continue;
            auto trim = [](std::string x) {
                x.erase(0, x.find_first_not_of(" \t"));
                x.erase(x.find_last_not_of(" \t\r") + 1);
                return x;
            };
            kv[trim(line.substr(0, EQ))] = trim(line.substr(EQ + 1));
        }
        auto num = [&](const char* k, float def, float lo, float hi) {
            try {
                return kv.contains(k) ? std::clamp(std::stof(kv[k]), lo, hi) : def;
            } catch (...) { return def; }
        };
        auto flag = [&](const char* k, bool def) { return kv.contains(k) ? (kv[k] == "true" || kv[k] == "1" || kv[k] == "yes") : def; };

        MORPH_WORKSPACES = flag("morph_workspaces", true);
        MORPH_WINDOWS    = flag("morph_windows", true);
        DRAG_BLOB        = flag("drag_blob", true);
        MORPH_SECONDS    = num("duration", 0.3f, 0.1f, 2.f);
        STAGGER          = num("stagger", 0.35f, 0.f, 0.85f);
        CURL             = num("curl", 0.18f, 0.f, 0.8f);
        CELL_PX          = (int)num("pixel_size", 3.f, 2.f, 12.f);
        BLOB_CELL_PX     = CELL_PX;
        LIT_CONTRAST     = num("detail", 0.10f, 0.03f, 0.4f);
        BLOB_HALF        = num("blob_size", 160.f, 60.f, 400.f);
        BLOB_TRAIL       = num("blob_trail", 1.6f, 0.f, 4.f);
        BLOB_WOBBLE      = num("blob_wobble", 1.f, 0.f, 3.f);
        TINT             = num("color_strength", 0.7f, 0.f, 1.f);
        const std::string MODE = kv.contains("color_mode") ? kv["color_mode"] : "window";
        COLOR_MODE             = MODE == "theme" ? 1 : MODE == "custom" ? 2 : 0;
        if (COLOR_MODE == 1) {
            std::ifstream     tf(THEME);
            const std::string J((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
            if (!parseHex(themeColor(J, "primary"), TINT_A) || !parseHex(themeColor(J, "tertiary"), TINT_B))
                COLOR_MODE = 0;
        } else if (COLOR_MODE == 2) {
            if (!parseHex(kv["custom_color"], TINT_A))
                COLOR_MODE = 0;
            if (!kv.contains("custom_color2") || !parseHex(kv["custom_color2"], TINT_B))
                std::copy(TINT_A, TINT_A + 3, TINT_B);
        }
    }

    void onRenderStage(eRenderStage stage) {
        if (stage == RENDER_POST) {
            // keep frames coming (with full damage) while a morph runs
            for (const auto& mon : State::monitorState()->monitors())
                if (mon && g_mon.contains(mon->m_id) && (g_mon[mon->m_id].morphing || g_mon[mon->m_id].dragWin.lock()))
                    g_pHyprRenderer->damageMonitor(mon);
            return;
        }
        if (stage != RENDER_PRE_WINDOWS && stage != RENDER_POST_WINDOWS)
            return;
        if (stage == RENDER_PRE_WINDOWS)
            loadSettings();
        if (!ownMonitorFrame())
            return;
        const auto MON = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!MON)
            return;
        if (stage == RENDER_PRE_WINDOWS) {
            watchGeometry(MON);
            if (g_mon[MON->m_id].morphing || g_mon[MON->m_id].dragWin.lock())
                g_pHyprRenderer->m_renderPass.add(makeUnique<CElement>(CElement::BG));
        } else
            g_pHyprRenderer->m_renderPass.add(makeUnique<CElement>(CElement::POST));
    }
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    for (const auto& MON : State::monitorState()->monitors())
        if (MON && MON->m_activeWorkspace)
            g_mon[MON->m_id].lastWorkspace = MON->m_activeWorkspace;

    auto& ev = Event::bus()->m_events;
    g_listeners.push_back(ev.monitor.added.listen([](PHLMONITOR MON) {
        if (MON && MON->m_activeWorkspace)
            g_mon[MON->m_id].lastWorkspace = MON->m_activeWorkspace;
    }));
    g_listeners.push_back(ev.workspace.active.listen([](PHLWORKSPACE ws) { onWorkspaceActive(ws); }));
    g_listeners.push_back(ev.render.stage.listen([](eRenderStage s) { onRenderStage(s); }));

    return {"hyprmorph", "Workspace switch as the arctis-hub OLED page morph", "guizzan", "0.5"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_listeners.clear();
    for (auto& [id, st] : g_mon)
        endBlob(st); // never leave a window hidden
    for (auto& [id, st] : g_mon) {
        if (st.vbo)
            glDeleteBuffers(1, &st.vbo);
        if (st.maskTex)
            glDeleteTextures(1, &st.maskTex);
        if (st.blobVbo)
            glDeleteBuffers(1, &st.blobVbo);
    }
    g_mon.clear();
    if (g_gl.part)
        glDeleteProgram(g_gl.part);
    if (g_gl.base)
        glDeleteProgram(g_gl.base);
    if (g_gl.blob)
        glDeleteProgram(g_gl.blob);
    if (g_gl.diff)
        glDeleteProgram(g_gl.diff);
    if (g_gl.vao)
        glDeleteVertexArrays(1, &g_gl.vao);
    if (g_gl.quad)
        glDeleteBuffers(1, &g_gl.quad);
    g_gl = {};
}
