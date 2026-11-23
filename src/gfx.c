// gfx.c - colour ramps, the pen cache, 2D/3D line primitives and the camera.
//
// Every line is drawn with R2_MERGEPEN (destination OR pen), so a dim halo never darkens a
// bright line already under it and crossing lines brighten instead of overwriting.

#include "vektor.h"

#define MAXW 10    // widest pen in pixels

static COLORREF g_ramp[NRAMP][NLEVEL];
static HPEN g_pens[NRAMP][NLEVEL][MAXW + 1];   // created on first use, kept until exit

static COLORREF mix(COLORREF a, COLORREF b, float t) {
    return RGB((int)lerpf(GetRValue(a), GetRValue(b), t),
               (int)lerpf(GetGValue(a), GetGValue(b), t),
               (int)lerpf(GetBValue(a), GetBValue(b), t));
}

static void build_ramp(int r, COLORREF base) {
    int hue = (int)(HUE * (NLEVEL - 1) + 0.5f);
    for (int l = 0; l < NLEVEL; l++) {
        if (l <= hue) {
            float t = powf((float)l / (float)hue, 1.5f);
            g_ramp[r][l] = mix(RGB(0, 0, 0), base, t);
        } else {
            float t = (float)(l - hue) / (float)(NLEVEL - 1 - hue);
            g_ramp[r][l] = mix(base, RGB(255, 255, 255), t * 0.8f);
        }
    }
}

static void build_fire(int r) {
    // black -> deep red -> red -> orange -> yellow -> white
    static const struct { float at; COLORREF c; } stops[] = {
        { 0.00f, RGB(0, 0, 0) },     { 0.20f, RGB(70, 0, 0) },     { 0.45f, RGB(220, 30, 0) },
        { 0.68f, RGB(255, 120, 0) }, { 0.85f, RGB(255, 220, 50) }, { 1.00f, RGB(255, 255, 230) },
    };
    for (int l = 0; l < NLEVEL; l++) {
        float x = (float)l / (NLEVEL - 1);
        int k = 0;
        while (k < 4 && x > stops[k + 1].at) k++;
        float t = (x - stops[k].at) / (stops[k + 1].at - stops[k].at);
        g_ramp[r][l] = mix(stops[k].c, stops[k + 1].c, sat(t));
    }
}

void gfx_startup(void) {
    build_ramp(R_CYAN, RGB(0, 230, 255));
    build_ramp(R_MAGENTA, RGB(255, 0, 230));
    build_ramp(R_YELLOW, RGB(255, 240, 0));
    build_ramp(R_GREEN, RGB(40, 255, 60));
    build_ramp(R_RED, RGB(255, 30, 30));
    build_ramp(R_WHITE, RGB(200, 210, 255));
    build_ramp(R_ORANGE, RGB(255, 130, 0));
    build_fire(R_FIRE);
}

void gfx_shutdown(void) {
    for (int r = 0; r < NRAMP; r++)
        for (int l = 0; l < NLEVEL; l++)
            for (int w = 0; w <= MAXW; w++)
                if (g_pens[r][l][w]) { DeleteObject(g_pens[r][l][w]); g_pens[r][l][w] = NULL; }
}

static inline int level_of(float i) {
    int l = (int)(i * (NLEVEL - 1) + 0.5f);
    return l < 0 ? 0 : (l >= NLEVEL ? NLEVEL - 1 : l);
}

COLORREF ramp_color(int ramp, float i) { return g_ramp[ramp][level_of(i)]; }

void gfx_begin(Gfx* g, HDC dc, int w, int h) {
    g->dc = dc;
    g->w = w;
    g->h = h;
    g->cx = w * 0.5f;
    g->cy = h * 0.5f;
    g->unit = h * 0.5f;
    g->aspect = (float)w / (float)h;
    // 1 px at 1080p, 2 px at 1440p, 3 px at 2160p.
    g->lw = h < 1300 ? 1 : (h < 1800 ? 2 : 3);
    g->pen_key = -1;
    PatBlt(dc, 0, 0, w, h, BLACKNESS);
    SetROP2(dc, R2_MERGEPEN);
    SelectObject(dc, GetStockObject(DC_BRUSH));
}

void gfx_end(Gfx* g) {
    SelectObject(g->dc, GetStockObject(BLACK_PEN));
    g->pen_key = -1;
}

// Selects the pen for (ramp, intensity, weight). Returns false if it would be black.
static bool use_pen(Gfx* g, int ramp, float i, int w) {
    int l = level_of(i);
    if (l == 0) return false;
    int px = w * g->lw;
    if (px < 1) px = 1;
    if (px > MAXW) px = MAXW;
    int key = (ramp * NLEVEL + l) * (MAXW + 1) + px;
    if (key == g->pen_key) return true;
    HPEN p = g_pens[ramp][l][px];
    if (!p) p = g_pens[ramp][l][px] = CreatePen(PS_SOLID, px, g_ramp[ramp][l]);
    SelectObject(g->dc, p);
    g->pen_key = key;
    return true;
}

static inline int ipx(float v) {   // clamp far-off coordinates so GDI never sees garbage
    return (int)clampf(v, -60000.0f, 60000.0f);
}

void gx_line(Gfx* g, int ramp, float i, int w, float x0, float y0, float x1, float y1) {
    if (!use_pen(g, ramp, i, w)) return;
    MoveToEx(g->dc, ipx(x0), ipx(y0), NULL);
    LineTo(g->dc, ipx(x1), ipx(y1));
}

void gx_poly(Gfx* g, int ramp, float i, int w, const POINT* p, int n) {
    if (n < 2 || !use_pen(g, ramp, i, w)) return;
    Polyline(g->dc, p, n);
}

void gx_glow_poly(Gfx* g, int ramp, float i, int w, const POINT* p, int n) {
    gx_poly(g, ramp, i * 0.38f, w * 3, p, n);
    gx_poly(g, ramp, i, w, p, n);
}

// ---- hairline batch ------------------------------------------------------------------
// Thin polylines queued by colour and drawn with one PolyPolyline per colour. Used for the
// thousands of particle trails, where per-call and pen-switch overhead dominates.

#define HB_PTS 65536
#define HB_RUNS 12288
#define HB_KEYS (NRAMP * NLEVEL)

static POINT g_hb_pts[HB_PTS], g_hb_out[HB_PTS];
static struct { uint16_t key, n; int at; } g_hb_run[HB_RUNS];
static DWORD g_hb_cnt[HB_RUNS];
static int g_hb_np, g_hb_nr;

void gx_hair(Gfx* g, int ramp, float i, const POINT* p, int n) {
    int l = level_of(i);
    if (n < 2 || l == 0) return;
    if (g_hb_np + n > HB_PTS || g_hb_nr >= HB_RUNS) gx_hair_flush(g);
    memcpy(g_hb_pts + g_hb_np, p, (size_t)n * sizeof *p);
    g_hb_run[g_hb_nr].key = (uint16_t)(ramp * NLEVEL + l);
    g_hb_run[g_hb_nr].n = (uint16_t)n;
    g_hb_run[g_hb_nr].at = g_hb_np;
    g_hb_nr++;
    g_hb_np += n;
}

void gx_hair_flush(Gfx* g) {
    static int first[HB_KEYS + 1];
    static int order[HB_RUNS];
    if (!g_hb_nr) return;
    // counting sort of the runs by colour
    memset(first, 0, sizeof first);
    for (int k = 0; k < g_hb_nr; k++) first[g_hb_run[k].key + 1]++;
    for (int k = 0; k < HB_KEYS; k++) first[k + 1] += first[k];
    for (int k = 0; k < g_hb_nr; k++) order[first[g_hb_run[k].key]++] = k;
    int k = 0;
    while (k < g_hb_nr) {
        int key = g_hb_run[order[k]].key, np = 0, nc = 0;
        for (; k < g_hb_nr && g_hb_run[order[k]].key == key; k++) {
            int r = order[k];
            memcpy(g_hb_out + np, g_hb_pts + g_hb_run[r].at, g_hb_run[r].n * sizeof(POINT));
            np += g_hb_run[r].n;
            g_hb_cnt[nc++] = g_hb_run[r].n;
        }
        int ramp = key / NLEVEL, l = key % NLEVEL;
        HPEN p = g_pens[ramp][l][1];
        if (!p) p = g_pens[ramp][l][1] = CreatePen(PS_SOLID, 1, g_ramp[ramp][l]);
        SelectObject(g->dc, p);
        PolyPolyline(g->dc, g_hb_out, g_hb_cnt, (DWORD)nc);
    }
    g->pen_key = -1;
    g_hb_np = g_hb_nr = 0;
}

void gx_dot(Gfx* g, int ramp, float i, float x, float y, float rpx) {
    int l = level_of(i);
    if (l == 0) return;
    int s = (int)(rpx * 2.0f + 0.5f);
    if (s < 1) s = 1;
    SetDCBrushColor(g->dc, g_ramp[ramp][l]);
    PatBlt(g->dc, ipx(x - s * 0.5f), ipx(y - s * 0.5f), s, s, 0x00FA0089);   // DPo: dest OR brush
}

void gx_ring(Gfx* g, int ramp, float i, int w, float x, float y, float r) {
    if (r < 0.5f || !use_pen(g, ramp, i, w)) return;
    int l = ipx(x - r), t = ipx(y - r), rt = ipx(x + r), b = ipx(y + r);
    Arc(g->dc, l, t, rt, b, rt, (int)y, rt, (int)y);
}

void gx_disc(Gfx* g, int ramp, float i, float x, float y, float r) {
    int l = level_of(i);
    if (l == 0 || r < 0.5f) return;
    SelectObject(g->dc, GetStockObject(NULL_PEN));
    g->pen_key = -1;
    SetDCBrushColor(g->dc, g_ramp[ramp][l]);
    Ellipse(g->dc, ipx(x - r), ipx(y - r), ipx(x + r) + 1, ipx(y + r) + 1);
}

void gx_flash(Gfx* g, int ramp, float i, float alpha) {
    // Adds the colour scaled by alpha to every pixel (dest OR brush). It brightens like a
    // light flash and costs one PatBlt, where AlphaBlend over the full screen took ~15 ms.
    if (alpha <= 0.004f) return;
    COLORREF c = ramp_color(ramp, i);
    float a = sat(alpha);
    SetDCBrushColor(g->dc, RGB((int)(GetRValue(c) * a), (int)(GetGValue(c) * a), (int)(GetBValue(c) * a)));
    PatBlt(g->dc, 0, 0, g->w, g->h, 0x00FA0089);
}

// ---- camera -------------------------------------------------------------------------

void cam_look(Cam* c, V3 pos, V3 target, V3 up_hint, float fov_deg) {
    c->pos = pos;
    c->fwd = vnorm(vsub(target, pos));
    c->right = vnorm(vcross(c->fwd, up_hint));
    c->up = vcross(c->right, c->fwd);
    c->focal = 1.0f / tanf(fov_deg * 0.5f * PI / 180.0f);
    c->znear = 0.05f;
    c->ortho = false;
}

void cam_ortho(Cam* c) {
    c->pos = v3(0, 0, 0);
    c->fwd = v3(0, 0, 1);
    c->right = v3(1, 0, 0);
    c->up = v3(0, 1, 0);
    c->focal = 1.0f;
    c->znear = -1e9f;
    c->ortho = true;
}

bool cam_proj(const Gfx* g, const Cam* c, V3 p, float* x, float* y, float* depth) {
    if (c->ortho) {
        *x = gx_px(g, p.x);
        *y = gx_py(g, p.y);
        if (depth) *depth = 1.0f;
        return true;
    }
    V3 d = vsub(p, c->pos);
    float z = vdot(d, c->fwd);
    if (depth) *depth = z;
    if (z < c->znear) return false;
    float k = c->focal / z;
    *x = gx_px(g, vdot(d, c->right) * k);
    *y = gx_py(g, vdot(d, c->up) * k);
    return true;
}

bool sphere_occludes(V3 cam, V3 center, float r, V3 p) {
    V3 d = vsub(p, cam);
    float len = vlen(d);
    if (len < 1e-6f) return false;
    d = vmul(d, 1.0f / len);
    V3 oc = vsub(cam, center);
    float b = vdot(oc, d), cc = vdot(oc, oc) - r * r;
    float disc = b * b - cc;
    if (disc <= 0) return false;
    float t = -b - sqrtf(disc);
    return t > 1e-4f && t < len - 1e-3f;
}

// Clips segment a-b to the camera's near plane. Returns false if wholly behind.
static bool clip_near(const Cam* c, V3* a, V3* b) {
    if (c->ortho) return true;
    float za = vdot(vsub(*a, c->pos), c->fwd), zb = vdot(vsub(*b, c->pos), c->fwd);
    float n = c->znear;
    if (za < n && zb < n) return false;
    if (za < n) *a = vlerp(*a, *b, (n - za) / (zb - za));
    else if (zb < n) *b = vlerp(*b, *a, (n - zb) / (za - zb));
    return true;
}

void g3_line(Gfx* g, const Cam* c, int ramp, float i, int w, V3 a, V3 b) {
    if (!clip_near(c, &a, &b)) return;
    float x0, y0, x1, y1;
    cam_proj(g, c, a, &x0, &y0, NULL);
    cam_proj(g, c, b, &x1, &y1, NULL);
    gx_line(g, ramp, i, w, x0, y0, x1, y1);
}

void g3_poly(Gfx* g, const Cam* c, int ramp, float i, int w, const V3* p, int n) {
    POINT buf[256];
    int m = 0;
    for (int k = 0; k < n; k++) {
        float x, y;
        if (cam_proj(g, c, p[k], &x, &y, NULL)) {
            buf[m].x = ipx(x);
            buf[m].y = ipx(y);
            if (++m == 256) { gx_poly(g, ramp, i, w, buf, m); buf[0] = buf[m - 1]; m = 1; }
        } else {
            gx_poly(g, ramp, i, w, buf, m);
            m = 0;
        }
    }
    gx_poly(g, ramp, i, w, buf, m);
}

// ---- sphere grid --------------------------------------------------------------------

void sphere_grid_defaults(SphereGrid* s) {
    s->center = v3(0, 0, 0);
    s->radius = 1.0f;
    s->rot = m3_id();
    s->nlat = 11;
    s->nlon = 12;
    s->ramp = R_CYAN;
    s->i = 0.5f;
    s->w = 1;
    s->reveal = 1.0f;
    s->from_equator = false;
    s->clip = false;
    s->clip_n = v3(0, 1, 0);
    s->clip_side = 1.0f;
}

// Draws one great or small circle arc of the sphere as runs of front-facing segments.
static void sphere_curve(Gfx* g, const Cam* c, const SphereGrid* s, const V3* local, int n, float frac) {
    POINT run[96];
    int m = 0;
    int last = (int)(frac * (n - 1) + 0.999f);
    if (last > n - 1) last = n - 1;
    float tail = frac * (n - 1) - (float)(last - 1);   // partial final segment
    for (int k = 0; k < last; k++) {
        V3 a = local[k], b = local[k + 1];
        if (k == last - 1 && tail < 1.0f) b = vnorm(vlerp(a, b, sat(tail)));
        if (s->clip) {
            float da = vdot(a, s->clip_n) * s->clip_side, db = vdot(b, s->clip_n) * s->clip_side;
            if (da < 0 && db < 0) { gx_poly(g, s->ramp, s->i, s->w, run, m); m = 0; continue; }
            if (da < 0) a = vlerp(a, b, da / (da - db));
            else if (db < 0) b = vlerp(a, b, da / (da - db));
        }
        V3 mid = vnorm(vadd(a, b));
        V3 wm = vadd(s->center, vmul(m3_mul(&s->rot, mid), s->radius));
        V3 nrm = m3_mul(&s->rot, mid);
        bool front = vdot(nrm, vsub(c->pos, wm)) > 0.0f;
        if (c->ortho) front = nrm.z <= 0.0f;
        if (!front) { gx_poly(g, s->ramp, s->i, s->w, run, m); m = 0; continue; }
        V3 wa = vadd(s->center, vmul(m3_mul(&s->rot, a), s->radius));
        V3 wb = vadd(s->center, vmul(m3_mul(&s->rot, b), s->radius));
        float xa, ya, xb, yb;
        if (!cam_proj(g, c, wa, &xa, &ya, NULL) || !cam_proj(g, c, wb, &xb, &yb, NULL)) {
            gx_poly(g, s->ramp, s->i, s->w, run, m);
            m = 0;
            continue;
        }
        if (m == 0) { run[0].x = ipx(xa); run[0].y = ipx(ya); m = 1; }
        run[m].x = ipx(xb);
        run[m].y = ipx(yb);
        if (++m == 96) { gx_poly(g, s->ramp, s->i, s->w, run, m); run[0] = run[95]; m = 1; }
    }
    gx_poly(g, s->ramp, s->i, s->w, run, m);
}

void draw_sphere_grid(Gfx* g, const Cam* c, const SphereGrid* s) {
    enum { SEG = 48 };
    V3 pts[SEG + 1];
    if (s->from_equator && s->reveal < 1.0f) {
        float lmax = s->reveal * PI * 0.5f;
        for (int i = 1; i <= s->nlat; i++) {
            float lat = -PI / 2 + PI * (float)i / (float)(s->nlat + 1);
            if (fabsf(lat) > lmax + 1e-4f) continue;
            float y = sinf(lat), r = cosf(lat);
            for (int k = 0; k <= SEG; k++) {
                float a = TAU * (float)k / SEG;
                pts[k] = v3(r * cosf(a), y, r * sinf(a));
            }
            sphere_curve(g, c, s, pts, SEG + 1, 1.0f);
        }
        if (lmax <= 0) return;
        for (int j = 0; j < s->nlon * 2; j++) {   // each meridian as two half-circles
            float a = PI * (float)j / (float)s->nlon;
            V3 e = v3(cosf(a), 0, sinf(a));
            for (int k = 0; k <= SEG / 2; k++) {
                float b = -lmax + 2.0f * lmax * (float)k / (SEG / 2);
                pts[k] = vadd(vmul(e, cosf(b)), v3(0, sinf(b), 0));
            }
            sphere_curve(g, c, s, pts, SEG / 2 + 1, 1.0f);
        }
        return;
    }
    int total = s->nlat + s->nlon;
    float per = 1.0f / (float)total;
    int line = 0;
    for (int i = 1; i <= s->nlat; i++, line++) {
        float frac = s->reveal >= 1.0f ? 1.0f : sat((s->reveal - line * per * 0.6f) / (per * 4.0f));
        if (frac <= 0) continue;
        float lat = -PI / 2 + PI * (float)i / (float)(s->nlat + 1);
        float y = sinf(lat), r = cosf(lat);
        for (int k = 0; k <= SEG; k++) {
            float a = TAU * (float)k / SEG;
            pts[k] = v3(r * cosf(a), y, r * sinf(a));
        }
        sphere_curve(g, c, s, pts, SEG + 1, frac);
    }
    for (int j = 0; j < s->nlon; j++, line++) {
        float frac = s->reveal >= 1.0f ? 1.0f : sat((s->reveal - line * per * 0.6f) / (per * 4.0f));
        if (frac <= 0) continue;
        float a = PI * (float)j / (float)s->nlon;
        V3 e = v3(cosf(a), 0, sinf(a));
        for (int k = 0; k <= SEG; k++) {
            float b = TAU * (float)k / SEG;
            pts[k] = vadd(vmul(e, cosf(b)), v3(0, sinf(b), 0));
        }
        sphere_curve(g, c, s, pts, SEG + 1, frac);
    }
}

// ---- misc ---------------------------------------------------------------------------

static float hashf(uint32_t seed, int i) {
    uint32_t h = seed * 0x9E3779B1u ^ (uint32_t)i * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (float)(h & 0xFFFFFF) / 8388607.5f - 1.0f;
}

float noise1(uint32_t seed, float x, int period) {
    float fl = floorf(x);
    int i = (int)fl;
    float t = x - fl;
    int a = period > 0 ? ((i % period) + period) % period : i;
    int b = period > 0 ? (((i + 1) % period) + period) % period : i + 1;
    return lerpf(hashf(seed, a), hashf(seed, b), smooth(t));
}

void draw_shard(Gfx* g, float x, float y, float size, float rot, uint32_t seed, float heat, float i) {
    float xy[16];
    int n = shard_shape(seed, size, rot, x, y, xy, 8);
    POINT p[9];
    for (int k = 0; k < n; k++) {
        p[k].x = (LONG)gx_px(g, xy[2 * k]);
        p[k].y = (LONG)gx_py(g, xy[2 * k + 1]);
    }
    p[n] = p[0];
    if (heat > 0.05f) gx_poly(g, R_FIRE, heat, 1, p, n + 1);
    gx_poly(g, R_WHITE, i * (1.0f - heat * 0.6f), 1, p, n + 1);
}

int shard_shape(uint32_t seed, float size, float rot, float x, float y, float* out, int max) {
    int n = 5 + (int)((hashf(seed, 99) + 1.0f) * 1.5f);   // 5..7 corners
    if (n > max) n = max;
    for (int k = 0; k < n; k++) {
        float a = rot + TAU * (float)k / n + hashf(seed, k) * 0.45f;
        float r = size * (0.55f + 0.45f * fabsf(hashf(seed, k + 40)));
        out[2 * k] = x + cosf(a) * r;
        out[2 * k + 1] = y + sinf(a) * r * 0.8f;
    }
    return n;
}
