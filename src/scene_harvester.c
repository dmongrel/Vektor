// scene_harvester.c - Scene 5: Debris Harvester.
//
// A large wireframe harvester sweeps the debris field Scene 4 left behind. Its tractor beam
// (converging lines from the intake's mandibles) pulls small chunks in, where they flash
// and vanish; large fragments crash into the hull, burst, and break into pieces it then
// collects. It grows as it feeds and leaves a trail of compressed waste pellets. Outro: the
// harvester fades, and the pellets drift together into one bright point at the centre,
// which Scene 1 swells into a new planet.
//
// The scene is 2D: positions are view units, drawn through an orthographic camera.

#include "vektor.h"

#define DUR       60.0f
#define T_ENTER   0.0f
#define T_SWEEP   44.0f    // from here the beam reaches everything left
#define T_OUTRO   51.0f
#define T_MERGE   58.8f
#define LEN       0.5f     // harvester length at scale 1
#define MAXD      200
#define MAXPEL    220
#define LARGE     0.045f

enum { D_FREE, D_CAUGHT, D_GONE };

typedef struct {
    float x, y, vx, vy, size, rot, spin;
    uint32_t seed;
    int state;
    bool large, sent;
} Deb;

typedef struct { float x, y, vx, vy; bool alive; } Pellet;

static Rng R;
static Deb deb[MAXD];
static int ndeb;
static Pellet pel[MAXPEL];
static int npel_total, merged;
static float hx, hy, hang, hspeed, hscale, hit_t, retarget_t, next_send;
static int target;
static int pellets_due;
static float pellet_clock;
static Cam cam;


// ---- harvester geometry -------------------------------------------------------------------

static void to_view(float lx, float ly, float* x, float* y) {
    float s = LEN * hscale, c = cosf(hang), n = sinf(hang);
    *x = hx + (lx * c - ly * n) * s;
    *y = hy + (lx * n + ly * c) * s;
}

static void intake(float* x, float* y) { to_view(0.32f, 0.0f, x, y); }

static void poly_local(Gfx* g, int ramp, float i, int w, const float* xy, int n, bool closed) {
    POINT p[32];
    for (int k = 0; k < n; k++) {
        float x, y;
        to_view(xy[2 * k], xy[2 * k + 1], &x, &y);
        p[k].x = (LONG)gx_px(g, x);
        p[k].y = (LONG)gx_py(g, y);
    }
    if (closed) p[n++] = p[0];
    gx_poly(g, ramp, i, w, p, n);
}

static void draw_harvester(Gfx* g, float t, float a) {
    if (a <= 0) return;
    bool hit = t - hit_t < 0.18f;
    int hull_ramp = hit ? R_WHITE : R_CYAN;
    float hi = (hit ? 1.0f : 0.8f) * a;
    static const float hull[] = {
        0.30f, 0.10f,  0.18f, 0.17f,  -0.05f, 0.18f,  -0.12f, 0.25f,  -0.42f, 0.25f,  -0.50f, 0.15f,
        -0.50f, -0.15f, -0.42f, -0.25f, -0.12f, -0.25f, -0.05f, -0.18f, 0.18f, -0.17f, 0.30f, -0.10f,
    };
    poly_local(g, hull_ramp, hi * 0.35f, 3, hull, 12, false);
    poly_local(g, hull_ramp, hi, 1, hull, 12, false);
    // inner hull: spine, ribs, deck plates
    static const float spine[] = { 0.24f, 0.0f, -0.47f, 0.0f };
    poly_local(g, hull_ramp, hi * 0.6f, 1, spine, 2, false);
    static const float ribs[][4] = {
        { 0.12f, 0.15f, 0.12f, -0.15f }, { -0.05f, 0.17f, -0.05f, -0.17f }, { -0.2f, 0.24f, -0.2f, -0.24f },
        { -0.34f, 0.24f, -0.34f, -0.24f },
    };
    for (int k = 0; k < 4; k++) poly_local(g, hull_ramp, hi * 0.45f, 1, ribs[k], 2, false);
    static const float deck[] = { -0.12f, 0.16f, -0.42f, 0.16f, -0.42f, -0.16f, -0.12f, -0.16f };
    poly_local(g, hull_ramp, hi * 0.4f, 1, deck, 4, true);
    // mandibles around the intake
    static const float mand_l[] = { 0.30f, 0.10f, 0.40f, 0.17f, 0.48f, 0.21f };
    static const float mand_r[] = { 0.30f, -0.10f, 0.40f, -0.17f, 0.48f, -0.21f };
    poly_local(g, R_YELLOW, 0.9f * a, 1, mand_l, 3, false);
    poly_local(g, R_YELLOW, 0.9f * a, 1, mand_r, 3, false);
    // intake throat, spinning
    float th[34];
    for (int k = 0; k < 16; k++) {
        float q = TAU * k / 16.0f;
        th[2 * k] = 0.27f + 0.025f * cosf(q);
        th[2 * k + 1] = 0.085f * sinf(q);
    }
    poly_local(g, R_MAGENTA, 0.75f * a, 1, th, 16, true);
    float sp = t * 5.0f;
    float bar[] = { 0.27f + 0.025f * cosf(sp), 0.085f * sinf(sp), 0.27f - 0.025f * cosf(sp), -0.085f * sinf(sp) };
    poly_local(g, R_MAGENTA, 0.9f * a, 1, bar, 2, false);
    // engines
    static const float eng_l[] = { -0.50f, 0.13f, -0.58f, 0.15f, -0.58f, 0.05f, -0.50f, 0.07f };
    static const float eng_r[] = { -0.50f, -0.13f, -0.58f, -0.15f, -0.58f, -0.05f, -0.50f, -0.07f };
    poly_local(g, hull_ramp, hi, 1, eng_l, 4, true);
    poly_local(g, hull_ramp, hi, 1, eng_r, 4, true);
    // running lights
    static const float lights[][2] = { { -0.42f, 0.25f }, { -0.42f, -0.25f }, { 0.18f, 0.17f }, { 0.18f, -0.17f } };
    for (int k = 0; k < 4; k++) {
        float x, y;
        to_view(lights[k][0], lights[k][1], &x, &y);
        bool on = fmodf(t + k * 0.25f, 1.0f) < 0.18f;
        gx_dot(g, k < 2 ? R_RED : R_WHITE, (on ? 1.0f : 0.25f) * a, gx_px(g, x), gx_py(g, y), 2.0f * g->lw);
    }
}

// ---- simulation ----------------------------------------------------------------------------

static void add_deb(float x, float y, float vx, float vy, float size, float rot, float spin, uint32_t seed) {
    if (ndeb >= MAXD) return;
    Deb* d = &deb[ndeb++];
    d->x = x;
    d->y = y;
    d->vx = vx;
    d->vy = vy;
    d->size = size;
    d->rot = rot;
    d->spin = spin;
    d->seed = seed;
    d->state = D_FREE;
    d->large = size >= LARGE;
    d->sent = false;
}

static void init(Handoff* h, uint64_t seed) {
    R.s = seed;
    ndeb = 0;
    if (h->has45)
        for (int k = 0; k < h->n_shards; k++) {
            Shard2* s = &h->shards[k];
            add_deb(s->x, s->y, s->vx, s->vy, s->size, s->rot, s->spin, s->seed);
        }
    // fill out a thin field so there is always something to harvest
    while (ndeb < 36) {
        float big = rf(&R) < 0.3f;
        add_deb(rr(&R, -1.6f, 1.6f), rr(&R, -0.85f, 0.85f), rr(&R, -0.01f, 0.01f), rr(&R, -0.01f, 0.01f),
                big ? rr(&R, 0.05f, 0.08f) : rr(&R, 0.015f, 0.04f), rr(&R, 0, TAU), rr(&R, -1, 1),
                (uint32_t)rng_next(&R));
    }
    memset(pel, 0, sizeof pel);
    npel_total = merged = 0;
    pellets_due = 0;
    pellet_clock = 0;
    hscale = 1.0f;
    hang = PI;
    hspeed = 0.17f;
    hx = view_aspect + LEN * 0.7f;   // nose just off the right edge
    hy = rr(&R, -0.25f, 0.25f);
    hit_t = -10;
    retarget_t = 0;
    next_send = 10.0f;
    target = -1;
    cam_ortho(&cam);
}

static void spawn_pellet(void) {
    for (int k = 0; k < MAXPEL; k++) {
        if (pel[k].alive) continue;
        Pellet* p = &pel[k];
        float x, y;
        to_view(-0.6f, rr(&R, -0.05f, 0.05f), &x, &y);
        p->x = x;
        p->y = y;
        p->vx = -cosf(hang) * 0.07f + rr(&R, -0.02f, 0.02f);
        p->vy = -sinf(hang) * 0.07f + rr(&R, -0.02f, 0.02f);
        p->alive = true;
        npel_total++;
        return;
    }
}

static void boom(float x, float y, float scale) {
    Rng* r = &R;
    fx_burst(r, v3(x, y, 0), v3(0, 0, 0), (int)(26 * scale), 0.05f, 0.35f * scale, 0.9f, R_FIRE, 1.0f, GRAV_NONE);
    fx_ring(v3(x, y, 0), v3(0, 0, 1), 0.03f * scale, 0.0f, 0.2f, R_WHITE, 1.0f, RING_DISC);
    fx_ring(v3(x, y, 0), v3(0, 0, 1), 0.01f, 0.12f * scale, 0.5f, R_ORANGE, 0.9f, RING_SCREEN);
}

static int remaining(void) {
    int n = 0;
    for (int k = 0; k < ndeb; k++) n += deb[k].state != D_GONE;
    return n;
}

static void update(float t, float dt) {
    fx_world.fade = 1.0f - ramp01(t, T_OUTRO + 3.0f, T_OUTRO + 5.0f);
    float fx_, fy_;
    intake(&fx_, &fy_);
    float fwdx = cosf(hang), fwdy = sinf(hang);

    // steering: toward the best target, away from the edges
    if (t >= retarget_t || target < 0 || deb[target].state != D_FREE) {
        retarget_t = t + 0.6f;
        target = -1;
        float best = 1e9f;
        for (int k = 0; k < ndeb; k++) {
            Deb* d = &deb[k];
            if (d->state != D_FREE || fabsf(d->x) > view_aspect + 0.1f) continue;
            float dx = d->x - hx, dy = d->y - hy, dist = sqrtf(dx * dx + dy * dy);
            float ahead = (dx * fwdx + dy * fwdy) / (dist + 1e-4f);
            float score = dist + 0.5f * (1.0f - ahead) + (d->large ? 0.6f : 0.0f);
            if (score < best) { best = score; target = k; }
        }
    }
    float want = hang;
    if (t > 3.0f && target >= 0) want = atan2f(deb[target].y - hy, deb[target].x - hx);
    float mx = view_aspect - 0.35f, my = 0.7f;
    if (t > 3.0f && (fabsf(hx) > mx || fabsf(hy) > my)) want = atan2f(-hy, -hx);
    float diff = remainderf(want - hang, TAU);
    hang = remainderf(hang + clampf(diff, -0.8f * dt, 0.8f * dt), TAU);
    float spd = t < T_SWEEP ? hspeed : hspeed * 1.4f;
    hx += cosf(hang) * spd * dt;
    hy += sinf(hang) * spd * dt;

    // engine exhaust
    if (t < T_OUTRO + 2.0f && rf(&R) < 0.8f) {
        for (int e = -1; e <= 1; e += 2) {
            float x, y;
            to_view(-0.6f, 0.1f * e, &x, &y);
            PSpawn s = { 0 };
            s.p = v3(x, y, 0);
            s.v = v3(-cosf(hang) * 0.25f + rr(&R, -0.03f, 0.03f), -sinf(hang) * 0.25f + rr(&R, -0.03f, 0.03f), 0);
            s.ttl = 0.35f;
            s.heat = 0.85f * (1.0f - ramp01(t, T_OUTRO, T_OUTRO + 2.0f));
            s.drag = 2.0f;
            s.size = 0.8f;
            s.ramp = R_FIRE;
            s.trail_len = 4;
            fx_spawn(&s);
        }
    }

    // big fragments are nudged into the harvester's path now and then
    if (t >= next_send && t < T_OUTRO) {
        next_send = t + rr(&R, 3.5f, 6.0f);
        int best = -1;
        float bd = 1e9f;
        for (int k = 0; k < ndeb; k++) {
            Deb* d = &deb[k];
            if (d->state != D_FREE || !d->large || d->sent) continue;
            float dx = d->x - hx, dy = d->y - hy, dist = dx * dx + dy * dy;
            if (dist < bd) { bd = dist; best = k; }
        }
        if (best >= 0) {
            Deb* d = &deb[best];
            float px = hx + cosf(hang) * 0.5f, py = hy + sinf(hang) * 0.5f;   // where the hull will be
            float dx = px - d->x, dy = py - d->y, l = sqrtf(dx * dx + dy * dy) + 1e-4f;
            d->vx = dx / l * 0.12f;
            d->vy = dy / l * 0.12f;
            d->sent = true;
        }
    }

    // the tractor beam
    int caught = 0;
    for (int k = 0; k < ndeb; k++) caught += deb[k].state == D_CAUGHT;
    int cap_max = t < T_SWEEP ? 3 : 7;
    float range = (t < T_SWEEP ? 0.55f : 3.5f) * hscale;
    for (int k = 0; k < ndeb && caught < cap_max && t > 2.0f && t < T_OUTRO + 1.0f; k++) {
        Deb* d = &deb[k];
        if (d->state != D_FREE || (d->large && t < T_SWEEP)) continue;
        float dx = d->x - fx_, dy = d->y - fy_, dist = sqrtf(dx * dx + dy * dy);
        if (dist > range) continue;
        // ahead of the intake, or close enough that it is alongside the hull
        if (t < T_SWEEP && dist > 0.3f * hscale && (dx * fwdx + dy * fwdy) / (dist + 1e-4f) < 0.45f) continue;
        d->state = D_CAUGHT;
        caught++;
    }

    for (int k = 0; k < ndeb; k++) {
        Deb* d = &deb[k];
        if (d->state == D_GONE) continue;
        d->rot += d->spin * dt;
        if (d->state == D_CAUGHT) {
            float dx = fx_ - d->x, dy = fy_ - d->y, dist = sqrtf(dx * dx + dy * dy) + 1e-4f;
            float pull = t < T_SWEEP ? 0.9f : 2.2f;
            d->vx = d->vx * (1.0f - 1.5f * dt) + dx / dist * pull * dt;
            d->vy = d->vy * (1.0f - 1.5f * dt) + dy / dist * pull * dt;
            d->spin *= 1.0f + dt;
            d->size *= 1.0f - 0.25f * dt;
            if (dist < 0.035f * hscale) {
                d->state = D_GONE;
                fx_ring(v3(fx_, fy_, 0), v3(0, 0, 1), 0.025f, 0.0f, 0.18f, R_WHITE, 1.0f, RING_DISC);
                fx_burst(&R, v3(fx_, fy_, 0), v3(0, 0, 0), 6, 0.05f, 0.2f, 0.4f, R_MAGENTA, 1.0f, GRAV_NONE);
                hscale = fminf(1.4f, hscale + 0.007f);
                pellets_due++;
            }
        } else {
            // large fragments crash into the hull and break up
            float cx, cy;
            to_view(-0.12f, 0.0f, &cx, &cy);
            float dx = d->x - cx, dy = d->y - cy, dist = sqrtf(dx * dx + dy * dy);
            if (d->large && dist < 0.27f * LEN * hscale + d->size) {
                d->state = D_GONE;
                boom(d->x, d->y, 1.3f);
                hit_t = t;
                hang += rr(&R, -0.12f, 0.12f);
                for (int q = 0; q < 3 + ri(&R, 2); q++) {
                    float a = atan2f(dy, dx) + rr(&R, -1.0f, 1.0f);
                    float sp = rr(&R, 0.06f, 0.15f);
                    add_deb(d->x, d->y, cosf(a) * sp, sinf(a) * sp, d->size * rr(&R, 0.3f, 0.5f),
                            rr(&R, 0, TAU), rr(&R, -3, 3), (uint32_t)rng_next(&R));
                }
                continue;
            }
            d->vx *= 1.0f - 0.2f * dt;
            d->vy *= 1.0f - 0.2f * dt;
        }
        d->x += d->vx * dt;
        d->y += d->vy * dt;
    }

    // compressed waste, one pellet per chunk, ejected a moment after it is swallowed
    pellet_clock += dt;
    if (pellets_due > 0 && pellet_clock > 0.25f) {
        pellet_clock = 0;
        pellets_due--;
        spawn_pellet();
    }

    // pellets drift; in the outro they gather into one point
    for (int k = 0; k < MAXPEL; k++) {
        Pellet* p = &pel[k];
        if (!p->alive) continue;
        if (t < T_OUTRO + 1.0f) {
            p->vx *= 1.0f - 0.5f * dt;
            p->vy *= 1.0f - 0.5f * dt;
        } else {
            float pull = 0.8f + 6.0f * lin01(t, T_OUTRO + 1.0f, T_MERGE);
            float dx = -p->x, dy = -p->y;
            p->vx = p->vx * (1.0f - 2.5f * dt) + dx * pull * dt;
            p->vy = p->vy * (1.0f - 2.5f * dt) + dy * pull * dt;
            if (t > T_MERGE) { p->x *= 1.0f - 6.0f * dt; p->y *= 1.0f - 6.0f * dt; }
            if (dx * dx + dy * dy < 0.0002f || t > DUR - 0.25f) { p->alive = false; merged++; continue; }
        }
        p->x += p->vx * dt;
        p->y += p->vy * dt;
    }
    (void)remaining;
}

// ---- drawing ----------------------------------------------------------------------------------

static void draw(Gfx* g, float t) {
    float a =1.0f - ramp01(t, T_OUTRO, T_OUTRO + 2.5f);

    for (int k = 0; k < ndeb; k++) {
        Deb* d = &deb[k];
        if (d->state == D_GONE) continue;
        bool c = d->state == D_CAUGHT;
        draw_shard(g, d->x, d->y, d->size, d->rot, d->seed, 0.0f, c ? 0.85f : SHARD_I);
    }

    // tractor beam: converging lines from the mandible tips and the throat
    float lx, ly, rx, ry, ix, iy;
    to_view(0.48f, 0.21f, &lx, &ly);
    to_view(0.48f, -0.21f, &rx, &ry);
    intake(&ix, &iy);
    for (int k = 0; k < ndeb; k++) {
        Deb* d = &deb[k];
        if (d->state != D_CAUGHT) continue;
        float fl = (0.7f + 0.3f * sinf(t * 23.0f + k)) * a;
        float px = gx_px(g, d->x), py = gx_py(g, d->y);
        gx_line(g, R_GREEN, 0.75f * fl, 1, gx_px(g, lx), gx_py(g, ly), px, py);
        gx_line(g, R_GREEN, 0.75f * fl, 1, gx_px(g, rx), gx_py(g, ry), px, py);
        gx_line(g, R_GREEN, 0.3f * fl, 3, gx_px(g, ix), gx_py(g, iy), px, py);
        gx_ring(g, R_GREEN, 0.6f * fl, 1, px, py, (d->size * 1.3f) * g->unit);
    }

    draw_harvester(g, t, a);

    for (int k = 0; k < MAXPEL; k++) {
        Pellet* p = &pel[k];
        if (!p->alive) continue;
        float x = gx_px(g, p->x), y = gx_py(g, p->y);
        gx_ring(g, R_YELLOW, 0.75f, 1, x, y, 0.007f * g->unit + 1.0f);
        gx_dot(g, R_ORANGE, 0.8f, x, y, 1.0f * g->lw);
    }

    fx_draw_rings(g, &cam);
    fx_draw(g, &cam);

    // the gathering point: ends exactly as Scene 1's first frame begins
    if (t > T_OUTRO + 1.0f) {
        float f = npel_total > 0 ? (float)merged / (float)npel_total : ramp01(t, T_OUTRO + 2.0f, DUR);
        f = fmaxf(f, ramp01(t, DUR - 1.0f, DUR));
        gx_dot(g, R_WHITE, 0.3f + 0.7f * f, g->cx, g->cy, (1.5f + 1.5f * f) * g->lw);
        gx_ring(g, R_WHITE, 0.5f * f, 2, g->cx, g->cy, 5.0f * g->lw * f);
    }
}

const SceneDef scene_harvester = { "Debris Harvester", DUR, init, update, draw };
