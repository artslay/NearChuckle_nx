#include "compat/reveal.h"
#include "compat/loader.h"

#include <cmath>
#include <cstring>

namespace {

// ─── The design space ───────────────────────────────────────────────────────
// Every coordinate below is the trailer's own 1280x720 viewBox, so the shapes
// are the SVG's numbers verbatim and can be diffed against it.
constexpr float DW = 1280.0f, DH = 720.0f;
constexpr int   SS = 2;                     // supersample factor

// ─── Palette ────────────────────────────────────────────────────────────────
constexpr SDL_Color PAPER = {250, 253, 251, 255};   // #FAFDFB
constexpr SDL_Color INK   = {  4,  20,  12, 255};   // #04140C
constexpr SDL_Color G_100 = {185, 246, 202, 255};   // #B9F6CA
constexpr SDL_Color G_200 = {105, 240, 174, 255};   // #69F0AE
constexpr SDL_Color G_400 = {  0, 230, 118, 255};   // #00E676
constexpr SDL_Color G_500 = {  0, 200,  83, 255};   // #00C853
constexpr SDL_Color G_600 = {  0, 168,  84, 255};   // #00A854
constexpr SDL_Color G_650 = {  0, 163,  77, 255};   // #00A34D
constexpr SDL_Color G_800 = {  0, 117,  63, 255};   // #00753F

inline SDL_Color withA(SDL_Color c, float a) {
    if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
    c.a = (Uint8)(c.a * a + 0.5f);
    return c;
}

// ─── Timing ─────────────────────────────────────────────────────────────────
//
// Fractions of the whole reveal rather than seconds, because the reveal's
// length is the jingle's length — it is 3.1s with no audio and 4.1s with, and
// the picture has to land with the sound either way.
constexpr float T_SPIN_A = 0.000f, T_SPIN_B = 0.150f;   // turns over, coming in
constexpr float T_DROP_A = 0.190f, T_DROP_B = 0.300f;   // falls to centre
constexpr float T_LAND_B = 0.365f;                      // squash out of the landing
constexpr float T_CRK_A  = 0.320f, T_CRK_B  = 0.470f;   // the seam opens
constexpr float T_SHD_A  = 0.320f, T_SHD_B  = 0.640f;   // shards
constexpr float T_ICN_A  = 0.415f, T_ICN_B  = 0.640f;   // the icon rises
constexpr float T_GEM_A  = 0.560f, T_GEM_B  = 0.720f;   // the halves let go
constexpr float T_CAP_A  = 0.660f, T_CAP_B  = 0.750f;   // caption
constexpr float T_BLK_A  = 0.880f;                      // dip to black

// Where things sit.
//
// Larger than the trailer's 1.35, which is the scale of a prop inside a wider
// scene. Here the mark is the whole shot, so it is sized to fill it — high
// enough at the start to have somewhere to fall from, and clear of the caption
// once it lands.
constexpr float GEM_X    = DW * 0.5f;
constexpr float GEM_HIGH = 200.0f;      // where it turns over, before the drop
constexpr float GEM_REST = 336.0f;      // where it lands and breaks
constexpr float GEM_SC   = 1.75f;
constexpr float ICON_PX  = 200.0f;      // the icon at full size

// ─── Easing, the trailer's own ──────────────────────────────────────────────
// cubic-bezier(0.2, 0, 0, 1), solved by Newton exactly as the trailer solves
// it, so a beat that lands on a frame there lands on the same frame here.
float ease(float x) {
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    constexpr float x1 = 0.2f, y1 = 0.0f, x2 = 0.0f, y2 = 1.0f;
    const float cx = 3 * x1, bx = 3 * (x2 - x1) - cx, ax = 1 - cx - bx;
    const float cy = 3 * y1, by = 3 * (y2 - y1) - cy, ay = 1 - cy - by;
    float t = x;
    for (int i = 0; i < 8; i++) {
        const float e = ((ax * t + bx) * t + cx) * t - x;
        if (fabsf(e) < 1e-6f) break;
        const float d = (3 * ax * t + 2 * bx) * t + cx;
        if (fabsf(d) < 1e-7f) break;
        t -= e / d;
    }
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    return ((ay * t + by) * t + cy) * t;
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
inline float seg(float s, float a, float b) { return clamp01((s - a) / (b - a)); }
inline float lerpf(float a, float b, float u) { return a + (b - a) * u; }

// The trailer's hash. Kept in double: in float the multiply by 43758.5453
// throws away the low bits that *are* the noise, and the jitter collapses into
// a handful of repeated offsets.
inline float hsh(double i) {
    const double x = sin(i * 127.1 + 311.7) * 43758.5453;
    return (float)((x - floor(x)) * 2.0 - 1.0);
}

// ─── Boil ───────────────────────────────────────────────────────────────────
//
// The whole drawing is redrawn on 2s — 12 frames a second — with every vertex
// nudged by up to ~1.1px and every line's weight varied by ~14%. Held between
// boil frames, so it reads as a hand redrawing the same shape rather than as
// noise. This is the single thing that makes it look drawn instead of
// rendered, so it is applied at the vertex level, not as a filter.
int g_fr = 0;

inline float boilW(float w, int seed) { return w * (1.0f + hsh(g_fr * 1.9 + seed) * 0.14f); }

struct P { float x, y; };

inline P boilP(P p, int seed, int i, float amp = 1.1f) {
    p.x += hsh(g_fr * 3.7 + seed + i) * amp;
    p.y += hsh(g_fr * 2.3 + seed + i + 17) * amp;
    return p;
}

// ─── Primitives ─────────────────────────────────────────────────────────────
//
// SDL_RenderGeometry is the fast path. It is not in every backend, so the
// first refusal switches to a scanline fill for the rest of the run instead of
// leaving a hole where the artwork should be.
bool g_geom = true;

void scanPoly(SDL_Renderer* r, const P* p, int n, SDL_Color c) {
    float y0 = p[0].y, y1 = p[0].y;
    for (int i = 1; i < n; i++) {
        if (p[i].y < y0) y0 = p[i].y;
        if (p[i].y > y1) y1 = p[i].y;
    }
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    for (int y = (int)floorf(y0); y <= (int)ceilf(y1); y++) {
        const float cy = y + 0.5f;
        float xs[24];
        int   m = 0;
        for (int i = 0; i < n && m < 24; i++) {
            const P& a = p[i];
            const P& b = p[(i + 1) % n];
            if ((a.y <= cy && b.y > cy) || (b.y <= cy && a.y > cy))
                xs[m++] = a.x + (cy - a.y) / (b.y - a.y) * (b.x - a.x);
        }
        for (int i = 1; i < m; i++)             // insertion sort; m is tiny
            for (int j = i; j > 0 && xs[j] < xs[j - 1]; j--) {
                const float t = xs[j]; xs[j] = xs[j - 1]; xs[j - 1] = t;
            }
        for (int i = 0; i + 1 < m; i += 2) {
            const int xa = (int)floorf(xs[i]), xb = (int)ceilf(xs[i + 1]);
            if (xb <= xa) continue;
            SDL_Rect row = {xa, y, xb - xa, 1};
            SDL_RenderFillRect(r, &row);
        }
    }
}

constexpr int MAXV = 40;

void fillPoly(SDL_Renderer* r, const P* p, int n, SDL_Color c) {
    if (n < 3 || c.a == 0) return;
    if (g_geom && n <= MAXV) {
        SDL_Vertex v[MAXV];
        int        idx[(MAXV - 2) * 3];
        for (int i = 0; i < n; i++) {
            v[i].position  = {p[i].x, p[i].y};
            v[i].color     = c;
            v[i].tex_coord = {0.0f, 0.0f};
        }
        for (int i = 0; i < n - 2; i++) {       // fan
            idx[i * 3 + 0] = 0;
            idx[i * 3 + 1] = i + 1;
            idx[i * 3 + 2] = i + 2;
        }
        if (SDL_RenderGeometry(r, nullptr, v, n, idx, (n - 2) * 3) == 0) return;
        g_geom = false;
        compatLogFmt("reveal: SDL_RenderGeometry unavailable (%s) — scanline fills",
                     SDL_GetError());
    }
    scanPoly(r, p, n, c);
}

// A filled disc, used for the round joins and caps the trailer's strokes have.
void disc(SDL_Renderer* r, P c, float rad, SDL_Color col) {
    if (rad < 0.35f) return;
    P v[16];
    for (int i = 0; i < 16; i++) {
        const float a = i * (6.2831853f / 16.0f);
        v[i] = {c.x + cosf(a) * rad, c.y + sinf(a) * rad};
    }
    fillPoly(r, v, 16, col);
}

void strokeSeg(SDL_Renderer* r, P a, P b, float w, SDL_Color c) {
    float dx = b.x - a.x, dy = b.y - a.y;
    const float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-4f) return;
    const float hx = -dy / len * w * 0.5f, hy = dx / len * w * 0.5f;
    P q[4] = {{a.x + hx, a.y + hy}, {b.x + hx, b.y + hy},
              {b.x - hx, b.y - hy}, {a.x - hx, a.y - hy}};
    fillPoly(r, q, 4, c);
}

// Round joins and caps, as the trailer's stroke-linejoin/linecap ask for.
void strokePath(SDL_Renderer* r, const P* p, int n, bool closed, float w, SDL_Color c) {
    if (n < 2 || c.a == 0) return;
    const int segs = closed ? n : n - 1;
    for (int i = 0; i < segs; i++) strokeSeg(r, p[i], p[(i + 1) % n], w, c);
    if (w > 2.0f) for (int i = 0; i < n; i++) disc(r, p[i], w * 0.5f, c);
}

// ─── Pencil hatching ────────────────────────────────────────────────────────
//
// The SVG does this with a <pattern> at 38 degrees. There is no pattern fill in
// SDL, and tiling a texture through UVs would need a wrap mode the renderer
// does not promise — so the lines are clipped to the facet analytically. The
// facets are convex, so each hatch line crosses the outline exactly twice and
// the span between is the whole of the work.
//
// The hatch is generated in screen space, not gem space, which is what the SVG
// does too (patternUnits="userSpaceOnUse"): the shading stays at a constant
// density on the paper while the shape it sits in moves and scales.
void hatchPoly(SDL_Renderer* r, const P* p, int n,
               float dirDeg, float spacing, float w, SDL_Color c) {
    if (n < 3 || c.a == 0 || spacing < 0.5f) return;
    const float a  = dirDeg * 3.14159265f / 180.0f;
    const float ca = cosf(a), sa = sinf(a);

    // Rotate into hatch space, where the lines are horizontal.
    P q[MAXV];
    if (n > MAXV) return;
    float u0 = 0.0f, u1 = 0.0f;
    for (int i = 0; i < n; i++) {
        q[i] = { p[i].x * ca + p[i].y * sa, -p[i].x * sa + p[i].y * ca };
        if (i == 0 || q[i].y < u0) u0 = q[i].y;
        if (i == 0 || q[i].y > u1) u1 = q[i].y;
    }
    const int k0 = (int)ceilf(u0 / spacing), k1 = (int)floorf(u1 / spacing);
    for (int k = k0; k <= k1; k++) {
        const float y = k * spacing;
        float xa = 0.0f, xb = 0.0f;
        int   hits = 0;
        for (int i = 0; i < n; i++) {
            const P& s = q[i];
            const P& e = q[(i + 1) % n];
            if ((s.y <= y && e.y > y) || (e.y <= y && s.y > y)) {
                const float x = s.x + (y - s.y) / (e.y - s.y) * (e.x - s.x);
                if (hits == 0)      xa = xb = x;
                else { if (x < xa) xa = x; if (x > xb) xb = x; }
                hits++;
            }
        }
        if (hits < 2 || xb - xa < 0.5f) continue;
        // Back to screen space, and jittered — a ruled hatch inside a boiling
        // outline is the one thing that would give the drawing away.
        const float j = hsh(g_fr * 4.1 + k * 3.0) * 0.7f;
        P s = { (xa + j) * ca - y * sa,  (xa + j) * sa + y * ca };
        P e = { (xb + j) * ca - y * sa,  (xb + j) * sa + y * ca };
        strokeSeg(r, s, e, w, c);
    }
}

// ─── The mark ───────────────────────────────────────────────────────────────
//
// Straight out of the trailer's <g id="gem"> / <g id="gemBack">. Every facet
// sits entirely on one side of the seam at x=0, which is why the break needs no
// clipping: the left facets are the left half.

struct Facet { SDL_Color c; int n; float p[8]; };

const Facet FRONT_L[] = {
    {G_600, 3, {-1.5f, -102, -50, -24, -1.5f, -16}},
    {G_200, 3, {-50,   -24,  -26, -56, -1.5f, -16}},
    {G_400, 3, {-1.5f, -16,  -50, -24, -33,    28}},
    {G_500, 3, {-1.5f, -16,  -33,  28, -1.5f, 102}},
    {G_650, 3, {-14,    64,  -33,  28, -1.5f, 102}},
};
const Facet FRONT_R[] = {
    {G_100, 3, { 1.5f, -102,  50, -24,  1.5f, -16}},
    {G_200, 3, { 1.5f, -102,  20, -64,  1.5f, -16}},
    {G_800, 3, { 1.5f, -16,   50, -24,  33,    28}},
    {G_650, 3, { 1.5f, -16,   33,  28,  1.5f, 102}},
    {G_800, 3, { 14,    64,   33,  28,  1.5f, 102}},
};
const Facet BACK_L[] = {
    {G_800, 3, {-1.5f, -102, -50, -24, -1.5f, -16}},
    {G_650, 3, {-1.5f, -16,  -50, -24, -33,    28}},
    {G_800, 3, {-1.5f, -16,  -33,  28, -1.5f, 102}},
};
const Facet BACK_R[] = {
    {G_600, 3, { 1.5f, -102,  50, -24,  1.5f, -16}},
    {G_800, 3, { 1.5f, -16,   50, -24,  33,    28}},
    {G_650, 3, { 1.5f, -16,   33,  28,  1.5f, 102}},
};

// The heavy outline each half carries, and the offset ghost of it underneath —
// the trailer draws the same path twice, the second time nudged down-right at
// 30% and half the weight, which is what gives a flat drawing its depth.
const float OUTLINE_L[8] = {-1.5f, -102, -50, -24, -33, 28, -1.5f, 102};
const float OUTLINE_R[8] = { 1.5f, -102,  50, -24,  33, 28,  1.5f, 102};

// Facets that carry shading, listed by index into the arrays above.
const int SHADE_L[] = {4};                   // hShade
const int SHADE_R[] = {2, 3, 4};             // hCross on 2 and 4, hShade on 3

struct Xf { float ox, oy, sx, sy; };
inline P ap(const Xf& x, float px, float py) { return {x.ox + px * x.sx, x.oy + py * x.sy}; }

void drawFacets(SDL_Renderer* r, const Xf& x, const Facet* f, int count,
                int seed0, float alpha) {
    for (int i = 0; i < count; i++) {
        P v[4];
        for (int k = 0; k < f[i].n; k++)
            v[k] = ap(x, f[i].p[k * 2], f[i].p[k * 2 + 1]);
        for (int k = 0; k < f[i].n; k++) v[k] = boilP(v[k], seed0 + i * 7, k);
        fillPoly(r, v, f[i].n, withA(f[i].c, alpha));
        strokePath(r, v, f[i].n, true, boilW(1.5f, seed0 + i), withA(INK, alpha));
    }
}

// One half of the mark, at `gx` — the whole break is these two called with a
// widening gap between them.
void drawHalf(SDL_Renderer* r, float gx, float gy, float sx, float sy,
              bool front, bool left, float alpha) {
    const Xf x = {gx, gy, sx, sy};
    const Facet* f = front ? (left ? FRONT_L : FRONT_R) : (left ? BACK_L : BACK_R);
    const int    n = front ? 5 : 3;
    const int seed = (left ? 100 : 300) + (front ? 0 : 500);

    drawFacets(r, x, f, n, seed, alpha);

    // Shading, screen-space, over the facets it belongs to.
    const float shA = alpha * (front ? 0.40f : 0.35f);
    const int*  sh  = left ? SHADE_L : SHADE_R;
    const int   shn = front ? (left ? 1 : 3) : 1;
    for (int i = 0; i < shn && front; i++) {
        const Facet& fc = f[sh[i]];
        P v[4];
        for (int k = 0; k < fc.n; k++) v[k] = ap(x, fc.p[k * 2], fc.p[k * 2 + 1]);
        hatchPoly(r, v, fc.n, 128.0f, 6.0f, 1.5f, withA(INK, shA));
        if (!left && sh[i] != 3)     // the two cross-hatched facets on the right
            hatchPoly(r, v, fc.n, 38.0f, 5.0f, 1.1f, withA(INK, shA));
    }
    if (!front) {                     // the back has one shaded facet either side
        const Facet& fc = f[left ? 0 : 1];
        P v[4];
        for (int k = 0; k < fc.n; k++) v[k] = ap(x, fc.p[k * 2], fc.p[k * 2 + 1]);
        hatchPoly(r, v, fc.n, 128.0f, 6.0f, 1.5f, withA(INK, shA));
    }

    // The outline, ghost first.
    const float* op = left ? OUTLINE_L : OUTLINE_R;
    P o[4], g[4];
    for (int k = 0; k < 4; k++) {
        o[k] = boilP(ap(x, op[k * 2], op[k * 2 + 1]), seed + 60, k);
        g[k] = {o[k].x + 2.0f, o[k].y + 1.8f};
    }
    strokePath(r, g, 4, true, boilW(2.0f, seed + 61), withA(INK, alpha * 0.30f));
    strokePath(r, o, 4, true, boilW(3.6f, seed + 62), withA(INK, alpha));
}

// ─── Shards ─────────────────────────────────────────────────────────────────
const SDL_Color SHARD_C[14] = {
    G_200, G_100, G_400, G_600, G_200, G_100, G_400,
    G_800, G_200, G_100, G_400, G_650, G_200, G_800,
};

void drawShards(SDL_Renderer* r, float t) {
    // Before the break there are no shards. seg() clamps, so without this the
    // fade reads 1 and all fourteen sit stacked on the gem's waist from the
    // first frame — which looks like a flower pinned to the mark.
    if (t < T_SHD_A) return;
    const float u = seg(t, T_SHD_A, T_SHD_A + (T_SHD_B - T_SHD_A) * 0.55f);
    const float fade = 1.0f - seg(t, T_SHD_A + (T_SHD_B - T_SHD_A) * 0.42f, T_SHD_B);
    if (fade <= 0.0f) return;

    for (int i = 0; i < 14; i++) {
        const float ang  = (-170.0f + i * (340.0f / 13.0f)) * 3.14159265f / 180.0f;
        const float dist = lerpf(6.0f, 130.0f + (i % 5) * 34.0f, ease(u));
        const float sc   = lerpf(0.4f, 1.05f, ease(u * 1.6f > 1.0f ? 1.0f : u * 1.6f));
        const float cx   = GEM_X + cosf(ang) * dist;
        const float cy   = GEM_REST + sinf(ang) * dist * 0.85f;
        const float rot  = ang + hsh(g_fr + i) * 0.05f;
        const float cr = cosf(rot) * sc, sr = sinf(rot) * sc;

        // (0,0) (36,-8) (44,0) (36,8) — the trailer's splinter.
        static const float SP[8] = {0, 0, 36, -8, 44, 0, 36, 8};
        P v[4];
        for (int k = 0; k < 4; k++) {
            v[k] = {cx + SP[k * 2] * cr - SP[k * 2 + 1] * sr,
                    cy + SP[k * 2] * sr + SP[k * 2 + 1] * cr};
            v[k] = boilP(v[k], 700 + i * 5, k);
        }
        fillPoly(r, v, 4, withA(SHARD_C[i], fade));
        strokePath(r, v, 4, true, boilW(1.8f, 700 + i), withA(INK, fade));
    }
}

// ─── The impact ─────────────────────────────────────────────────────────────
//
// Where the design's original break had a white flash, this has two rings drawn
// as pencil circles. A flash is a lighting effect and there is no lighting in a
// drawing — a ring the hand could have drawn belongs, and reads as the same
// beat.
void drawImpact(SDL_Renderer* r, float t) {
    for (int ring = 0; ring < 2; ring++) {
        const float a0 = T_CRK_A + ring * 0.030f;
        const float u  = seg(t, a0, a0 + 0.185f);
        if (u <= 0.0f || u >= 1.0f) continue;
        const float rad = lerpf(48.0f, 300.0f + ring * 70.0f, ease(u));
        const float alp = (1.0f - u) * (1.0f - u) * (ring ? 0.7f : 1.0f);
        P c[28];
        for (int i = 0; i < 28; i++) {
            const float a = i * (6.2831853f / 28.0f);
            c[i] = {GEM_X + cosf(a) * rad + hsh(g_fr * 2.0 + i + ring * 31) * 2.4f,
                    GEM_REST + sinf(a) * rad * 0.9f + hsh(g_fr * 2.6 + i + ring * 17) * 2.4f};
        }
        strokePath(r, c, 28, true, ring ? 3.0f : 4.4f,
                   withA(ring ? G_200 : G_500, alp));
    }
}

// ─── The game's icon ────────────────────────────────────────────────────────
//
// Drawn as the trailer draws its stand-in: a rounded plate with an ink outline,
// on the paper. The artwork is a square texture, so the corners are painted
// back to the plate colour and the outline is stroked over the join — the
// renderer has no rounded clip, and this is indistinguishable at any size the
// icon is actually shown at.
void roundRectPath(P* out, float cx, float cy, float half, float rad, int seed) {
    int n = 0;
    static const float SGN[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
    for (int c = 0; c < 4; c++) {
        const float ox = SGN[c][0] * (half - rad), oy = SGN[c][1] * (half - rad);
        const float a0 = c * 1.5707963f;
        for (int k = 0; k <= 4; k++) {
            const float a = a0 + k * (1.5707963f / 4.0f);
            out[n] = boilP({cx + ox + cosf(a) * rad, cy + oy + sinf(a) * rad}, seed, n, 0.8f);
            n++;
        }
    }
}

void drawIcon(SDL_Renderer* r, SDL_Texture* icon, float t) {
    if (t < T_ICN_A) return;
    const float u = seg(t, T_ICN_A, T_ICN_B);
    // Overshoot: it is thrown clear of the mark, so it arrives slightly big and
    // settles. A monotonic ease reads as inflating.
    float sc = lerpf(0.04f, 1.06f, ease(u));
    if (u > 0.82f) sc = lerpf(1.06f, 1.0f, ease(seg(u, 0.82f, 1.0f)));

    const float half = ICON_PX * 0.5f * sc;
    if (half < 2.0f) return;
    const float rad = 26.0f * (ICON_PX / 140.0f) * sc;
    const float cx  = GEM_X    + hsh(g_fr * 1.7 + 900) * 1.0f;
    const float cy  = GEM_REST + hsh(g_fr * 2.9 + 901) * 1.0f;

    // The plate.
    P rr[20];
    roundRectPath(rr, cx, cy, half, rad, 910);
    fillPoly(r, rr, 20, PAPER);

    if (icon) {
        SDL_FRect d = {cx - half, cy - half, half * 2, half * 2};
        SDL_RenderCopyF(r, icon, nullptr, &d);
        // Paint the corners back to the plate, so the square artwork ends where
        // the drawn outline says it does.
        SDL_SetRenderDrawColor(r, PAPER.r, PAPER.g, PAPER.b, 255);
        for (int q = 0; q < 4; q++) {
            const float sx = (q & 1) ? -1.0f : 1.0f;
            const float sy = (q & 2) ? -1.0f : 1.0f;
            for (int k = 0; k < (int)rad + 1; k++) {
                const float dy = rad - k - 0.5f;
                const float in = rad - sqrtf(rad * rad - dy * dy > 0 ? rad * rad - dy * dy : 0);
                if (in < 0.5f) continue;
                const float px = sx > 0 ? cx + half - in : cx - half;
                const float py = sy > 0 ? cy + half - k - 1 : cy - half + k;
                SDL_FRect row = {px, py, in, 1.0f};
                SDL_RenderFillRectF(r, &row);
            }
        }
    } else {
        fillPoly(r, rr, 20, G_500);
    }
    strokePath(r, rr, 20, true, boilW(3.0f, 912), INK);
}

// ─── Supersample target ─────────────────────────────────────────────────────
SDL_Texture* g_rt   = nullptr;
bool         g_rtTried = false;

}  // namespace

namespace reveal {

float captionAlpha(float t) { return clamp01(seg(t, T_CAP_A, T_CAP_B)); }

float blackAlpha(float t) {
    const float k = seg(t, T_BLK_A, 1.0f);
    return k * k;                            // ease in, so the picture holds
}

void release(void) {
    if (g_rt) { SDL_DestroyTexture(g_rt); g_rt = nullptr; }
    g_rtTried = false;
}

void draw(SDL_Renderer* r, float t, SDL_Texture* icon) {
    // 12fps. Everything jittered is jittered off this, so the drawing is held
    // for two display frames at a time and only the motion runs at 60.
    g_fr = (int)(SDL_GetTicks() / 83);

    int outW = 0, outH = 0;
    SDL_GetRendererOutputSize(r, &outW, &outH);
    if (outW <= 0 || outH <= 0) { outW = (int)DW; outH = (int)DH; }

    if (!g_rtTried) {
        g_rtTried = true;
        g_rt = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
                                 (int)DW * SS, (int)DH * SS);
        if (g_rt) SDL_SetTextureBlendMode(g_rt, SDL_BLENDMODE_NONE);
        else compatLogFmt("reveal: no %dx supersample target (%s) — drawing direct",
                          SS, SDL_GetError());
    }

    SDL_Texture* prevTarget = SDL_GetRenderTarget(r);
    float px = 1.0f, py = 1.0f;
    if (g_rt) {
        SDL_SetRenderTarget(r, g_rt);
        px = py = (float)SS;
    } else {
        px = outW / DW;
        py = outH / DH;
    }
    SDL_RenderSetScale(r, px, py);

    // ── The paper ──
    SDL_SetRenderDrawColor(r, PAPER.r, PAPER.g, PAPER.b, 255);
    SDL_RenderFillRect(r, nullptr);

    // ── Where the mark is, and which way round ──
    float gy = GEM_HIGH, sx = GEM_SC, sy = GEM_SC, alpha = 1.0f, gap = 0.0f;
    bool  front = true;

    if (t < T_SPIN_B) {
        // Turning over as it arrives: two full flips, the back of the mark
        // showing through the half of each turn that faces away.
        const float u = ease(seg(t, T_SPIN_A, T_SPIN_B));
        const float c = cosf(u * 3.14159265f * 4.0f);
        front = c >= 0.0f;
        sx    = GEM_SC * (c >= 0 ? 1.0f : -1.0f) * fmaxf(0.08f, fabsf(c));
        sy    = GEM_SC * lerpf(0.62f, 1.0f, u);
        sx   *= lerpf(0.62f, 1.0f, u);
        alpha = clamp01(seg(t, T_SPIN_A, T_SPIN_A + 0.04f));
    } else if (t < T_DROP_B) {
        // The fall accelerates — pow 2.1, the trailer's own.
        const float u = clamp01(seg(t, T_DROP_A, T_DROP_B));
        gy = lerpf(GEM_HIGH, GEM_REST, powf(u, 2.1f));
    } else {
        gy = GEM_REST;
        // Landing squash, out over a beat and a half.
        const float u = seg(t, T_DROP_B, T_LAND_B);
        if (u < 1.0f) {
            const float k = (1.0f - u) * cosf(u * 9.0f);
            sy = GEM_SC * (1.0f - 0.10f * k);
            sx = GEM_SC * (1.0f + 0.07f * k);
        }
        gap   = 54.0f * ease(seg(t, T_CRK_A, T_CRK_B));
        gap  += 90.0f * ease(seg(t, T_GEM_A, T_GEM_B));   // and then let go
        alpha = 1.0f - ease(seg(t, T_GEM_A, T_GEM_B));
    }

    if (alpha > 0.004f) {
        drawHalf(r, GEM_X - gap * 0.5f, gy, sx, sy, front, true,  alpha);
        drawHalf(r, GEM_X + gap * 0.5f, gy, sx, sy, front, false, alpha);
    }

    drawImpact(r, t);
    drawShards(r, t);
    drawIcon(r, icon, t);

    SDL_RenderSetScale(r, 1.0f, 1.0f);
    SDL_SetRenderTarget(r, prevTarget);
    if (g_rt) SDL_RenderCopy(r, g_rt, nullptr, nullptr);
}

}  // namespace reveal
