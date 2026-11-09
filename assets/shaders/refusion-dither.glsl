// SPDX-License-Identifier: GPL-3.0-or-later
#pragma parameter DITHER_STRENGTH "Dither blend strength" 1.0 0.0 1.0 0.05

#ifdef VERTEX
attribute vec4 VertexCoord;
uniform mat4 MVPMatrix;
void main() {
    gl_Position = MVPMatrix * VertexCoord;
}
#elif defined(FRAGMENT)
uniform sampler2D Texture;
uniform vec2 TextureSize;
uniform float DITHER_STRENGTH;

// This pass runs 1:1 at the native resolution. Work on whole two-pixel
// periods so both phases of a confirmed pattern receive the same mean.
struct Tile { vec3 a; vec3 b; vec3 c; vec3 d; };
vec3 pixel(vec2 p) {
    return texture2D(Texture, (p + 0.5) / TextureSize).rgb;
}
bool inBounds(vec2 p) {
    return all(greaterThanEqual(p, vec2(0.0))) && all(lessThan(p + 1.0, TextureSize));
}
bool matches(vec2 p, Tile ref) {
    return inBounds(p) && pixel(p) == ref.a && pixel(p + vec2(1, 0)) == ref.b
           && pixel(p + vec2(0, 1)) == ref.c && pixel(p + 1.0) == ref.d;
}

// Count matching two-pixel periods, stopping at the first boundary. Keep
// the span: when testing the adjacent band we never resample the first one.
int run(vec2 p, vec2 stepPixels, Tile ref, int limit) {
    int length = 0;
    for (int i = 1; i <= 5; ++i) {
        if (i > limit || !matches(p + float(i) * stepPixels, ref)) break;
        ++length;
    }
    return length;
}

bool supportedBand(vec2 p, vec2 along, Tile ref, int left, int right, int width) {
    if (!matches(p, ref)) return false;
    int need = width - 1;
    need -= run(p, along, ref, right < need ? right : need);
    return need == 0 || (left >= need && run(p, -along, ref, need) == need);
}

void main() {
    vec2 pos = floor(gl_FragCoord.xy);
    vec2 phase = mod(pos, 2.0);
    vec2 p = pos - phase;
    if (!inBounds(p)) {
        gl_FragColor = vec4(pixel(pos), 1.0);
        return;
    }

    Tile t = Tile(pixel(p), pixel(p + vec2(1, 0)), pixel(p + vec2(0, 1)), pixel(p + 1.0));
    vec3 src = phase.y == 0.0 ? (phase.x == 0.0 ? t.a : t.b) : (phase.x == 0.0 ? t.c : t.d);
    gl_FragColor = vec4(src, 1.0);

    vec3 other = t.b != t.a ? t.b : (t.c != t.a ? t.c : t.d);
    if (other == t.a || (t.c != t.a && t.c != other) || (t.d != t.a && t.d != other)) return;

    bool horizontal = t.a == t.b && t.c == t.d;
    bool vertical = t.a == t.c && t.b == t.d;
    bool checker = t.a == t.d && t.b == t.c;
    vec2 along = horizontal ? vec2(0, 2) : vec2(2, 0);
    vec2 across = along.yx;
    int width = checker ? 3 : 4;
    // A thin band qualifies only if both lanes alternate for six periods.
    // A single dotted line or a solid step between colors fails that test.
    bool bothLanes = horizontal || vertical || checker;
    int limit = bothLanes ? 5 : width - 1;
    int right = run(p, along, t, limit);
    // With only one neighboring band, support may lie entirely on either
    // side. Explore both sides unless the long-band test is already proven.
    bool longBand = bothLanes && right == 5;
    int left = longBand ? 0 : run(p, -along, t, limit);
    int span = left + 1 + right;
    if ((bothLanes && span >= 6) ||
        (span >= width && (supportedBand(p + across, along, t, left, right, width) ||
                           supportedBand(p - across, along, t, left, right, width)))) {
        vec3 mean = (t.a + t.b + t.c + t.d) * 0.25;
        gl_FragColor = vec4(mix(src, mean, DITHER_STRENGTH), 1.0);
    }
}
#endif
