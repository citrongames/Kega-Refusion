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

// A five-pixel window resolves both two- and four-pixel periods. For
// low-contrast ramps the density may change from row to row, so measure
// reversals and local coverage instead of requiring identical tiles.
bool nearColor(vec3 a, vec3 b) {
    return all(lessThanEqual(abs(b - a), vec3(0.30)));
}
bool sparseRepeat(vec2 p, vec2 offset, vec3 dotColor, vec3 background) {
    vec2 q = p + offset;
    return all(greaterThanEqual(q, vec2(0))) && all(lessThan(q, TextureSize))
        && pixel(q) == dotColor && pixel(q - sign(offset)) == background && pixel(q + sign(offset)) == background;
}
bool repeatedAlong(vec2 p, vec2 axis, vec3 ink, vec3 paper) {
    if (pixel(p + axis) != paper || pixel(p - axis) != paper) return false;
    // Keep these fixed offsets visible to older GLSL compilers.
    return sparseRepeat(p, 2.0 * axis, ink, paper) || sparseRepeat(p, -2.0 * axis, ink, paper)
        || sparseRepeat(p, 4.0 * axis, ink, paper) || sparseRepeat(p, -4.0 * axis, ink, paper)
        || sparseRepeat(p, 8.0 * axis, ink, paper) || sparseRepeat(p, -8.0 * axis, ink, paper);
}
float reversals(vec4 differences) {
    return (dot(abs(differences), vec4(1.0)) - abs(dot(differences, vec4(1.0)))) * 0.5;
}
bool blendRamp(vec2 pos, vec3 src, out vec3 result) {
    vec2 offset = vec2(1, 0);
    vec3 other = pixel(pos + offset);
    if (other == src) { offset = vec2(-1, 0); other = pixel(pos + offset); }
    if (other == src) { offset = vec2(0, 1); other = pixel(pos + offset); }
    if (other == src) { offset = vec2(0,-1); other = pixel(pos + offset); }
    if (other == src) { offset = vec2(1, 1); other = pixel(pos + offset); }
    if (other == src) { offset = vec2(-1,-1); other = pixel(pos + offset); }
    if (other == src) { offset = vec2(-1, 1); other = pixel(pos + offset); }
    if (other == src) { offset = vec2(1,-1); other = pixel(pos + offset); }
    if (other == src) { result = src; return true; }
    if (!nearColor(src, other)) return false;

    float count = 0.0, population = 0.0;
    float coverage = 0.0, totalWeight = 0.0, xTurns = 0.0;
    // Count changes in both directions. A single solid edge has zero reversals,
    // even if it runs diagonally through the window.
    vec4 yChanges = vec4(0.0), yNet = vec4(0.0), previous = vec4(0.0), prevValid = vec4(0.0);
    float yLastChanges = 0.0, yLastNet = 0.0, previousLast = 0.0, prevValidLast = 0.0;
    const vec4 kernel = vec4(1, 2, 2, 2);
    // Shift a full kernel inside the source at screen edges. Clamping each
    // individual tap there would overweight the last phase of a pattern.
    vec2 inset = min(vec2(2.0), floor((TextureSize - 1.0) * 0.5));
    vec2 center = clamp(pos, inset, TextureSize - 1.0 - inset);
    for (int y = -2; y <= 2; ++y) {
        vec2 q = center + vec2(-2, y);
        vec3 a = pixel(q), b = pixel(q + vec2(1,0)), c = pixel(q + vec2(2,0));
        vec3 d = pixel(q + vec2(3,0)), e = pixel(q + vec2(4,0));
        // Measure only the selected pair. Other entries can belong to a
        // neighboring ramp or a silhouette and must not enter its mean.
        vec4 row = vec4(a == other, b == other, c == other, d == other);
        vec4 zeros = vec4(a == src, b == src, c == src, d == src);
        float last = float(e == other);
        count += dot(row, vec4(1.0)) + last;
        float weight = abs(float(y)) == 2.0 ? 1.0 : 2.0;
        vec4 valid = row + zeros;
        float validLast = last + float(e == src);
        if ((valid != vec4(1.0) || validLast == 0.0)
            && (!nearColor(src,a) || !nearColor(src,b) || !nearColor(src,c) ||
                !nearColor(src,d) || !nearColor(src,e))) return false;
        population += dot(valid, vec4(1.0)) + validLast;
        totalWeight += weight * (dot(valid, kernel) + validLast);
        coverage += weight * (dot(row, kernel) + last);
        vec4 dx = (vec4(row.yzw, last) - row) * valid * vec4(valid.yzw, validLast);
        xTurns += reversals(dx);
        vec4 dy = (row - previous) * valid * prevValid;
        float dyLast = (last - previousLast) * validLast * prevValidLast;
        yChanges += abs(dy); yNet += dy;
        yLastChanges += abs(dyLast); yLastNet += dyLast;
        previous = row; previousLast = last;
        prevValid = valid; prevValidLast = validLast;
    }
    if (population < 15.0) return false;
    float yTurns = 0.5 * (dot(yChanges - abs(yNet), vec4(1.0))
                         + yLastChanges - abs(yLastNet));
    float support = max(smoothstep(0.75, 3.0, min(xTurns, yTurns)),
                        smoothstep(5.0, 7.0, max(xTurns, yTurns)));
    // A lone stipple at the taper of a gradient can still have a partner
    // several pixels away. A single isolated detail has no such support.
    float minority = min(count, population - count);
    if (minority <= 8.0 && support < 0.99) {
        bool otherIsInk = count <= population * 0.5;
        vec2 q = otherIsInk ? pos + offset : pos;
        vec3 ink = otherIsInk ? other : src;
        vec3 paper = otherIsInk ? src : other;
        // Probe through the paper so a long continuous stroke cannot supply
        // its own evidence of a second mark.
        bool repeats = repeatedAlong(q, vec2(1,0), ink, paper) ||
                       repeatedAlong(q, vec2(0,1), ink, paper);
        if (repeats) support = 1.0;
        else if (minority == 1.0) support = 0.0;
    }
    result = mix(src, other, coverage / totalWeight * support * DITHER_STRENGTH);
    return true;
}

void main() {
    vec2 pos = floor(gl_FragCoord.xy);
    vec3 source = pixel(pos);
    if (DITHER_STRENGTH <= 0.0) { gl_FragColor = vec4(source, 1.0); return; }
    vec3 ramp;
    if (blendRamp(pos, source, ramp)) { gl_FragColor = vec4(ramp, 1.0); return; }
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
