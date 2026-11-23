// scene_stations.c - Scene 4: Station Cascade.
//
// A ring of stations joined by cables orbits a wireframe planet. One reactor fails, flashes
// and explodes; the shockwave runs down the cables and debris strikes the neighbours, so the
// failure cascades around the ring faster and faster. Most debris stays in orbit as a ring
// of fire that decays into the planet; the heavier fragments are flung clear and drift.
// Intro: the stations unfold from Scene 3's salvo points. Outro: the planet dims and drifts
// out of frame, leaving the fragments as Scene 5's debris field.

#include "vektor.h"

#define DUR       60.0f
#define FOV       45.0f
#define CAM_D     7.5f
#define RING_R    2.4f
#define OMEGA     0.25f    // orbital angular speed
#define T_FAIL    12.0f    // first reactor warning
#define T_OUTRO   52.0f
#define MAXST     16
#define MAXSHARD  80
#define SC        1.6f     // station model scale

const float STATIONS_ELEV = 0.38f;

typedef enum { ST_ALIVE, ST_WARN, ST_DEAD } StState;

typedef struct {
    StState state;
    float warn0, warn_dur, dead_t;
    int step;              // cascade generation
} Station;

typedef struct {
    float t0;              // pulse start
    int from, to;
    bool alive;
} Pulse;

typedef struct {
    V3 p, v;
    float size, rot, spin, born;
    uint32_t seed;
    bool alive;
} Shard;

static Rng R;
static Handoff* H;
static Station st[MAXST];
static int nst;
static Pulse pulses[MAXST * 2];
static Shard sh[MAXSHARD];
static int nshard;
static Shard2 flat[MAXSHARD];  // fragments frozen to the screen for the outro
static int nflat;
static bool flattened;
static Cam cam;
static float flash_t;

// ---- exported geometry (Scene 3's outro aims at these) ----------------------------------

void stations_camera(Cam* c, float t) {
    float az = 0.35f + 0.02f * t;
    float e = STATIONS_ELEV;
    V3 pos = v3(CAM_D * cosf(e) * sinf(az), CAM_D * sinf(e), CAM_D * cosf(e) * cosf(az));
    cam_look(c, pos, v3(0, 0, 0), v3(0, 1, 0), FOV);
}

static float slot_angle(const Handoff* h, int i, float t) {
    int n = h->n_stations > 0 ? h->n_stations : 10;
    return h->ring_phase + TAU * (float)i / (float)n + OMEGA * t;
}

V3 stations_slot(const Handoff* h, int i, float t) {
    float a = slot_angle(h, i, t);
    return v3(RING_R * cosf(a), 0, RING_R * sinf(a));
}

float stations_planet_r(void) {
    float f = 1.0f / tanf(FOV * 0.5f * PI / 180.0f);
    return f / sqrtf(CAM_D * CAM_D - 1.0f);
}

// ---- station model ----------------------------------------------------------------------

static void frame_of(int i, float t, V3* c, V3* r, V3* tg, V3* up) {
    float a = slot_angle(H, i, t);
    *c = v3(RING_R * cosf(a), 0, RING_R * sinf(a));
    *r = v3(cosf(a), 0, sinf(a));
    *tg = v3(-sinf(a), 0, cosf(a));
    *up = v3(0, 1, 0);
}

static bool hidden(V3 p) { return sphere_occludes(cam.pos, v3(0, 0, 0), 1.0f, p); }

static void line3(Gfx* g, int ramp, float i, int w, V3 a, V3 b) {
    if (hidden(vmul(vadd(a, b), 0.5f))) return;
    g3_line(g, &cam, ramp, i, w, a, b);
}

static void draw_station(Gfx* g, int i, float t) {
    Station* s = &st[i];
    float grow = smooth((t - 0.2f - i * 0.08f) / 1.6f);
    float unfold = smooth((t - 1.5f - i * 0.06f) / 1.8f);
    V3 c, r, tg, up;
    frame_of(i, t, &c, &r, &tg, &up);
    if (hidden(c)) return;

    if (t < 2.5f) {   // the salvo point the station unfolds from
        float x, y;
        if (cam_proj(g, &cam, c, &x, &y, NULL)) gx_dot(g, R_YELLOW, 1.0f - smooth(t / 2.5f) * 0.6f, x, y, 2.5f * g->lw);
    }
    if (grow <= 0) return;
    float k = SC * grow;
    bool warn = s->state == ST_WARN;
    float wu = warn ? sat((t - s->warn0) / s->warn_dur) : 0;
    bool blink = warn && sinf(TAU * (2.0f + 12.0f * wu * wu) * (t - s->warn0)) > 0;
    int hull = blink ? R_WHITE : R_CYAN;
    float hi = blink ? 1.0f : 0.75f;

    // hub: hexagonal prism along the orbit tangent, back faces hidden
    V3 hx[2][6];
    for (int e = 0; e < 2; e++)
        for (int q = 0; q < 6; q++) {
            float a = TAU * q / 6.0f;
            hx[e][q] = vadd(c, vmul(vadd(vadd(vmul(r, cosf(a) * 0.07f), vmul(up, sinf(a) * 0.07f)),
                                        vmul(tg, e ? 0.09f : -0.09f)), k));
        }
    for (int q = 0; q < 6; q++) {
        int q2 = (q + 1) % 6;
        V3 fc = vmul(vadd(vadd(hx[0][q], hx[0][q2]), vadd(hx[1][q], hx[1][q2])), 0.25f);
        V3 n = vsub(fc, c);
        bool front = vdot(n, vsub(cam.pos, fc)) > 0;
        if (front) {
            line3(g, hull, hi, 1, hx[0][q], hx[0][q2]);
            line3(g, hull, hi, 1, hx[1][q], hx[1][q2]);
            line3(g, hull, hi, 1, hx[0][q], hx[1][q]);
            line3(g, hull, hi, 1, hx[0][q2], hx[1][q2]);
        }
    }
    // solar panels unfold along +-up
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        float len = 0.04f + 0.24f * unfold;
        V3 base = vadd(c, vmul(up, sgn * 0.075f * k));
        V3 p0 = vadd(base, vmul(tg, -0.06f * k)), p1 = vadd(base, vmul(tg, 0.06f * k));
        V3 ext = vmul(up, sgn * len * k);
        V3 p2 = vadd(p1, ext), p3 = vadd(p0, ext);
        V3 loop[5] = { p0, p1, p2, p3, p0 };
        if (!hidden(base)) g3_poly(g, &cam, R_YELLOW, 0.55f * grow, 1, loop, 5);
        for (int m = 1; m < 3; m++) {
            float f = (float)m / 3.0f;
            line3(g, R_YELLOW, 0.32f * grow, 1, vlerp(p0, p3, f), vlerp(p1, p2, f));
        }
    }
    // antenna
    line3(g, R_CYAN, 0.6f * grow, 1, vadd(c, vmul(r, 0.07f * k)), vadd(c, vmul(r, 0.17f * k)));
    // reactor ring: a steady glow, then the warning blink, brighter and faster
    V3 rr_[17];
    float rad = 0.045f * k * (1.0f + 0.6f * wu);
    for (int q = 0; q <= 16; q++) {
        float a = TAU * q / 16.0f;
        rr_[q] = vadd(c, vadd(vmul(r, cosf(a) * rad), vmul(up, sinf(a) * rad)));
    }
    float ri_ = warn ? (blink ? 1.0f : 0.5f) : 0.55f + 0.1f * sinf(t * 4.0f + i);
    g3_poly(g, &cam, warn ? R_WHITE : R_ORANGE, ri_ * 0.4f, 3, rr_, 17);
    g3_poly(g, &cam, warn ? R_WHITE : R_ORANGE, ri_, 1, rr_, 17);
}

// Cable i joins station i to station i+1, sagging toward the planet.
static void cable_points(int i, float t, V3* out, int n) {
    int j = (i + 1) % nst;
    V3 ci, ri_, ti, ui, cj, rj, tj, uj;
    frame_of(i, t, &ci, &ri_, &ti, &ui);
    frame_of(j, t, &cj, &rj, &tj, &uj);
    V3 a = vadd(ci, vmul(ti, 0.09f * SC)), b = vadd(cj, vmul(tj, -0.09f * SC));
    V3 rm = vnorm(vadd(ri_, rj));
    for (int k = 0; k < n; k++) {
        float s = (float)k / (float)(n - 1);
        out[k] = vsub(vlerp(a, b, s), vmul(rm, 0.18f * sinf(PI * s)));
    }
}

static void draw_cable(Gfx* g, int i, float t) {
    int j = (i + 1) % nst;
    float draw_frac = smooth((t - 2.6f - i * 0.1f) / 1.6f);
    if (draw_frac <= 0) return;
    float broken = -1;
    if (st[i].state == ST_DEAD) broken = st[i].dead_t;
    if (st[j].state == ST_DEAD && (broken < 0 || st[j].dead_t < broken)) broken = st[j].dead_t;
    float fade = broken < 0 ? 1.0f : 1.0f - sat((t - broken) / 0.7f);
    if (fade <= 0) return;
    enum { N = 14 };
    V3 p[N];
    cable_points(i, t, p, N);
    int last = (int)(draw_frac * (N - 1));
    float flick = broken >= 0 ? (sinf(t * 60.0f) > 0 ? 1.0f : 0.3f) : 1.0f;
    for (int k = 0; k < last; k++) line3(g, R_MAGENTA, 0.5f * fade * flick, 1, p[k], p[k + 1]);
    if (broken >= 0 || draw_frac < 1) return;
    // data pulses running along the cable
    for (int q = 0; q < 2; q++) {
        float u = fmodf(t * 0.45f + q * 0.5f + i * 0.13f, 1.0f);
        V3 pos = vlerp(p[(int)(u * (N - 1))], p[(int)(u * (N - 1)) + 1 < N ? (int)(u * (N - 1)) + 1 : N - 1], fmodf(u * (N - 1), 1.0f));
        float x, y;
        if (!hidden(pos) && cam_proj(g, &cam, pos, &x, &y, NULL)) gx_dot(g, R_CYAN, 0.9f, x, y, 1.6f * g->lw);
    }
}

// The shockwave of an explosion running down a cable to the next station.
static void draw_pulse(Gfx* g, const Pulse* pu, float t) {
    float u = sat((t - pu->t0) / 1.0f);
    int cable = (pu->to == (pu->from + 1) % nst) ? pu->from : pu->to;
    bool forward = cable == pu->from;
    enum { N = 14 };
    V3 p[N];
    cable_points(cable, t, p, N);
    float pos = (forward ? u : 1.0f - u) * (N - 1);
    int k = (int)pos;
    if (k >= N - 1) k = N - 2;
    V3 head = vlerp(p[k], p[k + 1], pos - (float)k);
    float x, y;
    if (!hidden(head) && cam_proj(g, &cam, head, &x, &y, NULL)) {
        gx_dot(g, R_WHITE, 1.0f, x, y, 3.5f * g->lw);
        gx_ring(g, R_YELLOW, 0.6f, 1, x, y, 7.0f * g->lw);
    }
    // the cable behind the pulse glows hot
    for (int q = 0; q < N - 1; q++) {
        float mid = (float)q + 0.5f;
        bool behind = forward ? mid < pos : mid > pos;
        if (behind) line3(g, R_FIRE, 0.75f, 2, p[q], p[q + 1]);
    }
}

// ---- cascade ----------------------------------------------------------------------------

static void spawn_shards(V3 c, V3 vorb, int n, float t) {
    for (int k = 0; k < n && nshard < MAXSHARD; k++) {
        Shard* s = &sh[nshard++];
        V3 out = vnorm(vadd(vmul(vnorm(c), 0.4f), v3(rr(&R, -1, 1), rr(&R, -0.7f, 0.7f), rr(&R, -1, 1))));
        s->p = c;
        s->v = vmul(out, rr(&R, 0.45f, 1.0f));
        (void)vorb;
        s->size = rf(&R) < 0.4f ? rr(&R, 0.16f, 0.26f) : rr(&R, 0.06f, 0.12f);
        s->rot = rr(&R, 0, TAU);
        s->spin = rr(&R, -2.0f, 2.0f);
        s->seed = (uint32_t)rng_next(&R);
        s->born = t;
        s->alive = true;
    }
}

static void explode(int i, float t) {
    Station* s = &st[i];
    s->state = ST_DEAD;
    s->dead_t = t;
    V3 c, r, tg, up;
    frame_of(i, t, &c, &r, &tg, &up);
    V3 vorb = vmul(tg, OMEGA * RING_R);
    // the burning debris that forms the ring of fire
    for (int k = 0; k < 150; k++) {
        PSpawn p = { 0 };
        p.p = c;
        p.v = vadd(vorb, vmul(rand_unit(&R), rr(&R, 0.02f, 0.16f)));
        p.v.y *= 0.25f;
        p.ttl = rr(&R, 30.0f, 45.0f);
        p.heat = rr(&R, 0.75f, 1.0f);
        p.drag = rr(&R, 0.008f, 0.03f);
        p.size = rr(&R, 0.6f, 1.2f);
        p.ramp = R_FIRE;
        p.grav = GRAV_CENTER;
        p.trail_len = 10;
        fx_spawn(&p);
    }
    fx_burst(&R, c, vorb, 30, 0.3f, 1.0f, 0.8f, R_WHITE, 1.0f, GRAV_NONE);
    fx_ring(c, up, 0.25f, 0.0f, 0.3f, R_WHITE, 1.0f, RING_DISC);
    fx_ring(c, up, 0.05f, 1.1f, 1.0f, R_YELLOW, 0.9f, RING_PLANE);
    fx_ring(c, up, 0.1f, 0.9f, 0.7f, R_ORANGE, 0.9f, RING_SCREEN);
    spawn_shards(c, vorb, 3 + ri(&R, 3), t);
    if (flash_t < 0) flash_t = t;   // the first one lights up the screen

    // shockwave down both cables; debris thrown at the neighbours
    for (int d = -1; d <= 1; d += 2) {
        int j = (i + d + nst) % nst;
        if (st[j].state != ST_ALIVE) continue;
        for (int q = 0; q < MAXST * 2; q++) {
            if (pulses[q].alive) continue;
            pulses[q] = (Pulse){ t, i, j, true };
            break;
        }
        st[j].state = ST_WARN;
        st[j].step = s->step + 1;
        st[j].warn0 = t + 1.0f;
        st[j].warn_dur = fmaxf(0.35f, 2.3f * powf(0.84f, (float)st[j].step));
        V3 cj, rj, tj, uj;
        frame_of(j, t + 1.0f, &cj, &rj, &tj, &uj);
        for (int k = 0; k < 5; k++) {
            PSpawn p = { 0 };
            p.p = c;
            p.v = vadd(vmul(vsub(cj, c), rr(&R, 0.9f, 1.05f)), vmul(rand_unit(&R), 0.08f));
            p.ttl = 1.0f;
            p.heat = 1.0f;
            p.drag = 0;
            p.size = 1.4f;
            p.ramp = R_FIRE;
            p.grav = GRAV_NONE;
            p.trail_len = 10;
            fx_spawn(&p);
        }
    }
}

// ---- scene interface --------------------------------------------------------------------

static void init(Handoff* h, uint64_t seed) {
    H = h;
    R.s = seed;
    if (!h->has34) {
        h->n_stations = 10;
        h->ring_phase = rr(&R, 0, TAU);
    }
    nst = h->n_stations;
    if (nst > MAXST) nst = MAXST;
    memset(st, 0, sizeof st);
    memset(pulses, 0, sizeof pulses);
    memset(sh, 0, sizeof sh);
    nshard = 0;
    nflat = 0;
    flattened = false;
    flash_t = -1;
    int first = ri(&R, nst);
    st[first].state = ST_WARN;
    st[first].warn0 = T_FAIL;
    st[first].warn_dur = 2.6f;
    st[first].step = 0;
    h->has45 = true;
    h->n_shards = 0;
}

// Projects the 3D fragments to view units once; afterwards they drift on the screen.
static void freeze_shards(void) {
    Gfx v = { 0 };     // a view-unit "surface": x right, y down, one unit per view unit
    v.unit = 1.0f;
    nflat = 0;
    for (int k = 0; k < nshard && nflat < MAXSHARD; k++) {
        Shard* s = &sh[k];
        float x, y, z;
        if (hidden(s->p) || !cam_proj(&v, &cam, s->p, &x, &y, &z)) continue;
        if (fabsf(x) > view_aspect + 0.15f || fabsf(y) > 1.1f) continue;
        Shard2* f = &flat[nflat++];
        f->x = x;
        f->y = -y;
        f->vx = rr(&R, -0.012f, 0.012f);
        f->vy = rr(&R, -0.012f, 0.012f);
        f->size = s->size * cam.focal / z;
        f->rot = s->rot;
        f->spin = s->spin * 0.5f;
        f->seed = s->seed;
    }
}

static void update(float t, float dt) {
    stations_camera(&cam, t);
    fx_world.center = v3(0, 0, 0);
    fx_world.gm = OMEGA * OMEGA * RING_R * RING_R * RING_R;
    fx_world.kill_r = 1.0f;
    fx_world.atmo_r = 1.45f;
    fx_world.atmo_k = 1.4f;
    fx_world.fade = 1.0f - ramp01(t, T_OUTRO + 3.0f, DUR - 0.6f);

    // impacts on the planet's surface
    V3 w;
    int n_imp = 0;
    while (fx_pop_impact(&w)) {
        if (n_imp++ > 6 || rf(&R) > 0.5f) continue;
        V3 nrm = vnorm(w);
        fx_ring(nrm, nrm, 0.01f, rr(&R, 0.06f, 0.14f), 0.6f, R_ORANGE, 0.85f, RING_PLANE);
    }

    for (int i = 0; i < nst; i++)
        if (st[i].state == ST_WARN && t >= st[i].warn0 + st[i].warn_dur) explode(i, t);
    for (int q = 0; q < MAXST * 2; q++) {
        Pulse* pu = &pulses[q];
        if (!pu->alive || t < pu->t0 + 1.0f) continue;
        pu->alive = false;
        V3 c, r, tg, up;
        frame_of(pu->to, t, &c, &r, &tg, &up);
        fx_burst(&R, c, vmul(tg, OMEGA * RING_R), 10, 0.1f, 0.4f, 0.5f, R_WHITE, 1.0f, GRAV_NONE);
    }

    for (int k = 0; k < nshard; k++) {
        Shard* s = &sh[k];
        s->v = vmul(s->v, 1.0f / (1.0f + 0.32f * dt));
        s->p = vmad(s->p, s->v, dt);
        s->rot += s->spin * dt;
    }

    // From the outro on, the fragments live on the screen, not in orbit.
    if (t >= T_OUTRO && !flattened) { flattened = true; freeze_shards(); }
    if (flattened) {
        for (int k = 0; k < nflat; k++) {
            flat[k].x += flat[k].vx * dt;
            flat[k].y += flat[k].vy * dt;
            flat[k].rot += flat[k].spin * dt;
        }
        H->n_shards = nflat < MAX_HAND_SHARDS ? nflat : MAX_HAND_SHARDS;
        memcpy(H->shards, flat, sizeof(Shard2) * (size_t)H->n_shards);
    }
}

static void draw(Gfx* g, float t) {
    stations_camera(&cam, t);
    float u = ramp01(t, T_OUTRO, DUR - 0.5f);

    // The planet, fire and stations slide out of frame during the outro.
    Gfx gs = *g;
    gs.cx -= u * u * (g->w * 0.5f + g->unit * 0.6f);

    SphereGrid s;
    sphere_grid_defaults(&s);
    s.rot = m3_axis(v3(0, 1, 0), 0.06f * t);
    s.nlat = 7;
    s.nlon = 12;
    s.ramp = R_GREEN;
    s.i = lerpf(0.5f, 0.18f, u);
    draw_sphere_grid(&gs, &cam, &s);
    float px, py, pz;
    if (cam_proj(&gs, &cam, v3(0, 0, 0), &px, &py, &pz))
        gx_ring(&gs, R_GREEN, lerpf(0.3f, 0.4f, u), 1, px, py, cam_scale(&gs, &cam, pz) * 0.995f);

    for (int i = 0; i < nst; i++) {
        if (st[i].state != ST_DEAD) draw_station(&gs, i, t);
        draw_cable(&gs, i, t);
    }
    for (int q = 0; q < MAXST * 2; q++) if (pulses[q].alive && t >= pulses[q].t0) draw_pulse(&gs, &pulses[q], t);

    fx_draw_rings(&gs, &cam);
    fx_draw(&gs, &cam);

    if (!flattened) {
        for (int k = 0; k < nshard; k++) {
            Shard* sd = &sh[k];
            float x, y, z;
            if (hidden(sd->p) || !cam_proj(g, &cam, sd->p, &x, &y, &z)) continue;
            float heat = 0.9f * (1.0f - sat((t - sd->born) / 5.0f));
            draw_shard(g, (x - g->cx) / g->unit, (g->cy - y) / g->unit, sd->size * cam.focal / z, sd->rot, sd->seed, heat, SHARD_I);
        }
    } else {
        for (int k = 0; k < nflat; k++) draw_shard(g, flat[k].x, flat[k].y, flat[k].size, flat[k].rot, flat[k].seed, 0.0f, SHARD_I);
    }

    if (flash_t >= 0 && t - flash_t < 0.5f) gx_flash(g, R_WHITE, 1.0f, 0.3f * (1.0f - (t - flash_t) / 0.5f));
}

const SceneDef scene_stations = { "Station Cascade", DUR, init, update, draw };
