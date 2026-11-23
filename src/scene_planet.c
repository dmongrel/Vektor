// scene_planet.c - Scene 1: Tectonic Planet.
//
// A wireframe planet tears itself apart: fault lines grow and glow, plates pull apart,
// ruptures cascade along the cracks, magma vents erupt, and the planet splits along its
// main fault into two halves that drift apart trailing debris. Outro: the camera dives into
// the gap and the fissure's edge closes into the mouth of the wormhole.

#include "vektor.h"

#define DUR       60.0f
#define T_GROW    2.6f     // sphere grows from a point
#define T_MAIN    5.0f     // main fault starts to open
#define T_RUPT    18.0f    // rupture chains begin
#define T_VENT    22.0f    // vents begin
#define T_SPLIT   44.0f
#define T_OUTRO   53.5f

#define MAXCP   1600
#define MAINN   144
#define MAXVENT 12
#define MAXCHAIN 24

typedef struct {
    V3 p;                  // local, unit sphere
    int prev;              // segment prev -> this; -1 = start of a walk
    float born;
} CrackPt;

typedef struct { int k; float start, dur; bool rung; } Vent;
typedef struct { int k; float at; int left; } Chain;

static Rng R;
static Handoff* H;
static CrackPt cp[MAXCP];
static int ncp;
static Vent vents[MAXVENT];
static int nvent;
static Chain chains[MAXCHAIN];
static float next_chain;
static V3 axis, n_world, n_local;
static V3 tumble_a, tumble_b;
static float omega0;
static Cam cam;
static bool split_flash;
static float debris_clock;

// ---- geometry --------------------------------------------------------------------

static float spin_angle(float t) {
    float ts = T_SPLIT;
    if (t > ts) t = ts;
    return omega0 * (t - t * t / (2.0f * ts));
}

static M3 body_rot(float t) { return m3_axis(axis, spin_angle(t)); }

static float separation(float t) {
    float u = t - T_SPLIT;
    return u <= 0 ? 0 : 0.055f * u + 0.0075f * u * u;
}

// Transform of half `side` (+1/-1) at time t; before the split both are the whole body.
static void half_xform(float t, float side, M3* rot, V3* off) {
    M3 base = body_rot(t);
    if (t <= T_SPLIT) { *rot = base; *off = v3(0, 0, 0); return; }
    float u = t - T_SPLIT;
    M3 tum = m3_axis(side > 0 ? tumble_a : tumble_b, 0.05f * u * side);
    *rot = m3_mulm(&tum, &base);
    *off = vmul(n_world, separation(t) * side);
}

static V3 to_world(const M3* rot, V3 off, V3 p, float lift) {
    return vadd(off, vmul(m3_mul(rot, p), 1.0f + lift));
}

static bool front(V3 world_p, V3 world_n) { return vdot(world_n, vsub(cam.pos, world_p)) > 0; }

// ---- generation ------------------------------------------------------------------

static int add_pt(V3 p, int prev, float born) {
    if (ncp >= MAXCP) return -1;
    cp[ncp].p = vnorm(p);
    cp[ncp].prev = prev;
    cp[ncp].born = born;
    return ncp++;
}

static void random_walk(V3 start, int prev, float born, float step_t, int steps, int depth) {
    V3 p = vnorm(start);
    V3 dir = vnorm(vcross(p, rand_unit(&R)));
    int last = prev;
    float turn = 0;
    for (int s = 0; s < steps && ncp < MAXCP; s++) {
        turn = clampf(turn + rr(&R, -0.25f, 0.25f), -0.5f, 0.5f);
        dir = vrot(dir, p, turn * 0.6f);
        V3 np = vnorm(vmad(p, dir, 0.045f));
        dir = vnorm(vsub(dir, vmul(np, vdot(dir, np))));
        // keep secondary cracks off the split plane so the halves stay clean
        float side = vdot(p, n_local);
        if (fabsf(vdot(np, n_local)) < 0.04f) np = vnorm(vmad(np, n_local, side > 0 ? 0.04f : -0.04f));
        p = np;
        born += step_t;
        int k = add_pt(p, last, born);
        if (k < 0) return;
        last = k;
        if (depth < 2 && rf(&R) < 0.05f) random_walk(p, k, born, step_t * 1.1f, steps / 2, depth + 1);
    }
}

static void build_cracks(void) {
    ncp = 0;
    // Main fault: a jagged great circle around the split plane, opening from two seeds.
    V3 u = vperp(n_local), v = vcross(n_local, u);
    uint32_t seed = (uint32_t)rng_next(&R);
    float a0 = rr(&R, 0, TAU);
    for (int k = 0; k < MAINN; k++) {
        float a = TAU * k / MAINN;
        float j = noise1(seed, a * 3.0f, 0) * 0.05f + noise1(seed + 7, a * 11.0f, 0) * 0.02f;
        V3 p = vadd(vadd(vmul(u, cosf(a)), vmul(v, sinf(a))), vmul(n_local, j));
        float d1 = fabsf(remainderf(a - a0, TAU)), d2 = fabsf(remainderf(a - a0 - PI, TAU));
        add_pt(p, k == 0 ? MAINN - 1 : k - 1, T_MAIN + fminf(d1, d2) * 4.2f);
    }
    // Secondary faults: branching walks, some from the main fault, some on their own.
    for (int c = 0; c < 16; c++) {
        float born = rr(&R, 7.0f, 30.0f);
        V3 start;
        int prev = -1;
        if (c < 8) {
            prev = ri(&R, MAINN);
            start = cp[prev].p;
            if (cp[prev].born > born) born = cp[prev].born + 0.5f;
        } else {
            start = rand_unit(&R);
        }
        random_walk(start, prev, born, rr(&R, 0.10f, 0.18f), 22 + ri(&R, 30), 0);
    }
}

// ---- scene interface -------------------------------------------------------------

static void init(Handoff* h, uint64_t seed) {
    H = h;
    R.s = seed;
    axis = vnorm(v3(rr(&R, 0.1f, 0.35f), 1.0f, rr(&R, -0.2f, 0.2f)));
    omega0 = rr(&R, 0.28f, 0.4f);
    n_world = vnorm(v3(1.0f, rr(&R, -0.2f, 0.25f), 0.38f));
    M3 rs = body_rot(T_SPLIT);
    n_local = m3_tmul(&rs, n_world);
    tumble_a = vnorm(vcross(n_world, v3(0, 1, 0.3f)));
    tumble_b = vnorm(vcross(n_world, v3(0.2f, 1, -0.4f)));
    build_cracks();

    nvent = 0;
    for (int k = 0; k < MAXVENT; k++) {
        int idx = ri(&R, ncp);
        if (cp[idx].born > T_SPLIT - 6) continue;
        vents[nvent].k = idx;
        vents[nvent].start = fmaxf(cp[idx].born + 2.0f, rr(&R, T_VENT, T_SPLIT - 4.0f));
        vents[nvent].dur = rr(&R, 3.0f, 7.0f);
        vents[nvent].rung = false;
        nvent++;
    }
    for (int k = 0; k < MAXCHAIN; k++) chains[k].left = 0;
    next_chain = T_RUPT;
    split_flash = false;
    debris_clock = 0;

    h->jag_seed = (uint32_t)rng_next(&R);
    h->has12 = true;
}

static void start_chain(int k, int len, float at) {
    for (int c = 0; c < MAXCHAIN; c++)
        if (chains[c].left == 0) { chains[c].k = k; chains[c].at = at; chains[c].left = len; return; }
}

static void rupture(V3 wp, V3 wn, float big) {
    fx_burst(&R, wp, vmul(wn, 0.35f * big), (int)(16 * big), 0.15f, 0.65f * big, 1.4f, R_FIRE, 1.0f, GRAV_CENTER);
    fx_ring(wp, wn, 0.01f, 0.16f * big, 0.8f, R_FIRE, 0.9f, RING_PLANE);
    fx_ring(wp, wn, 0.03f * big, 0.0f, 0.2f, R_WHITE, 0.9f, RING_DISC);
}

static void update(float t, float dt) {
    fx_world.center = v3(0, 0, 0);
    fx_world.gm = t < T_SPLIT ? 0.55f : 0.0f;
    fx_world.kill_r = t < T_SPLIT ? 0.985f : 0.0f;
    fx_world.fade = 1.0f - ramp01(t, 57.0f, 59.6f);
    V3 dummy;
    while (fx_pop_impact(&dummy)) {}

    M3 rot = body_rot(t);
    float stress = ramp01(t, T_MAIN, T_SPLIT);

    // Rupture chains, more and more often as the stress builds.
    if (t >= next_chain && t < T_SPLIT - 0.5f) {
        int k = ri(&R, ncp);
        for (int tries = 0; tries < 8 && cp[k].born > t; tries++) k = ri(&R, ncp);
        if (cp[k].born <= t) start_chain(k, 4 + ri(&R, 8), t);
        next_chain = t + lerpf(2.6f, 0.45f, stress) * rr(&R, 0.6f, 1.3f);
    }
    for (int c = 0; c < MAXCHAIN; c++) {
        Chain* ch = &chains[c];
        if (ch->left <= 0 || t < ch->at) continue;
        V3 lp = cp[ch->k].p;
        V3 wn = m3_mul(&rot, lp);
        if (t < T_SPLIT) rupture(wn, wn, 0.7f + 0.6f * stress);
        int k = ch->k;
        for (int s = 0; s < 3 && cp[k].prev >= 0; s++) k = cp[k].prev;
        ch->left = (k == ch->k) ? 0 : ch->left - 1;   // reached the root of its crack
        ch->k = k;
        ch->at = t + rr(&R, 0.07f, 0.16f);
    }

    // Vents: fountains of magma arcing back down under the planet's gravity.
    for (int v = 0; v < nvent && t < T_SPLIT; v++) {
        Vent* vt = &vents[v];
        if (t < vt->start || t > vt->start + vt->dur) continue;
        V3 lp = cp[vt->k].p;
        V3 wn = m3_mul(&rot, lp);
        if (!vt->rung) {
            vt->rung = true;
            fx_ring(vmul(wn, 1.01f), wn, 0.02f, 0.3f, 1.2f, R_ORANGE, 0.9f, RING_PLANE);
            fx_ring(vmul(wn, 1.01f), wn, 0.045f, 0.0f, 0.3f, R_WHITE, 0.9f, RING_DISC);
        }
        float rate = 70.0f * (1.0f - lin01(t, vt->start + vt->dur - 1.0f, vt->start + vt->dur));
        if (rf(&R) < rate * dt * 2.0f) {
            V3 tan = vperp(wn);
            tan = vrot(tan, wn, rr(&R, 0, TAU));
            PSpawn s = { 0 };
            s.p = vmul(wn, 1.01f);
            s.v = vadd(vmul(wn, rr(&R, 0.55f, 1.05f)), vmul(tan, rr(&R, 0.0f, 0.3f)));
            s.ttl = rr(&R, 1.5f, 2.6f);
            s.heat = rr(&R, 0.85f, 1.0f);
            s.drag = 0.15f;
            s.size = rr(&R, 0.8f, 1.3f);
            s.ramp = R_FIRE;
            s.grav = GRAV_CENTER;
            s.trail_len = 10;
            fx_spawn(&s);
        }
    }

    // The split.
    if (t >= T_SPLIT && !split_flash) {
        split_flash = true;
        for (int k = 0; k < MAINN; k += 6) {
            V3 wp = m3_mul(&rot, cp[k].p);
            fx_burst(&R, wp, v3(0, 0, 0), 10, 0.2f, 0.9f, 2.2f, R_FIRE, 1.0f, GRAV_NONE);
        }
        fx_ring(v3(0, 0, 0), n_world, 0.9f, 3.2f, 1.6f, R_WHITE, 1.0f, RING_PLANE);
        fx_ring(v3(0, 0, 0), n_world, 0.6f, 2.4f, 1.9f, R_FIRE, 1.0f, RING_PLANE);
    }
    // Debris trailing from the torn rims.
    if (t > T_SPLIT && t < T_OUTRO + 2.0f) {
        debris_clock += dt * lerpf(160.0f, 40.0f, lin01(t, T_SPLIT, T_OUTRO));
        while (debris_clock >= 1.0f) {
            debris_clock -= 1.0f;
            float side = rf(&R) < 0.5f ? 1.0f : -1.0f;
            M3 hr;
            V3 off;
            half_xform(t, side, &hr, &off);
            V3 rim = cp[ri(&R, MAINN)].p;
            V3 wp = to_world(&hr, off, vmul(rim, rr(&R, 0.75f, 1.0f)), 0);
            float sp = (0.055f + 0.015f * (t - T_SPLIT)) * side;
            PSpawn s = { 0 };
            s.p = wp;
            s.v = vadd(vmul(n_world, sp * rr(&R, 0.2f, 0.7f)), vmul(rand_unit(&R), rr(&R, 0.02f, 0.18f)));
            s.ttl = rr(&R, 1.5f, 3.5f);
            s.heat = rr(&R, 0.75f, 1.0f);
            s.drag = 0.05f;
            s.size = rr(&R, 0.6f, 1.4f);
            s.ramp = R_FIRE;
            s.grav = GRAV_NONE;
            s.trail_len = 10;
            fx_spawn(&s);
        }
    }
}

// ---- drawing ---------------------------------------------------------------------

static void draw_cracks(Gfx* g, float t, float side) {
    M3 rot;
    V3 off;
    half_xform(t, side, &rot, &off);
    float stress = ramp01(t, T_MAIN, T_SPLIT);
    float cool = 1.0f - 0.55f * lin01(t, T_SPLIT, T_SPLIT + 8.0f);
    float gap = 0.004f + 0.022f * stress * stress;
    for (int k = 0; k < ncp; k++) {
        int pk = cp[k].prev;
        if (pk < 0 || t < cp[pk].born) continue;
        V3 a = cp[pk].p, b = cp[k].p;
        bool is_main = k < MAINN;
        if (t > T_SPLIT) {
            // after the split each half keeps its own cracks; the main fault is now the rim
            if (!is_main && vdot(vadd(a, b), n_local) * side < 0) continue;
        } else if (side < 0) {
            continue;
        }
        float grow = sat((t - cp[pk].born) / fmaxf(cp[k].born - cp[pk].born, 0.02f));
        if (grow < 1) b = vnorm(vlerp(a, b, grow));
        V3 mid = vnorm(vadd(a, b));
        V3 wm = to_world(&rot, off, mid, 0);
        if (!front(wm, m3_mul(&rot, mid))) continue;
        float age = t - cp[k].born;
        float flick = 0.85f + 0.15f * noise1((uint32_t)k, t * 6.0f, 0);
        float heat = clampf(0.35f + 0.25f * sat(age * 0.5f) + 0.45f * stress, 0, 1) * flick * cool;
        if (is_main && t > T_SPLIT - 2.0f && t < T_SPLIT) heat = 1.0f;
        // plate edges pulled apart, with magma between them
        V3 side_v = vnorm(vcross(vsub(b, a), mid));
        float e = (is_main && t > T_SPLIT) ? 0 : gap;
        if (e > 0.006f) {
            V3 a1 = to_world(&rot, off, vnorm(vmad(a, side_v, e)), 0.002f);
            V3 b1 = to_world(&rot, off, vnorm(vmad(b, side_v, e)), 0.002f);
            V3 a2 = to_world(&rot, off, vnorm(vmad(a, side_v, -e)), 0.002f);
            V3 b2 = to_world(&rot, off, vnorm(vmad(b, side_v, -e)), 0.002f);
            g3_line(g, &cam, R_CYAN, 0.62f, 1, a1, b1);
            g3_line(g, &cam, R_CYAN, 0.62f, 1, a2, b2);
        }
        V3 wa = to_world(&rot, off, a, 0.003f), wb = to_world(&rot, off, b, 0.003f);
        g3_line(g, &cam, R_FIRE, heat * 0.45f, 3, wa, wb);
        g3_line(g, &cam, R_FIRE, heat, 1, wa, wb);
    }
}

// The molten cut face of one half: the jagged rim and cooling rings inside it.
static void draw_cut_face(Gfx* g, float t, float side) {
    M3 rot;
    V3 off;
    half_xform(t, side, &rot, &off);
    V3 face_n = vmul(m3_mul(&rot, n_local), -side);
    if (!front(off, face_n)) return;
    float u = t - T_SPLIT;
    float heat = 1.0f - 0.45f * sat(u / 9.0f);
    static const float scales[] = { 1.0f, 0.78f, 0.55f, 0.32f, 0.12f };
    V3 pts[MAINN + 1];
    for (int r = 0; r < 5; r++) {
        float sc = scales[r];
        for (int k = 0; k <= MAINN; k++) {
            V3 p = cp[k % MAINN].p;
            V3 flat = vsub(p, vmul(n_local, vdot(p, n_local)));
            float wob = 1.0f + 0.04f * noise1((uint32_t)r * 31u, (float)k * 0.2f + t * 1.5f, 0);
            pts[k] = to_world(&rot, off, vmul(flat, sc * (r ? wob : 1.0f)), 0);
        }
        float i = heat * (r == 0 ? 0.95f : 0.8f - 0.08f * r) * (0.85f + 0.15f * sinf(t * 5.0f + r));
        g3_poly(g, &cam, R_FIRE, i * 0.4f, 3, pts, MAINN + 1);
        g3_poly(g, &cam, R_FIRE, i, 1, pts, MAINN + 1);
    }
}

static void draw_half(Gfx* g, float t, float side, float intensity) {
    M3 rot;
    V3 off;
    half_xform(t, side, &rot, &off);
    SphereGrid s;
    sphere_grid_defaults(&s);
    s.center = off;
    s.rot = rot;
    s.ramp = R_CYAN;
    s.i = intensity;
    if (t > T_SPLIT) {
        s.clip = true;
        s.clip_n = n_local;
        s.clip_side = side;
    }
    draw_sphere_grid(g, &cam, &s);
}

static void draw(Gfx* g, float t) {
    // Camera: fixed, then the dive into the gap.
    V3 p0 = v3(0, 0.9f, 3.6f);
    float d = ramp01(t, T_OUTRO, DUR - 0.4f);
    float dive = d * d;
    V3 pos = vlerp(p0, v3(0.0f, 0.05f, 0.55f), dive);
    V3 tgt = vlerp(v3(0, 0, 0), v3(0, -0.05f, -2.0f), dive);
    cam_look(&cam, pos, tgt, v3(0, 1, 0), 45.0f);

    // Intro: a bright point swells into the planet, one grid line at a time.
    if (t < T_GROW + 2.0f) {
        float i = 1.0f - lin01(t, 0.4f, 2.2f);
        gx_dot(g, R_WHITE, i, g->cx, g->cy, (3.0f + 4.0f * sat(t * 2.0f)) * g->lw);
        gx_ring(g, R_WHITE, i * 0.5f, 2, g->cx, g->cy, (5.0f + 10.0f * sat(t)) * g->lw);
    }
    if (t < 4.5f) {
        float grow = smooth(t / T_GROW);
        M3 rot = body_rot(t);
        SphereGrid s;
        sphere_grid_defaults(&s);
        s.rot = rot;
        s.radius = fmaxf(grow, 0.001f);
        s.reveal = sat((t - 0.2f) / 4.0f);
        s.i = 0.45f;
        draw_sphere_grid(g, &cam, &s);
    } else if (t <= T_SPLIT) {
        draw_half(g, t, 1.0f, 0.42f);
        draw_cracks(g, t, 1.0f);
    } else {
        float dim = 0.42f * (1.0f - 0.4f * d);
        draw_half(g, t, 1.0f, dim);
        draw_half(g, t, -1.0f, dim);
        draw_cut_face(g, t, 1.0f);
        draw_cut_face(g, t, -1.0f);
        draw_cracks(g, t, 1.0f);
        draw_cracks(g, t, -1.0f);
    }

    fx_draw_rings(g, &cam);
    fx_draw(g, &cam);

    // The split flash.
    if (t > T_SPLIT && t < T_SPLIT + 0.8f) gx_flash(g, R_WHITE, 1.0f, 0.4f * (1.0f - (t - T_SPLIT) / 0.8f));
    if (t > T_SPLIT - 1.5f && t < T_SPLIT) gx_flash(g, R_FIRE, 0.6f, 0.12f * lin01(t, T_SPLIT - 1.5f, T_SPLIT));

    // Outro: the fissure's edge closes into a ring and more rings stack up behind it,
    // ending exactly on the wormhole's first frame.
    if (t > T_OUTRO + 1.0f) {
        enum { NP = 48 };
        POINT tgt_pts[NP + 1], pts[NP + 1];
        for (int r = 0; r < MOUTH_RINGS; r++) {
            float i = wormhole_mouth_points(g, H, r, tgt_pts, NP);
            tgt_pts[NP] = tgt_pts[0];
            float st = T_OUTRO + 1.6f + 0.42f * r;
            float u = ramp01(t, st, fminf(st + 1.6f, DUR - 0.05f));
            if (u <= 0) continue;
            // ring 0 closes in from beyond the screen; the rest open out from the centre
            float scale = r == 0 ? lerpf(3.0f, 1.0f, u) : u;
            for (int k = 0; k <= NP; k++) {
                pts[k].x = (LONG)(g->cx + (tgt_pts[k].x - g->cx) * scale);
                pts[k].y = (LONG)(g->cy + (tgt_pts[k].y - g->cy) * scale);
            }
            float ii = i * (r == 0 ? 1.0f : u);
            gx_poly(g, R_FIRE, ii * 0.4f, 3, pts, NP + 1);
            gx_poly(g, R_FIRE, ii, 1, pts, NP + 1);
        }
    }
}

const SceneDef scene_planet = { "Tectonic Planet", DUR, init, update, draw };
