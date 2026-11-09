// SPDX-License-Identifier: GPL-3.0-or-later
#pragma parameter DITHER_STRENGTH "Dither blend strength" 1.0 0.0 1.0 0.05
#ifdef VERTEX
attribute vec4 VertexCoord;
uniform mat4 MVPMatrix;
void main() { gl_Position = MVPMatrix * VertexCoord; }
#elif defined(FRAGMENT)
uniform sampler2D Texture;
uniform vec2 TextureSize;
uniform float DITHER_STRENGTH;
vec3 pixel(vec2 p) { return texture2D(Texture, (p + 0.5) / TextureSize).rgb; }
struct Row { vec3 a; vec3 b; vec3 c; vec3 d; };
Row readRow(vec2 p, vec2 x) {
    return Row(pixel(p-x), pixel(p), pixel(p+x), pixel(p+2.0*x));
}
vec4 mask(Row row, vec3 color) {
    return vec4(row.a == color, row.b == color, row.c == color, row.d == color);
}
float total(vec4 v) { return dot(v, vec4(1.0)); }
vec3 changes(vec4 row) { return max(-row.xyz * row.yzw, 0.0); }
vec2 runs(vec4 a, vec4 b, vec4 c, vec4 d) {
    vec3 ab = changes(a), bb = changes(b), cb = changes(c), db = changes(d);
    vec2 turns = min(ab.xy,ab.yz) + min(bb.xy,bb.yz) + min(cb.xy,cb.yz) + min(db.xy,db.yz);
    float longRuns = min(min(ab.x,ab.y),ab.z) + min(min(bb.x,bb.y),bb.z)
                   + min(min(cb.x,cb.y),cb.z) + min(min(db.x,db.y),db.z);
    return vec2(turns.x + turns.y, longRuns);
}
bool closeColor(vec3 a, vec3 b) { return all(lessThanEqual(abs(a-b),vec3(0.30))); }
bool separated(vec2 p, vec2 dir, vec3 ink, vec3 paper) {
    return pixel(p) == ink && pixel(p-dir) == paper && pixel(p+dir) == paper;
}
bool sparse(vec2 p, vec2 dir, vec3 ink, vec3 paper) {
    if (pixel(p-dir) != paper || pixel(p+dir) != paper) return false;
    return separated(p+2.0*dir, dir, ink, paper) || separated(p-2.0*dir, dir, ink, paper)
        || separated(p+4.0*dir, dir, ink, paper) || separated(p-4.0*dir, dir, ink, paper)
        || separated(p+3.0*dir, dir, ink, paper) || separated(p-3.0*dir, dir, ink, paper)
        || separated(p+6.0*dir, dir, ink, paper) || separated(p-6.0*dir, dir, ink, paper)
        || separated(p+8.0*dir, dir, ink, paper) || separated(p-8.0*dir, dir, ink, paper);
}
// Project nearby color changes onto the local palette step. Requiring
// variation in two parallel lines distinguishes a shaded field that
// changes colors from a single stroke or the interior of a small glyph.
bool fieldProbe(vec2 q, vec2 dir, vec2 across, vec3 projection) {
    vec2 end = q + dir + across;
    if (any(lessThan(min(q,end),vec2(0.0))) || any(greaterThanEqual(max(q,end),TextureSize))) return false;
    float a = abs(dot(pixel(q) - pixel(q + dir), projection));
    if (a < 0.20 || a > 2.1) return false;
    float b = abs(dot(pixel(q + across) - pixel(end), projection));
    return b >= 0.20 && b <= 2.1;
}
bool texturedField(vec2 p, vec2 dir, vec3 c, vec3 other) {
    vec2 across = dir.yx;
    vec3 delta = other-c, projection = delta / dot(delta,delta);
    return fieldProbe(p + 8.0*dir + 2.0*across, dir, across, projection) ||
           fieldProbe(p - 8.0*dir - 2.0*across, -dir, -across, projection) ||
           fieldProbe(p + 8.0*dir - 2.0*across, dir, -across, projection) ||
           fieldProbe(p - 8.0*dir + 2.0*across, -dir, across, projection) ||
           fieldProbe(p + 7.0*across, dir, across, projection) ||
           fieldProbe(p - 7.0*across, -dir, -across, projection);
}
bool inside(vec2 p) {
    return all(greaterThanEqual(p, vec2(0.0))) && all(lessThan(p, TextureSize));
}
bool supportRow(vec2 q, vec2 dir, vec3 c, vec3 other) {
    return inside(q) && inside(q + 5.0*dir) && pixel(q) == c && pixel(q + dir) == other &&
           pixel(q + 2.0*dir) == c && pixel(q + 3.0*dir) == other &&
           pixel(q + 4.0*dir) == c && pixel(q + 5.0*dir) == other;
}
bool longStripe(vec2 p, vec2 along, vec2 across, vec3 c, vec3 other) {
    for (int side = -1; side <= 1; side += 2) {
        vec2 dir = float(side) * along;
        if (!inside(p + 5.0*dir) || pixel(p + dir) != other || pixel(p + 2.0*dir) != c ||
            pixel(p + 3.0*dir) != other || pixel(p + 4.0*dir) != c || pixel(p + 5.0*dir) != other) continue;
        bool above = supportRow(p + across, dir, c, other);
        bool below = supportRow(p - across, dir, c, other);
        if ((above && (below || supportRow(p + 2.0*across, dir, c, other))) ||
            (below && supportRow(p - 2.0*across, dir, c, other))) return true;
    }
    return false;
}
void main() {
    vec2 p = floor(gl_FragCoord.xy);
    vec3 c = pixel(p);
    gl_FragColor = vec4(c,1.0);
    if (DITHER_STRENGTH <= 0.0 || any(lessThan(TextureSize, vec2(4.0)))) return;
    vec2 x = vec2(1,0), y = vec2(0,1);
    vec3 l = pixel(p-x), r = pixel(p+x), d = pixel(p-y), u = pixel(p+y);
    vec3 dl = pixel(p-x-y), dr = pixel(p+x-y), ul = pixel(p-x+y), ur = pixel(p+x+y);
    // Prefer a pair repeated across scanlines where a third color meets the
    // center pixel. Its two colors still belong to the same textured area.
    bool left = l != c && ((u == c && ul == l) || (d == c && dl == l));
    bool right = r != c && ((u == c && ur == r) || (d == c && dr == r));
    bool down = d != c && ((l == c && dl == d) || (r == c && dr == d));
    bool up = u != c && ((l == c && ul == u) || (r == c && ur == u));
    // Both sides of a straight silhouette can repeat vertically. Select
    // the side that also returns to the center color along the scanline.
    if (left && right && l != r) {
        left = pixel(p - 2.0*x) == c;
        right = pixel(p + 2.0*x) == c;
    }
    if (down && up && d != u) {
        down = pixel(p - 2.0*y) == c;
        up = pixel(p + 2.0*y) == c;
    }
    vec2 offset = x;
    vec3 other = r;
    if (left && !right) { other=l; offset=-x; }
    else if (!right && up) { other=u; offset=y; }
    else if (!right && down) { other=d; offset=-y; }
    if (other == c) { other=l; offset=-x; }
    if (other == c) { other=u; offset=y; }
    if (other == c) { other=d; offset=-y; }
    if (other == c) { other=ur; offset=x+y; }
    if (other == c) { other=dl; offset=-x-y; }
    if (other == c) { other=ul; offset=-x+y; }
    if (other == c) { other=dr; offset=x-y; }
    if (l == r && r != c && (d == c || u == c)) { other = r; offset = x; }
    else if (u == d && u != c && (l == c || r == c)) { other = u; offset = y; }
    if (other == c) return;

    // Extend the footprint into the pattern, keeping its position relative
    // to the image content during scrolling.
    if ((left && !right) || p.x >= TextureSize.x - 2.0) {
        x = -x;
        vec3 tmp = l; l = r; r = tmp;
        tmp = ul; ul = ur; ur = tmp;
        tmp = dl; dl = dr; dr = tmp;
    }
    if ((down && !up) || p.y >= TextureSize.y - 2.0) {
        y = -y;
        vec3 tmp = d; d = u; u = tmp;
        tmp = dl; dl = ul; ul = tmp;
        tmp = dr; dr = ur; ur = tmp;
    }
    // At the actual frame boundary slide the window inward. Repeating the
    // last texel would bias the density and invent parallel scanlines.
    vec2 inset = vec2(x.x < 0.0 ? 2.0 : 1.0, y.y < 0.0 ? 2.0 : 1.0);
    vec2 origin = clamp(p, inset, TextureSize - 4.0 + inset);
    Row ra, rb, rc;
    if (origin == p) {
        ra = Row(dl, d, dr, pixel(p + 2.0*x - y));
        rb = Row(l, c, r, pixel(p + 2.0*x));
        rc = Row(ul, u, ur, pixel(p + 2.0*x + y));
    } else {
        ra = readRow(origin - y, x);
        rb = readRow(origin, x);
        rc = readRow(origin + y, x);
    }
    Row rd = readRow(origin + 2.0*y, x);
    // Signed masks use -1/+1 for the selected pair and zero for foreign
    // colors, which never contribute to the output or to a reversal.
    vec4 ma=mask(ra,other)-mask(ra,c), mb=mask(rb,other)-mask(rb,c);
    vec4 mc=mask(rc,other)-mask(rc,c), md=mask(rd,other)-mask(rd,c);
    float valid = total(abs(ma)+abs(mb)+abs(mc)+abs(md));
    if (valid < 6.0) return;
    float count = 0.5*(total(ma+mb+mc+md)+valid);
    vec2 rx = runs(ma,mb,mc,md);
    vec4 ab=max(-ma*mb,0.0), bc=max(-mb*mc,0.0), cd=max(-mc*md,0.0);
    float yTurns=total(min(ab,bc)+min(bc,cd)), yRuns=total(min(min(ab,bc),cd));
    float xTurns = rx.x;
    bool gentle = closeColor(c, other);
    bool midtone = all(lessThanEqual(abs(c - other), vec3(0.58)));
    // A high-amplitude gradient still forms coherent scanline reversals.
    // Geometry can confirm such pairs even when the palette steps are wide.
    bool patterned = ((left || right) && xTurns >= 2.0) ||
                     ((down || up) && yTurns >= 2.0) || (xTurns >= 2.0 && yTurns >= 2.0);
    if (!gentle && midtone && (patterned || rx.y >= 1.0 || yRuns >= 1.0)) {
        // A single dotted rule can have many reversals along its length.
        // Require the less common shade on multiple rows and columns.
        float signInk = count <= valid * 0.5 ? 1.0 : -1.0;
        vec4 ia = max(ma * signInk, 0.0), ib = max(mb * signInk, 0.0);
        vec4 ic = max(mc * signInk, 0.0), id = max(md * signInk, 0.0);
        float rows = total(min(vec4(total(ia), total(ib), total(ic), total(id)), vec4(1.0)));
        float cols = total(min(ia + ib + ic + id, vec4(1.0)));
        if (min(rows, cols) >= 2.0) {
            gentle = (xTurns > 0.0 && texturedField(p, x, c, other)) ||
                     (yTurns > 0.0 && texturedField(p, y, c, other));
        }
    }
    float density = count/valid;
    bool vertical = (left || right) && yTurns < 2.0 &&
        (rx.y >= 2.0 || (xTurns >= 2.0 && r == other &&
         ((l == other && (pixel(p + 2.0*x) == c || pixel(p - 2.0*x) == c)) ||
          (pixel(p + 2.0*x) == c && pixel(p + 3.0*x) == other))));
    bool horizontal = (down || up) && xTurns < 2.0 &&
        (yRuns >= 2.0 || (yTurns >= 2.0 && u == other &&
         ((d == other && (pixel(p + 2.0*y) == c || pixel(p - 2.0*y) == c)) ||
          (pixel(p + 2.0*y) == c && pixel(p + 3.0*y) == other))));
    // At a density transition the last mark on a scanline may be isolated;
    // the parallel line immediately inside the texture still carries a run.
    bool nearbyRun = (rx.y >= 1.0 && xTurns >= 3.0) || (yRuns >= 1.0 && yTurns >= 3.0);
    if (vertical || horizontal) {
        // High-contrast glyphs can contain several adjacent stems. Confirm
        // an extended run across three real scanlines before merging it.
        if (!gentle && !longStripe(p, vertical ? x : y, vertical ? y : x, c, other)) return;
        density=0.5;
    } else if (!gentle) {
        // A complete two-dimensional lattice supports checker and 25/75
        // fields. Single dotted baselines have no repeated second row.
        if (ma != mc || mb != md || ma.xy != ma.zw || mb.xy != mb.zw || valid != 16.0) return;
    } else {
        float minority = min(count,valid-count);
        bool texture = nearbyRun || (xTurns >= 1.0 && yTurns >= 1.0 &&
                       xTurns + yTurns >= 3.0 && minority >= 2.0);
        if (!texture) {
            bool otherInk=count <= 0.5*valid;
            vec2 q=otherInk ? p+offset:p;
            vec3 ink=otherInk ? other:c, paper=otherInk ? c:other;
            if (!sparse(q,x,ink,paper) && !sparse(q,y,ink,paper)) return;
        }
    }
    gl_FragColor=vec4(mix(c,other,density*DITHER_STRENGTH),1.0);
}
#endif
