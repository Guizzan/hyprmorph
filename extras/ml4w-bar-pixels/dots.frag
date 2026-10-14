#version 440
// Dot-matrix background for the ML4W bar / dock (hyprmorph theme).
// A fine grid of round, anti-aliased dots fills the pill; the dots shrink away
// toward its edge so the shape dissolves softly. A slow wave of light flows
// across them, a few twinkle in the accent colours, and around the cursor they
// light up and bulge outward like under a lens. A ripple ring can run through.
// Compile: qsb --glsl "100es,120,150" --hlsl 50 --msl 12 -o dots.frag.qsb dots.frag

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 size;      // item size, logical px
    vec2 cursor;    // logical px (smoothed)
    float hover;    // 0..1, cursor presence (smoothed)
    float time;     // seconds
    float radius;   // pill corner radius
    float spacing;  // grid pitch, logical px
    float react;    // cursor effect strength 0..3
    float ripple;   // ripple ring radius (px), < 0 = none
    float rippleA;  // ripple strength 0..1
    vec2 rippleC;   // ripple centre
    vec4 colorBg;
    vec4 colorA;
    vec4 colorB;
};

float hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float sdRound(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    vec2 p = qt_TexCoord0 * size;
    float R = 46.0;

    // the lens: dots near the cursor are pushed outward (inverse-mapped)
    vec2 q = p - cursor;
    float dq = length(q);
    float lens = hover * react * exp(-dq * dq / (R * R));
    vec2 ps = p - (dq > 0.001 ? q / dq : vec2(0.0)) * lens * 7.0;

    vec2 cell = floor(ps / spacing);
    vec2 c = (cell + 0.5) * spacing;
    float h = hash(cell);

    // where this dot sits in the pill: dots shrink away toward the edge
    vec2 hs = size * 0.5;
    float sd = sdRound(c - hs, hs, min(radius, min(hs.x, hs.y)));
    float inside = smoothstep(0.0, spacing * 3.0, -sd);
    if (inside <= 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    // slow light wave flowing along, plus each dot's own breathing
    float wave = 0.5 + 0.5 * sin(c.x * 0.018 - time * 0.9 + sin(c.y * 0.05 + time * 0.3) * 1.2);
    float breathe = 0.85 + 0.15 * sin(time * (0.6 + h * 1.4) + h * 40.0);

    // cursor spotlight and the ripple ring
    float spot = hover * react * exp(-length(c - cursor) * length(c - cursor) / (R * R * 0.8));
    float ring = 0.0;
    if (ripple >= 0.0) {
        float k = (length(c - rippleC) - ripple) / 10.0;
        ring = rippleA * exp(-k * k);
    }
    // a few dots twinkle in the accent colours
    float tw = h > 0.94 ? pow(0.5 + 0.5 * sin(time * (1.5 + h * 3.0) + h * 70.0), 6.0) : 0.0;

    float glow = clamp(spot + ring + tw, 0.0, 1.0);
    float r = spacing * (0.30 + 0.07 * wave) * breathe * inside * (1.0 + 0.45 * glow);
    float d = length(ps - c);
    float a = smoothstep(r + 0.75, r - 0.75, d);

    vec3 base = colorBg.rgb * (1.0 + 0.9 * wave) + vec3(0.035);
    vec3 accent = mix(colorA.rgb, colorB.rgb, step(0.5, fract(h * 7.0)));
    vec3 col = mix(base, accent * 1.15, glow);
    float alpha = a * mix(0.88, 1.0, glow) * inside;

    fragColor = vec4(col * alpha, alpha) * qt_Opacity;
}
