#version 440
// Pixel morph for the ML4W bar / dock (hyprmorph theme): the pill's live
// picture is cut into a grid of cells that fly in from (t 0 -> 1) or out to
// (t 1 -> 0) the screen edge, each with its own take-off delay and a slight
// sideways arc, eased in-out cubic and landing on the pixel grid - the same
// motion as the window morphs. At t = 1 every cell is home: exactly the pill.
// Compile: qsb --glsl "100es,120,150" --hlsl 50 --msl 12 -o morph.frag.qsb morph.frag

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 area;      // this effect's size, logical px
    vec2 pillPos;   // the pill's home position inside the effect
    vec2 pillSize;
    float t;        // 0 = gone, 1 = assembled
    float cell;     // cell size, logical px
    float travel;   // how far cells fly (px)
    float dir;      // +1: they come from / leave downward, -1: upward
    float stagger;  // fraction of the run over which cells set off
};
layout(binding = 1) uniform sampler2D src;

float hash(vec2 p) {
    p = fract(p * vec2(127.13, 311.71));
    p += dot(p, p + 19.19);
    return fract(p.x * p.y);
}

float ease(float x) {
    return x < 0.5 ? 4.0 * x * x * x : 1.0 - pow(-2.0 * x + 2.0, 3.0) / 2.0;
}

void main() {
    vec2 p = qt_TexCoord0 * area;
    vec2 lp = p - pillPos;
    int cols = int(ceil(pillSize.x / cell));
    int rows = int(ceil(pillSize.y / cell));
    int cx = int(floor(lp.x / cell));
    vec4 outc = vec4(0.0);
    bool found = false;

    for (int dc = -3; dc <= 3 && !found; dc++) {
        int c = cx + dc;
        if (c < 0 || c >= cols)
            continue;
        for (int r = 0; r < rows; r++) {
            vec2 id = vec2(float(c), float(r));
            float h1 = hash(id), h2 = hash(id + 17.3), h3 = hash(id + 41.7);
            // rows nearest the edge they come from move a little earlier
            float edgeRow = dir > 0.0 ? float(rows - 1 - r) / max(float(rows - 1), 1.0) : float(r) / max(float(rows - 1), 1.0);
            float delay = clamp(h1 * 0.8 + edgeRow * 0.2, 0.0, 1.0) * stagger;
            float local = clamp((t - delay) / (1.0 - stagger), 0.0, 1.0);
            float e = ease(local);
            vec2 home = id * cell;
            vec2 start = home + vec2((h2 - 0.5) * cell * 3.0, dir * travel * (0.65 + 0.35 * h3));
            vec2 pos = mix(start, home, e);
            pos.x += sin(3.14159265 * e) * (h3 - 0.5) * cell * 2.0;
            pos = floor(pos / cell + 0.5) * cell;
            vec2 d = lp - pos;
            if (d.x >= 0.0 && d.y >= 0.0 && d.x < cell && d.y < cell) {
                vec4 s = texture(src, (home + d) / pillSize);
                outc = s * smoothstep(0.0, 0.18, local);
                found = true;
                break;
            }
        }
    }
    fragColor = outc * qt_Opacity;
}
