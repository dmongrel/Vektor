// scene_wormhole.c - Scene 2: Wormhole Transit.
//
// A chase camera follows a ship down a tunnel of rings that grows ahead of it and splits.
// Unchosen branches implode (shrink, twist, invert colour, burst). The ship fires at glowing
// growth nodes, setting off chain explosions of the rings ahead, which then regrow.
// Intro: the jagged fire-coloured mouth left by Scene 1 smooths into a cyan tunnel.
// Outro: a last shot hits the far end, the tunnel collapses back toward the camera ring by
// ring, and the one ring left tilts into the equator of Scene 3's sphere.

#include "vektor.h"

#define DUR      60.0f
#define S        0.7f      // ring spacing
#define NP       48        // points per ring (Scene 1's outro draws the mouth with 48 too)
#define CAP      512
#define AHEAD    46        // rings kept ahead of the camera
#define MOUTH0   3         // first mouth ring index
#define CAM0     1.0f      // camera ring parameter at t = 0
#define CAM_UP   0.35f
#define FOV      70.0f
#define SPEED    6.0f      // rings per second
#define T_LAST   51.5f     // last shot
#define T_STOP   54.0f
#define T_MORPH0 57.0f
#define T_MORPH1 59.6f
#define MAXBR    6
#define BRN      18
#define MAXSHOT  8

typedef struct {
    V3 c, t, n, b;
    float r, born;         // full radius, birth time (grows in over 0.5 s)
    float boom;            // chain explosion time, <0 none
    float collapse;        // implosion start, <0 none
    float jag;             // mouth jaggedness weight
    bool node, targeted, boomed, gone, hidden;
} Ring;

typedef struct {
    int junction;
    Ring r[BRN];
    bool active, collapsing;
} Branch;

typedef struct { float s, target; bool alive, last; } Shot;

static Rng R;
static Handoff* H;
static Ring rings[CAP];
static int head;           // next ring index to generate
static Branch br[MAXBR];
static Shot shots[MAXSHOT];
static float s_cam, s_ship, speed, s_brake;
static float yaw_rate, pitch_rate;
static int next_junction, next_node;
static int kink_left;
static V3 kink_axis;
static float last_hit_t;
static int last_ring;      // the ring that survives the outro
static bool last_fired;
static Cam cam;

#define RING(i) (rings[(unsigned)(i) % CAP])

// ---- shared mouth geometry (also used by Scene 1's outro) ---------------------------

static float depth_intensity(float d) {
    float f = 1.0f - sat((d - 1.0f) / 24.0f);
    return 0.9f * powf(f, 1.3f);
}

static float jag_radius(uint32_t seed, int idx, float a, float jag) {
    if (jag <= 0) return 1.0f;
    float x = a / TAU * 9.0f;
    return 1.0f + jag * (0.13f * noise1(seed + (uint32_t)idx * 7u, x, 9) +
                         0.06f * noise1(seed + 999u + (uint32_t)idx, x * 3.0f, 27));
}

static void start_camera(Cam* c) {
    V3 pos = v3(0, CAM_UP, CAM0 * S);
    cam_look(c, pos, v3(0, 0, (CAM0 + 6.0f) * S), v3(0, 1, 0), FOV);
}

float wormhole_mouth_points(const Gfx* g, const Handoff* h, int ring, POINT* out, int n) {
    Cam c;
    start_camera(&c);
    int idx = MOUTH0 + ring;
    V3 ctr = v3(0, 0, idx * S), nn = v3(0, 1, 0), bb = v3(-1, 0, 0);
    for (int k = 0; k < n; k++) {
        float a = TAU * k / n;
        float r = jag_radius(h->jag_seed, idx, a, 1.0f);
        V3 p = vadd(ctr, vadd(vmul(nn, cosf(a) * r), vmul(bb, sinf(a) * r)));
        float x, y;
        cam_proj(g, &c, p, &x, &y, NULL);
        out[k].x = (LONG)x;
        out[k].y = (LONG)y;
    }
    return depth_intensity(idx * S - CAM0 * S);
}

// ---- path ---------------------------------------------------------------------------

static void gen_ring(float t) {
    int i = head++;
    Ring* r = &RING(i);
    memset(r, 0, sizeof *r);
    r->boom = r->collapse = -1;
    r->r = 1.0f;
    if (i == 0) {
        r->c = v3(0, 0, 0);
        r->t = v3(0, 0, 1);
        r->n = v3(0, 1, 0);
        r->b = v3(-1, 0, 0);
    } else {
        Ring* p = &RING(i - 1);
        r->c = vmad(p->c, p->t, S);
        V3 t2 = p->t;
        if (i > MOUTH0 + 12) {
            yaw_rate = clampf(yaw_rate * 0.97f + rr(&R, -0.012f, 0.012f), -0.06f, 0.06f);
            pitch_rate = clampf(pitch_rate * 0.97f + rr(&R, -0.012f, 0.012f), -0.06f, 0.06f);
            t2 = vrot(t2, p->n, yaw_rate);
            t2 = vrot(t2, p->b, pitch_rate);
            if (kink_left > 0) { t2 = vrot(t2, kink_axis, 0.065f); kink_left--; }
        }
        r->t = vnorm(t2);
        r->n = vnorm(vsub(p->n, vmul(r->t, vdot(p->n, r->t))));
        r->b = vcross(r->t, r->n);
    }
    r->born = (i < MOUTH0 + MOUTH_RINGS) ? -10.0f : t;
    r->hidden = i < MOUTH0;
    r->jag = (i >= MOUTH0 && i < MOUTH0 + MOUTH_RINGS) ? 1.0f : 0.0f;

    // growth nodes
    if (i >= next_node && i > MOUTH0 + 16) {
        r->node = true;
        next_node = i + 14 + ri(&R, 16);
    }
    // junctions: the main path kinks one way, a branch the other
    if (i >= next_junction && i > MOUTH0 + 20 && t < T_LAST - 8.0f) {
        next_junction = i + 34 + ri(&R, 18);
        int nb = rf(&R) < 0.35f ? 2 : 1;
        float phi = rr(&R, 0, TAU);
        kink_axis = vnorm(vadd(vmul(r->n, cosf(phi)), vmul(r->b, sinf(phi))));
        kink_left = 8;
        for (int b = 0; b < nb; b++) {
            Branch* B = NULL;
            for (int k = 0; k < MAXBR; k++) if (!br[k].active) { B = &br[k]; break; }
            if (!B) break;
            V3 ax = b == 0 ? kink_axis : vrot(kink_axis, r->t, rr(&R, 1.6f, 2.4f));
            float bend = b == 0 ? -0.07f : 0.07f;
            B->active = true;
            B->collapsing = false;
            B->junction = i;
            Ring prev = *r;
            for (int k = 0; k < BRN; k++) {
                Ring* q = &B->r[k];
                memset(q, 0, sizeof *q);
                q->boom = q->collapse = -1;
                q->r = 1.0f;
                q->c = vmad(prev.c, prev.t, S);
                V3 tt = k < 10 ? vrot(prev.t, ax, bend) : prev.t;
                tt = vrot(tt, prev.n, rr(&R, -0.02f, 0.02f));
                q->t = vnorm(tt);
                q->n = vnorm(vsub(prev.n, vmul(q->t, vdot(prev.n, q->t))));
                q->b = vcross(q->t, q->n);
                q->born = t + k * 0.03f;
                prev = *q;
            }
        }
    }
}

// Interpolated path point at ring parameter s.
static void path_at(float s, V3* c, V3* t, V3* n) {
    int i = (int)floorf(s);
    float f = s - (float)i;
    if (i < 0) { i = 0; f = 0; }
    if (i + 1 >= head) { i = head - 2; f = 1; }
    Ring *a = &RING(i), *b = &RING(i + 1);
    *c = vlerp(a->c, b->c, f);
    *t = vnorm(vlerp(a->t, b->t, f));
    *n = vnorm(vlerp(a->n, b->n, f));
}

// ---- scene interface ----------------------------------------------------------------

static void init(Handoff* h, uint64_t seed) {
    H = h;
    R.s = seed;
    if (!h->has12) h->jag_seed = (uint32_t)rng_next(&R);
    head = 0;
    memset(br, 0, sizeof br);
    memset(shots, 0, sizeof shots);
    yaw_rate = pitch_rate = 0;
    kink_left = 0;
    next_junction = MOUTH0 + 30;
    next_node = MOUTH0 + 18;
    s_cam = CAM0;
    s_ship = CAM0 - 0.5f;
    speed = 0;
    last_hit_t = -1;
    last_ring = -1;
    last_fired = false;
    while (head < (int)s_cam + AHEAD) gen_ring(0);
}

static void burst_ring(const Ring* r, float scale, int ramp, int n) {
    for (int k = 0; k < n; k++) {
        float a = rr(&R, 0, TAU);
        V3 radial = vadd(vmul(r->n, cosf(a)), vmul(r->b, sinf(a)));
        PSpawn s = { 0 };
        s.p = vadd(r->c, vmul(radial, r->r * scale));
        s.v = vadd(vmul(radial, rr(&R, 0.4f, 1.6f)), vmul(r->t, rr(&R, -0.5f, 1.2f)));
        s.ttl = rr(&R, 0.5f, 1.1f);
        s.heat = 1.0f;
        s.drag = 1.2f;
        s.size = 1.0f;
        s.ramp = (uint8_t)ramp;
        s.grav = GRAV_NONE;
        s.trail_len = 6;
        fx_spawn(&s);
    }
}

static void fire(float target, bool last) {
    for (int k = 0; k < MAXSHOT; k++) {
        if (shots[k].alive) continue;
        shots[k].alive = true;
        shots[k].s = s_ship + 0.4f;
        shots[k].target = target;
        shots[k].last = last;
        return;
    }
}

static void update(float t, float dt) {
    fx_world.fade = 1.0f - ramp01(t, T_MORPH0 - 0.5f, T_MORPH0 + 0.8f);

    // speed: pick up from Scene 1's dive, cruise, then brake linearly to a known stop
    if (t < T_LAST) {
        speed = t < 2.5f ? lerpf(2.5f, SPEED, smooth(t / 2.5f)) : SPEED;
        s_cam += speed * dt;
        s_brake = s_cam;
    } else {
        float D = T_STOP - T_LAST, u = sat((t - T_LAST) / D);
        speed = SPEED * (1.0f - u);
        s_cam = s_brake + SPEED * D * (u - 0.5f * u * u);
    }
    float lead = lerpf(-0.5f, 3.2f, smooth(t / 2.2f));
    s_ship = s_cam + lead;

    if (t < T_LAST) while (head < (int)s_cam + AHEAD) gen_ring(t);

    // ship fires at nodes coming up
    if (t < T_LAST) {
        for (int i = (int)s_ship + 8; i < (int)s_ship + 16 && i < head; i++) {
            Ring* r = &RING(i);
            if (r->node && !r->targeted) { r->targeted = true; fire((float)i, false); break; }
        }
    }
    if (t >= T_LAST && !last_fired) { last_fired = true; fire((float)(head - 2), true); }

    for (int k = 0; k < MAXSHOT; k++) {
        Shot* sh = &shots[k];
        if (!sh->alive) continue;
        sh->s += (speed + 22.0f) * dt;
        if (sh->s < sh->target) continue;
        sh->alive = false;
        int ti = (int)sh->target;
        if (sh->last) {
            // the far end blows, and the collapse runs back toward the camera
            last_hit_t = t;
            // the camera stops at s_brake + SPEED * BRAKE / 2; keep a ring a little past that
            last_ring = (int)(s_brake + SPEED * (T_STOP - T_LAST) * 0.5f) + 3;
            int nearest = (int)s_cam - 1 < 0 ? 0 : (int)s_cam - 1;
            for (int i = ti; i >= nearest; i--)
                if (i != last_ring) RING(i).collapse = t + (ti - i) * 0.075f;
            burst_ring(&RING(ti), 0.3f, R_WHITE, 40);
        } else {
            for (int j = 0; j < 9 && ti + j < head; j++) RING(ti + j).boom = t + j * 0.07f;
        }
    }

    // chain explosions
    for (int i = (int)s_cam - 2; i < head; i++) {
        if (i < 0) continue;
        Ring* r = &RING(i);
        if (r->boom >= 0 && t >= r->boom && !r->boomed) {
            r->boomed = true;
            burst_ring(r, 1.0f, R_YELLOW, 16);
            fx_ring(r->c, r->t, 0.2f, 1.6f, 0.6f, R_WHITE, 1.0f, RING_PLANE);
        }
        if (r->collapse >= 0 && !r->gone && t >= r->collapse + 0.5f) {
            r->gone = true;
            burst_ring(r, 0.1f, R_MAGENTA, 12);
        }
    }

    // branches implode once the ship has committed past the junction
    for (int b = 0; b < MAXBR; b++) {
        Branch* B = &br[b];
        if (!B->active) continue;
        if (!B->collapsing && s_ship > B->junction + 1.5f) {
            B->collapsing = true;
            for (int k = 0; k < BRN; k++) B->r[k].collapse = t + 0.25f + k * 0.06f;
        }
        bool any = false;
        for (int k = 0; k < BRN; k++) {
            Ring* q = &B->r[k];
            if (q->gone) continue;
            any = true;
            if (q->collapse >= 0 && t >= q->collapse + 0.55f) {
                q->gone = true;
                burst_ring(q, 0.08f, k & 1 ? R_MAGENTA : R_WHITE, 14);
                fx_ring(q->c, q->t, 0.05f, 0.5f, 0.4f, R_RED, 0.9f, RING_SCREEN);
            }
        }
        if (!any || B->junction < (int)s_cam - 4) B->active = false;
    }

    // exhaust
    if (t < T_STOP + 1.0f && rf(&R) < 0.7f) {
        V3 c, tt, n;
        path_at(s_ship, &c, &tt, &n);
        PSpawn s = { 0 };
        s.p = vadd(vmad(c, n, -0.28f), vmul(tt, -0.2f));
        s.v = vmul(tt, speed * S * 0.6f);
        s.ttl = 0.35f;
        s.heat = 0.9f;
        s.drag = 3.0f;
        s.size = 0.8f;
        s.ramp = R_FIRE;
        s.trail_len = 4;
        fx_spawn(&s);
    }
}

// ---- drawing ------------------------------------------------------------------------

// Ring points in world space with radius scale, twist and jag.
static void ring_points(const Ring* r, int idx, float scale, float twist, float jag, V3* out) {
    for (int k = 0; k <= NP; k++) {
        float a = TAU * (k % NP) / NP;
        float rad = r->r * scale * jag_radius(H->jag_seed, idx, a, jag);
        float aa = a + twist;
        out[k] = vadd(r->c, vadd(vmul(r->n, cosf(aa) * rad), vmul(r->b, sinf(aa) * rad)));
    }
}

typedef struct { float scale, twist, i, jag; int ramp; bool vis; } RingLook;

static RingLook look_of(const Ring* r, int idx, float t, float blend) {
    RingLook L = { 1, 0, 0, 0, R_CYAN, false };
    if (r->hidden || r->gone) return L;
    float d = vdot(vsub(r->c, cam.pos), cam.fwd);
    if (d < 0.08f) return L;
    L.vis = true;
    L.scale = smooth((t - r->born) / 0.5f);
    L.i = depth_intensity(d);
    L.jag = r->jag * (1.0f - ramp01(t, 0.3f, 3.0f));
    L.ramp = blend < 1.0f ? R_FIRE : R_CYAN;
    if (r->boom >= 0 && t >= r->boom) {
        float u = t - r->boom;
        if (u < 0.15f) { L.ramp = R_WHITE; L.i = 1.0f; }
        else L.scale *= smooth((u - 0.15f) / 0.7f);
    }
    if (r->collapse >= 0 && t >= r->collapse) {
        float u = sat((t - r->collapse) / 0.55f);
        L.scale *= powf(1.0f - u, 1.5f);
        L.twist = u * 2.6f;
        L.ramp = u < 0.3f ? R_CYAN : (u < 0.65f ? R_MAGENTA : R_RED);
        L.i = fminf(1.0f, L.i + 0.3f * u);
    }
    (void)idx;
    return L;
}

static void draw_ring(Gfx* g, const Ring* r, int idx, RingLook L, float blend, bool halo) {
    V3 pts[NP + 1];
    ring_points(r, idx, L.scale, L.twist, L.jag, pts);
    if (blend > 0.0f && blend < 1.0f) {
        if (halo) g3_poly(g, &cam, R_FIRE, L.i * 0.4f * (1 - blend), 3, pts, NP + 1);
        g3_poly(g, &cam, R_FIRE, L.i * (1 - blend), 1, pts, NP + 1);
        g3_poly(g, &cam, R_CYAN, L.i * blend, 1, pts, NP + 1);
        return;
    }
    if (halo) g3_poly(g, &cam, L.ramp, L.i * 0.4f, 3, pts, NP + 1);
    g3_poly(g, &cam, L.ramp, L.i, 1, pts, NP + 1);
}

static void draw_longs(Gfx* g, const Ring* a, int ia, RingLook La, const Ring* b, int ib, RingLook Lb, float k_i) {
    if (!La.vis || !Lb.vis || k_i <= 0) return;
    V3 pa[NP + 1], pb[NP + 1];
    ring_points(a, ia, La.scale, La.twist, La.jag, pa);
    ring_points(b, ib, Lb.scale, Lb.twist, Lb.jag, pb);
    float i = fminf(La.i, Lb.i) * 0.45f * k_i;
    int ramp = La.ramp == R_FIRE ? R_FIRE : (La.ramp == R_CYAN ? R_GREEN : La.ramp);
    for (int k = 0; k < NP; k += 4) g3_line(g, &cam, ramp, i, 1, pa[k], pb[k]);
}

static void draw_node(Gfx* g, const Ring* r, RingLook L, float t) {
    if (!r->node || r->boomed || !L.vis) return;
    float pulse = 0.7f + 0.3f * sinf(t * 9.0f);
    for (int q = 0; q < 4; q++) {
        float a = PI * 0.25f + q * PI * 0.5f;
        V3 rad = vadd(vmul(r->n, cosf(a)), vmul(r->b, sinf(a)));
        V3 tan = vadd(vmul(r->n, -sinf(a)), vmul(r->b, cosf(a)));
        V3 c = vadd(r->c, vmul(rad, r->r * L.scale));
        float s = 0.08f;
        V3 d[5] = { vmad(c, rad, s), vmad(c, tan, s), vmad(c, rad, -s), vmad(c, tan, -s), vmad(c, rad, s) };
        g3_poly(g, &cam, R_YELLOW, L.i * pulse * 0.45f, 3, d, 5);
        g3_poly(g, &cam, R_YELLOW, fminf(1.0f, L.i * 1.2f) * pulse, 1, d, 5);
    }
}

static void draw_ship(Gfx* g, float t) {
    float a = (1.0f - ramp01(t, T_STOP, T_STOP + 1.5f)) * ramp01(t, 0.4f, 1.2f);
    if (a <= 0) return;
    V3 c, f, u;
    path_at(s_ship, &c, &f, &u);
    V3 rt = vcross(f, u);
    c = vmad(c, u, -0.28f);
    float bank = 0.3f * sinf(t * 0.7f);
    u = vrot(u, f, bank);
    rt = vrot(rt, f, bank);
#define SP(x, y, z) vadd(vadd(vadd(c, vmul(f, x)), vmul(u, y)), vmul(rt, z))
    V3 nose = SP(0.34f, 0, 0), wl = SP(-0.12f, -0.02f, 0.24f), wr = SP(-0.12f, -0.02f, -0.24f);
    V3 tail = SP(-0.16f, 0.0f, 0), fin = SP(-0.14f, 0.1f, 0), keel = SP(-0.05f, -0.05f, 0);
#undef SP
    V3 outline[6] = { nose, wl, tail, wr, nose, fin };
    g3_poly(g, &cam, R_YELLOW, 0.35f * a, 3, outline, 5);
    g3_poly(g, &cam, R_YELLOW, 0.95f * a, 1, outline, 6);
    g3_line(g, &cam, R_YELLOW, 0.95f * a, 1, fin, tail);
    g3_line(g, &cam, R_YELLOW, 0.6f * a, 1, wl, keel);
    g3_line(g, &cam, R_YELLOW, 0.6f * a, 1, wr, keel);
    g3_line(g, &cam, R_YELLOW, 0.6f * a, 1, nose, keel);
    float x, y, z;
    if (cam_proj(g, &cam, tail, &x, &y, &z)) gx_dot(g, R_FIRE, 0.95f * a, x, y, 2.5f * g->lw);
}

static void draw_shots(Gfx* g) {
    for (int k = 0; k < MAXSHOT; k++) {
        if (!shots[k].alive) continue;
        V3 c0, c1, t, n;
        path_at(shots[k].s - 0.8f, &c0, &t, &n);
        c0 = vmad(c0, n, -0.28f);
        path_at(shots[k].s, &c1, &t, &n);
        c1 = vmad(c1, n, -0.28f);
        g3_line(g, &cam, R_YELLOW, 0.4f, 3, c0, c1);
        g3_line(g, &cam, R_WHITE, 1.0f, 1, c0, c1);
    }
}

// Matches the surviving ring's screen points to Scene 3's equator and blends between them.
static void draw_morph(Gfx* g, float t) {
    enum { N = 48 };
    POINT eq[N], rp[N + 1], out[N + 1];
    if (skirmish_equator_points(g, eq, N) != N) return;
    Ring* r = &RING(last_ring);
    RingLook L = look_of(r, last_ring, t, 1.0f);
    for (int k = 0; k < N; k++) {
        float a = TAU * k / N;
        V3 p = vadd(r->c, vadd(vmul(r->n, cosf(a) * r->r), vmul(r->b, sinf(a) * r->r)));
        float x, y;
        if (!cam_proj(g, &cam, p, &x, &y, NULL)) return;
        rp[k].x = (LONG)x;
        rp[k].y = (LONG)y;
    }
    // best rotation/direction pairing of the two loops
    int best_off = 0, best_dir = 1;
    double best = 1e300;
    for (int dir = -1; dir <= 1; dir += 2)
        for (int off = 0; off < N; off++) {
            double s = 0;
            for (int k = 0; k < N; k += 4) {
                int j = ((off + dir * k) % N + N) % N;
                double dx = rp[k].x - eq[j].x, dy = rp[k].y - eq[j].y;
                s += dx * dx + dy * dy;
            }
            if (s < best) { best = s; best_off = off; best_dir = dir; }
        }
    float u = ramp01(t, T_MORPH0, T_MORPH1);
    for (int k = 0; k <= N; k++) {
        int kk = k % N;
        int j = ((best_off + best_dir * kk) % N + N) % N;
        out[k].x = (LONG)lerpf((float)rp[kk].x, (float)eq[j].x, u);
        out[k].y = (LONG)lerpf((float)rp[kk].y, (float)eq[j].y, u);
    }
    float i0 = L.vis ? L.i : 0.6f;
    float i = lerpf(i0, 0.5f, u);
    gx_poly(g, R_CYAN, i * (1 - u), 1, out, N + 1);
    gx_poly(g, R_GREEN, i * u, 1, out, N + 1);
}

static void draw(Gfx* g, float t) {
    V3 c, tt, n;
    path_at(s_cam, &c, &tt, &n);
    V3 tc, t2, n2;
    path_at(s_cam + 6.0f, &tc, &t2, &n2);
    if (t <= 0.0f) start_camera(&cam);
    else cam_look(&cam, vmad(c, n, CAM_UP), tc, n, FOV);

    float blend = ramp01(t, 0.5f, 3.0f);
    float longs = ramp01(t, 0.3f, 1.5f);
    bool morph = last_ring >= 0 && t >= T_MORPH0;

    int i0 = (int)s_cam - 1;
    if (i0 < 0) i0 = 0;
    // far to near
    for (int i = head - 1; i >= i0; i--) {
        if (morph && i == last_ring) continue;
        Ring* r = &RING(i);
        RingLook L = look_of(r, i, t, blend);
        if (!L.vis) continue;
        if (i + 1 < head) draw_longs(g, r, i, L, &RING(i + 1), i + 1, look_of(&RING(i + 1), i + 1, t, blend), longs);
        draw_ring(g, r, i, L, blend, L.i > 0.5f || r->jag > 0);
        draw_node(g, r, L, t);
    }
    for (int b = 0; b < MAXBR; b++) {
        Branch* B = &br[b];
        if (!B->active) continue;
        const Ring* prev = &RING(B->junction);
        int pidx = B->junction;
        RingLook Lp = look_of(prev, pidx, t, blend);
        for (int k = 0; k < BRN; k++) {
            Ring* q = &B->r[k];
            RingLook L = look_of(q, 1000 + k, t, blend);
            if (!L.vis) { prev = q; Lp = L; continue; }
            draw_longs(g, prev, pidx, Lp, q, 1000 + k, L, longs);
            draw_ring(g, q, 1000 + k, L, blend, false);
            prev = q;
            pidx = 1000 + k;
            Lp = L;
        }
    }

    fx_draw_rings(g, &cam);
    fx_draw(g, &cam);
    draw_shots(g);
    draw_ship(g, t);
    if (morph) draw_morph(g, t);
}

const SceneDef scene_wormhole = { "Wormhole Transit", DUR, init, update, draw };
