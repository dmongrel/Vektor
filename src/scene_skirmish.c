// scene_skirmish.c - Scene 3: Orbital Skirmish.
//
// Wireframe tanks fight on the outside of a rotating lat/long sphere, watched by a camera
// whose orbit tightens as the battle goes on. Missiles arc between tanks; impacts send
// shockwave rings across the surface that stun nearby tanks; destroyed tanks break into
// debris that falls inward through the grid toward the core. The last tank fires a salvo
// into space as the camera pulls back, and the salvo's missiles curve into orbit where
// Scene 4's stations will unfold.

#include "vektor.h"

#define DUR       60.0f
#define FOV       50.0f
#define NLAT      7
#define NLON      12
#define T_LAND    2.4f
#define T_FIGHT   5.5f
#define T_DONE    44.0f    // aim to be down to one tank by here
#define T_FORCE   46.5f
#define T_SALVO   48.0f
#define T_BLEND0  50.0f
#define T_BLEND1  58.0f
#define MAXT      8
#define MAXM      24
#define MAXSHOCK  24
#define MAXDEB    480
#define TS        0.1f     // tank scale

typedef struct {
    V3 p, h;               // local position (unit) and heading (tangent)
    float turret, speed, turn;
    int hp, target;
    bool alive;
    float land, stun0, stun1, hit_t;
} Tank;

typedef struct {
    V3 a, b;               // local endpoints
    int target;
    float u, dur, arc;
    bool homing, lethal, alive;
} Missile;

typedef struct { V3 c; float t0, ttl, rmax; bool alive; } Shock;

typedef struct { V3 c, hv, v, axis; float spin, age, ttl; bool alive; } Debris;

typedef struct { float ang; } Salvo;

static Rng R;
static Handoff* H;
static Tank tanks[MAXT];
static int ntank;
static Missile ms[MAXM];
static Shock shocks[MAXSHOCK];
static Debris deb[MAXDEB];
static Salvo salvo[16];
static int nsalvo;
static int survivor;
static float next_shot;
static int kills_done;
static bool lethal_inflight;
static V3 salvo_from;
static Cam cam;
static M3 plat;            // platform rotation, local -> world

// ---- camera and platform -------------------------------------------------------------

static float plat_angle(float t) { return 0.1f * t; }

static float cam_dist_end(void) {
    float f = 1.0f / tanf(FOV * 0.5f * PI / 180.0f);
    float rp = stations_planet_r();
    return sqrtf((f / rp) * (f / rp) + 1.0f);
}

static void camera_at(Cam* c, float t) {
    float az = 0.16f * t;
    float el = 0.42f + 0.1f * sinf(0.2f * t);
    float d = lerpf(4.2f, 2.75f, ramp01(t, 6.0f, T_DONE));
    float out = ramp01(t, T_SALVO, T_BLEND1);
    el = lerpf(el, STATIONS_ELEV, out);
    d = lerpf(d, cam_dist_end(), out);
    V3 pos = v3(d * cosf(el) * sinf(az), d * sinf(el), d * cosf(el) * cosf(az));
    cam_look(c, pos, v3(0, 0, 0), v3(0, 1, 0), FOV);
}

int skirmish_equator_points(const Gfx* g, POINT* out, int n) {
    Cam c;
    camera_at(&c, 0);
    for (int k = 0; k < n; k++) {
        float a = TAU * k / n;
        float x, y;
        cam_proj(g, &c, v3(cosf(a), 0, sinf(a)), &x, &y, NULL);
        out[k].x = (LONG)x;
        out[k].y = (LONG)y;
    }
    return n;
}

static V3 to_world(V3 local) { return m3_mul(&plat, local); }

// ---- sphere helpers -----------------------------------------------------------------

static float arc_between(V3 a, V3 b) { return acosf(clampf(vdot(a, b), -1.0f, 1.0f)); }

static V3 slerp_unit(V3 a, V3 b, float u) {
    float th = arc_between(a, b);
    if (th < 1e-4f) return a;
    float s = sinf(th);
    return vnorm(vadd(vmul(a, sinf((1 - u) * th) / s), vmul(b, sinf(u * th) / s)));
}

// ---- tanks ----------------------------------------------------------------------------

static int pick_target(int self) {
    int best = -1;
    float bd = 1e9f;
    for (int k = 0; k < ntank; k++) {
        if (k == self || !tanks[k].alive) continue;
        float d = arc_between(tanks[self].p, tanks[k].p) * rr(&R, 0.6f, 1.4f);
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

static int alive_count(void) {
    int n = 0;
    for (int k = 0; k < ntank; k++) n += tanks[k].alive;
    return n;
}

static void shock(V3 c, float t, float rmax) {
    for (int k = 0; k < MAXSHOCK; k++) {
        if (shocks[k].alive) continue;
        shocks[k] = (Shock){ c, t, 0.9f, rmax, true };
        break;
    }
    for (int k = 0; k < ntank; k++) {
        Tank* tk = &tanks[k];
        if (!tk->alive) continue;
        float d = arc_between(c, tk->p);
        if (d > rmax) continue;
        float arrive = t + 0.9f * (1.0f - sqrtf(1.0f - d / rmax));
        if (tk->stun1 < arrive + 1.4f) { tk->stun0 = arrive; tk->stun1 = arrive + 1.4f; }
    }
}

// Tank vertices in local platform space for the current pose.
typedef struct { V3 v[16]; } TankGeo;

static void tank_geo(const Tank* tk, float lift, TankGeo* G) {
    V3 u = tk->p, f = tk->h, r = vcross(f, u);
    V3 base = vmul(u, 1.003f + lift);
    float ca = cosf(tk->turret), sa = sinf(tk->turret);
    V3 tf = vadd(vmul(f, ca), vmul(r, sa)), tr = vcross(tf, u);
    // hull: 4 bottom, 4 top
    static const float hb[4][2] = { { -1, -0.6f }, { 1, -0.6f }, { 1, 0.6f }, { -1, 0.6f } };
    static const float ht[4][2] = { { -0.8f, -0.5f }, { 0.7f, -0.5f }, { 0.7f, 0.5f }, { -0.8f, 0.5f } };
    static const float tb[4][2] = { { -0.45f, -0.35f }, { 0.35f, -0.35f }, { 0.35f, 0.35f }, { -0.45f, 0.35f } };
    static const float tt[4][2] = { { -0.35f, -0.28f }, { 0.2f, -0.28f }, { 0.2f, 0.28f }, { -0.35f, 0.28f } };
    for (int k = 0; k < 4; k++) {
        G->v[k] = vadd(base, vmul(vadd(vmul(f, hb[k][0]), vmul(r, hb[k][1])), TS));
        G->v[4 + k] = vadd(base, vmul(vadd(vadd(vmul(f, ht[k][0]), vmul(r, ht[k][1])), vmul(u, 0.45f)), TS));
        G->v[8 + k] = vadd(base, vmul(vadd(vadd(vmul(tf, tb[k][0]), vmul(tr, tb[k][1])), vmul(u, 0.45f)), TS));
        G->v[12 + k] = vadd(base, vmul(vadd(vadd(vmul(tf, tt[k][0]), vmul(tr, tt[k][1])), vmul(u, 0.75f)), TS));
    }
}

// Draws the edges of the front-facing faces of the two frustum boxes, plus the barrel.
static void draw_tank(Gfx* g, const Tank* tk, float t, float lift, float alpha) {
    TankGeo G;
    tank_geo(tk, lift, &G);
    V3 w[16];
    for (int k = 0; k < 16; k++) w[k] = to_world(G.v[k]);
    V3 top = to_world(vmul(tk->p, 1.0f + TS * 0.8f + lift));
    if (sphere_occludes(cam.pos, v3(0, 0, 0), 0.998f, top)) return;

    bool stunned = t >= tk->stun0 && t < tk->stun1;
    bool hit = t - tk->hit_t < 0.25f;
    int ramp = hit ? R_WHITE : R_GREEN;
    float i = 0.85f * alpha;
    if (stunned) i *= 0.45f + 0.55f * (sinf(t * 40.0f) > 0);
    if (hit) i = 1.0f;

    for (int box = 0; box < 2; box++) {
        V3* b = &w[box * 8];
        V3 bc = v3(0, 0, 0);
        for (int k = 0; k < 8; k++) bc = vadd(bc, b[k]);
        bc = vmul(bc, 1.0f / 8.0f);
        static const int faces[6][4] = {
            { 0, 1, 2, 3 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 }, { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 },
        };
        for (int fa = 0; fa < 6; fa++) {
            V3 p0 = b[faces[fa][0]], p1 = b[faces[fa][1]], p2 = b[faces[fa][2]], p3 = b[faces[fa][3]];
            V3 fc = vmul(vadd(vadd(p0, p1), vadd(p2, p3)), 0.25f);
            V3 n = vcross(vsub(p1, p0), vsub(p3, p0));
            if (vdot(n, vsub(fc, bc)) < 0) n = vmul(n, -1);
            if (vdot(n, vsub(cam.pos, fc)) <= 0) continue;
            V3 loop[5] = { p0, p1, p2, p3, p0 };
            g3_poly(g, &cam, ramp, i, 1, loop, 5);
        }
    }
    // barrel
    V3 u = tk->p, f = tk->h, r = vcross(f, u);
    V3 tf = vadd(vmul(f, cosf(tk->turret)), vmul(r, sinf(tk->turret)));
    V3 base = vmul(u, 1.003f + lift);
    V3 b0 = vadd(base, vmul(vadd(vmul(tf, 0.2f), vmul(u, 0.6f)), TS));
    V3 b1 = vadd(base, vmul(vadd(vmul(tf, 1.3f), vmul(u, 0.62f)), TS));
    g3_line(g, &cam, ramp, i, 1, to_world(b0), to_world(b1));
}

static void barrel_tip(const Tank* tk, V3* out) {
    V3 u = tk->p, f = tk->h, r = vcross(f, u);
    V3 tf = vadd(vmul(f, cosf(tk->turret)), vmul(r, sinf(tk->turret)));
    *out = vadd(vmul(u, 1.003f), vmul(vadd(vmul(tf, 1.3f), vmul(u, 0.62f)), TS));
}

static void explode_tank(int k, float t) {
    Tank* tk = &tanks[k];
    tk->alive = false;
    V3 wp = to_world(vmul(tk->p, 1.02f));
    V3 wn = to_world(tk->p);
    fx_burst(&R, wp, vmul(wn, 0.3f), 50, 0.2f, 0.9f, 1.6f, R_FIRE, 1.0f, GRAV_CENTER);
    fx_burst(&R, wp, vmul(wn, 0.2f), 14, 0.3f, 1.1f, 0.7f, R_WHITE, 1.0f, GRAV_NONE);
    fx_ring(wp, wn, 0.07f, 0.0f, 0.22f, R_WHITE, 1.0f, RING_DISC);
    fx_ring(wp, wn, 0.05f, 0.5f, 0.8f, R_ORANGE, 0.9f, RING_SCREEN);
    shock(tk->p, t, 0.75f);
    // break the hull edges into debris that falls inward through the grid
    TankGeo G;
    tank_geo(tk, 0, &G);
    static const int edges[][2] = {
        { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, { 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
        { 8, 9 }, { 9, 10 }, { 10, 11 }, { 11, 8 }, { 12, 13 }, { 13, 14 }, { 14, 15 }, { 15, 12 },
    };
    for (unsigned e = 0; e < sizeof edges / sizeof edges[0]; e++) {
        for (int d = 0; d < MAXDEB; d++) {
            if (deb[d].alive) continue;
            V3 a = to_world(G.v[edges[e][0]]), b = to_world(G.v[edges[e][1]]);
            Debris* D = &deb[d];
            D->c = vmul(vadd(a, b), 0.5f);
            D->hv = vmul(vsub(b, a), 0.5f);
            D->v = vadd(vmul(wn, rr(&R, 0.2f, 0.7f)), vmul(rand_unit(&R), rr(&R, 0.1f, 0.4f)));
            D->axis = rand_unit(&R);
            D->spin = rr(&R, -8, 8);
            D->age = 0;
            D->ttl = rr(&R, 3.5f, 5.0f);
            D->alive = true;
            break;
        }
    }
}

static void impact(const Missile* m, float t) {
    V3 c = vnorm(m->b);
    V3 wp = to_world(vmul(c, 1.01f)), wn = to_world(c);
    fx_burst(&R, wp, vmul(wn, 0.25f), 26, 0.15f, 0.6f, 1.1f, R_FIRE, 1.0f, GRAV_CENTER);
    fx_ring(wp, wn, 0.06f, 0.0f, 0.22f, R_WHITE, 1.0f, RING_DISC);
    shock(c, t, 0.5f);
    // Kills happen only on scheduled lethal shots; everything else is a near miss.
    if (m->lethal && tanks[m->target].alive && alive_count() > 1) {
        explode_tank(m->target, t);
        kills_done++;
        lethal_inflight = false;
        return;
    }
    if (m->lethal) lethal_inflight = false;
    for (int k = 0; k < ntank; k++)
        if (tanks[k].alive && arc_between(c, tanks[k].p) < 0.12f) tanks[k].hit_t = t;
}

static void fire_at(int k, int target, bool lethal, float dur) {
    Tank* tk = &tanks[k];
    for (int m = 0; m < MAXM; m++) {
        if (ms[m].alive) continue;
        Missile* M = &ms[m];
        tk->target = target;
        barrel_tip(tk, &M->a);
        M->b = tanks[target].p;
        if (!lethal) {   // aim near the target, not at it
            V3 off = vrot(vperp(M->b), M->b, rr(&R, 0, TAU));
            M->b = vnorm(vmad(M->b, off, rr(&R, 0.07f, 0.2f)));
        }
        M->target = target;
        float ang = arc_between(vnorm(M->a), M->b);
        M->dur = dur > 0 ? dur : 0.8f + ang * 1.3f;
        M->arc = 0.12f + 0.45f * ang / PI;
        M->u = 0;
        M->homing = lethal;
        M->lethal = lethal;
        M->alive = true;
        fx_ring(to_world(M->a), to_world(tk->p), 0.04f, 0.0f, 0.12f, R_YELLOW, 1.0f, RING_DISC);
        return;
    }
}

static float kill_time(int i) {
    return T_FIGHT + 5.0f + (T_DONE - T_FIGHT - 5.0f) * (float)i / (float)(ntank - 2);
}

static int random_alive(int not_this) {
    int k = ri(&R, ntank);
    for (int n = 0; n < ntank; n++, k = (k + 1) % ntank)
        if (tanks[k].alive && k != not_this) return k;
    return -1;
}

// How squarely a tank faces the camera (1 = dead centre, <0 = far side).
static float facing(int k) {
    V3 wp = to_world(tanks[k].p);
    return vdot(wp, vnorm(vsub(cam.pos, wp)));
}

static V3 missile_pos(const Missile* m) {
    V3 p = slerp_unit(vnorm(m->a), vnorm(m->b), m->u);
    return vmul(p, 1.0f + 0.02f + m->arc * sinf(PI * m->u));
}

// ---- scene interface ------------------------------------------------------------------

static void init(Handoff* h, uint64_t seed) {
    H = h;
    R.s = seed;
    memset(tanks, 0, sizeof tanks);
    memset(ms, 0, sizeof ms);
    memset(shocks, 0, sizeof shocks);
    memset(deb, 0, sizeof deb);
    memset(salvo, 0, sizeof salvo);
    ntank = 7 + ri(&R, 2);
    for (int k = 0; k < ntank; k++) {
        Tank* tk = &tanks[k];
        for (int tries = 0; tries < 40; tries++) {
            tk->p = rand_unit(&R);
            tk->p.y *= 0.75f;
            tk->p = vnorm(tk->p);
            bool ok = true;
            for (int j = 0; j < k; j++) ok &= arc_between(tk->p, tanks[j].p) > 0.55f;
            if (ok) break;
        }
        tk->h = vperp(tk->p);
        tk->h = vrot(tk->h, tk->p, rr(&R, 0, TAU));
        tk->speed = rr(&R, 0.05f, 0.09f);
        tk->turn = 0;
        tk->hp = 2;
        tk->target = -1;
        tk->alive = true;
        tk->land = T_LAND + k * 0.18f;
        tk->stun0 = tk->stun1 = -1;
        tk->hit_t = -10;
    }
    survivor = -1;
    nsalvo = 0;
    next_shot = T_FIGHT;
    kills_done = 0;
    lethal_inflight = false;
    h->n_stations = 8 + ri(&R, 5);
    h->ring_phase = rr(&R, 0, TAU);
    h->has34 = true;
}

static void update(float t, float dt) {
    plat = m3_axis(v3(0, 1, 0), plat_angle(t));
    camera_at(&cam, t);
    fx_world.center = v3(0, 0, 0);
    fx_world.gm = 0.45f;
    fx_world.kill_r = 0.995f;
    fx_world.fade = 1.0f;
    V3 dummy;
    while (fx_pop_impact(&dummy)) {}

    // landings
    for (int k = 0; k < ntank; k++) {
        Tank* tk = &tanks[k];
        if (tk->alive && tk->land > 0 && t >= tk->land) {
            tk->land = 0;
            V3 wp = to_world(tk->p);
            fx_ring(vmul(wp, 1.005f), wp, 0.02f, 0.25f, 0.6f, R_GREEN, 0.9f, RING_PLANE);
            fx_burst(&R, vmul(wp, 1.01f), vmul(wp, 0.2f), 8, 0.1f, 0.3f, 0.6f, R_WHITE, 0.9f, GRAV_CENTER);
        }
    }

    // driving and aiming
    for (int k = 0; k < ntank; k++) {
        Tank* tk = &tanks[k];
        if (!tk->alive || tk->land > 0) continue;
        bool stunned = t >= tk->stun0 && t < tk->stun1;
        bool parked = t >= T_SALVO - 1.0f;
        if (!stunned && !parked) {
            tk->turn = clampf(tk->turn + rr(&R, -2.0f, 2.0f) * dt, -0.7f, 0.7f);
            tk->h = vrot(tk->h, tk->p, tk->turn * dt);
            V3 np = vnorm(vmad(tk->p, tk->h, tk->speed * dt));
            tk->h = vnorm(vsub(tk->h, vmul(np, vdot(tk->h, np))));
            tk->p = np;
        }
        if (tk->target < 0 || !tanks[tk->target].alive) tk->target = pick_target(k);
        float want = tk->turret;
        if (parked && k == survivor) {
            want = tk->turret + 2.5f * dt;   // sweeps round before the salvo
        } else if (tk->target >= 0) {
            V3 d = vsub(tanks[tk->target].p, tk->p);
            d = vsub(d, vmul(tk->p, vdot(d, tk->p)));
            V3 r = vcross(tk->h, tk->p);
            want = atan2f(vdot(d, r), vdot(d, tk->h));
        }
        float diff = remainderf(want - tk->turret, TAU);
        if (!stunned) tk->turret += clampf(diff, -2.5f * dt, 2.5f * dt);
    }

    // Lethal shots arrive on a fixed schedule, one kill at a time down to the last tank.
    // The final victim is whichever of the last two faces the camera less, so the survivor
    // is the one in view for the salvo.
    int alive = alive_count();
    if (alive > 1 && !lethal_inflight && kills_done < ntank - 1 && t >= kill_time(kills_done) - 1.6f) {
        int victim;
        if (alive == 2) {
            int a = random_alive(-1), b = random_alive(a);
            victim = facing(a) < facing(b) ? a : b;
        } else {
            victim = random_alive(-1);
        }
        int shooter = random_alive(victim);
        if (shooter >= 0) {
            lethal_inflight = true;
            fire_at(shooter, victim, true, fmaxf(0.8f, kill_time(kills_done) - t));
        }
    }
    // Near misses keep the fight busy in between.
    if (t >= next_shot && alive > 1 && t < T_SALVO - 1.5f) {
        int k = random_alive(-1);
        int tg = random_alive(k);
        if (k >= 0 && tg >= 0 && !(t >= tanks[k].stun0 && t < tanks[k].stun1)) fire_at(k, tg, false, 0);
        next_shot = t + rr(&R, 0.35f, 1.1f) * (alive > 3 ? 1.0f : 1.6f);
    }
    if (t >= T_FORCE && alive > 1) {   // safety net
        int keep = -1;
        for (int k = 0; k < ntank; k++) if (tanks[k].alive && (keep < 0 || facing(k) > facing(keep))) keep = k;
        for (int k = 0; k < ntank; k++) if (tanks[k].alive && k != keep) explode_tank(k, t);
    }
    if (survivor < 0 && alive_count() == 1)
        for (int k = 0; k < ntank; k++) if (tanks[k].alive) survivor = k;

    // missiles
    for (int m = 0; m < MAXM; m++) {
        Missile* M = &ms[m];
        if (!M->alive) continue;
        if (M->homing && tanks[M->target].alive) M->b = tanks[M->target].p;
        M->u += dt / M->dur;
        if (M->u >= 1.0f) { M->alive = false; impact(M, t); continue; }
        if (rf(&R) < 0.8f) {
            PSpawn s = { 0 };
            s.p = to_world(missile_pos(M));
            s.ttl = 0.7f;
            s.heat = 0.55f;
            s.size = 0.7f;
            s.ramp = R_FIRE;
            s.trail_len = 0;
            fx_spawn(&s);
        }
    }

    // shockwaves and debris
    for (int k = 0; k < MAXSHOCK; k++)
        if (shocks[k].alive && t > shocks[k].t0 + shocks[k].ttl) shocks[k].alive = false;
    for (int d = 0; d < MAXDEB; d++) {
        Debris* D = &deb[d];
        if (!D->alive) continue;
        D->age += dt;
        float r = vlen(D->c);
        if (D->age > D->ttl || r < 0.2f) { D->alive = false; continue; }
        D->v = vadd(vmul(D->v, 1.0f / (1.0f + 0.3f * dt)), vmul(D->c, -0.9f * dt / fmaxf(r, 0.3f)));
        D->c = vmad(D->c, D->v, dt);
        D->hv = vrot(D->hv, D->axis, D->spin * dt);
    }

    // the final salvo
    if (t >= T_SALVO && nsalvo == 0 && survivor >= 0) {
        Tank* tk = &tanks[survivor];
        V3 wp = to_world(vmul(tk->p, 1.03f)), wn = to_world(tk->p);
        // Fan the missiles up and out in the same left-to-right order as the station slots
        // they will end in, so their paths do not cross.
        salvo_from = wp;
        nsalvo = H->n_stations;
        Cam c4;
        stations_camera(&c4, T_BLEND1 - DUR);
        for (int i = 0; i < nsalvo; i++) {
            float xi = vdot(stations_slot(H, i, T_BLEND1 - DUR), c4.right);
            int rank = 0;
            for (int j = 0; j < nsalvo; j++)
                if (vdot(stations_slot(H, j, T_BLEND1 - DUR), c4.right) > xi) rank++;
            salvo[i].ang = PI * (0.12f + 0.76f * (float)rank / (float)(nsalvo - 1));
        }
        fx_ring(wp, wn, 0.15f, 0.0f, 0.3f, R_WHITE, 1.0f, RING_DISC);
        fx_burst(&R, wp, vmul(wn, 0.4f), 30, 0.2f, 0.8f, 1.0f, R_YELLOW, 1.0f, GRAV_NONE);
    }
    fx_world.fade = 1.0f - ramp01(t, 55.0f, 58.0f);
}

// ---- drawing ----------------------------------------------------------------------------

static void draw_shock(Gfx* g, const Shock* s, float t) {
    float u = sat((t - s->t0) / s->ttl);
    float r = s->rmax * (1.0f - (1.0f - u) * (1.0f - u));
    float i = 0.95f * (1.0f - u);
    V3 a = vperp(s->c), b = vcross(s->c, a);
    enum { N = 40 };
    V3 pts[N + 1];
    for (int k = 0; k <= N; k++) {
        float q = TAU * k / N;
        V3 dir = vadd(vmul(a, cosf(q)), vmul(b, sinf(q)));
        pts[k] = to_world(vmul(vnorm(vadd(vmul(s->c, cosf(r)), vmul(dir, sinf(r)))), 1.006f));
    }
    for (int k = 0; k < N; k++) {
        V3 mid = vmul(vadd(pts[k], pts[k + 1]), 0.5f);
        if (vdot(mid, vsub(cam.pos, mid)) <= 0) continue;
        g3_line(g, &cam, R_YELLOW, i * 0.4f, 3, pts[k], pts[k + 1]);
        g3_line(g, &cam, R_YELLOW, i, 1, pts[k], pts[k + 1]);
    }
}

// Screen position of salvo missile i at `time`: a cubic curve from the launch point, up
// along its fan direction, curving over into its station slot along the orbit. After
// T_BLEND1 it rides the slot itself.
static void salvo_pos(const Gfx* g, int i, float time, float* x, float* y) {
    Cam c4;
    float t4 = time - DUR;
    stations_camera(&c4, t4);
    float x3, y3, x4, y4;
    cam_proj(g, &c4, stations_slot(H, i, t4), &x3, &y3, NULL);
    float u = smooth(lin01(time, T_SALVO, T_BLEND1));
    if (u >= 1.0f) { *x = x3; *y = y3; return; }
    cam_proj(g, &c4, stations_slot(H, i, t4 + 0.1f), &x4, &y4, NULL);
    float x0, y0;
    if (!cam_proj(g, &cam, salvo_from, &x0, &y0, NULL)) { x0 = g->cx; y0 = g->cy; }
    float a = salvo[i].ang, U = g->unit;
    float c1x = x0 + cosf(a) * 0.8f * U, c1y = y0 - (sinf(a) * 0.8f + 0.3f) * U;
    float dx = x4 - x3, dy = y4 - y3, l = sqrtf(dx * dx + dy * dy) + 1e-3f;
    float c2x = x3 - dx / l * 0.5f * U, c2y = y3 - dy / l * 0.5f * U;
    float v = 1.0f - u;
    float b0 = v * v * v, b1 = 3 * v * v * u, b2 = 3 * v * u * u, b3 = u * u * u;
    *x = b0 * x0 + b1 * c1x + b2 * c2x + b3 * x3;
    *y = b0 * y0 + b1 * c1y + b2 * c2y + b3 * y3;
}

static void draw_salvo(Gfx* g, float t) {
    if (nsalvo == 0) return;
    for (int i = 0; i < nsalvo; i++) {
        POINT tr[12];
        int n = 0;
        for (int k = 0; k < 12; k++) {
            float tk = t - 0.03f * k;
            if (tk < T_SALVO) break;
            float x, y;
            salvo_pos(g, i, tk, &x, &y);
            tr[n].x = (LONG)x;
            tr[n].y = (LONG)y;
            n++;
        }
        if (n == 0) continue;
        gx_poly(g, R_FIRE, 0.4f, 3, tr, n < 5 ? n : 5);
        gx_poly(g, R_FIRE, 0.8f, 1, tr, n < 6 ? n : 6);
        if (n > 6) gx_poly(g, R_FIRE, 0.42f, 1, tr + 5, n - 5);
        gx_dot(g, R_YELLOW, 1.0f, (float)tr[0].x, (float)tr[0].y, 2.5f * g->lw);
    }
}

static void draw(Gfx* g, float t) {
    plat = m3_axis(v3(0, 1, 0), plat_angle(t));
    camera_at(&cam, t);

    SphereGrid s;
    sphere_grid_defaults(&s);
    s.rot = plat;
    s.nlat = NLAT;
    s.nlon = NLON;
    s.ramp = R_GREEN;
    s.i = 0.5f;
    s.from_equator = true;
    s.reveal = ramp01(t, 0.15f, 2.6f);
    draw_sphere_grid(g, &cam, &s);
    // Scene 2 hands over the whole equator, back half included; let the back fade out.
    if (t < 1.0f) {
        V3 eq[49];
        for (int k = 0; k <= 48; k++) eq[k] = to_world(v3(cosf(TAU * k / 48), 0, sinf(TAU * k / 48)));
        g3_poly(g, &cam, R_GREEN, 0.5f * (1.0f - smooth(t)), 1, eq, 49);
    }

    // faint core
    float cx, cy, cz;
    if (cam_proj(g, &cam, v3(0, 0, 0), &cx, &cy, &cz)) {
        float pr = 0.12f * cam_scale(g, &cam, cz);
        float i = 0.3f * (0.8f + 0.2f * sinf(t * 3)) * ramp01(t, 0.5f, 2.5f) * (1.0f - ramp01(t, T_SALVO, 54.0f));
        gx_ring(g, R_GREEN, i, 1, cx, cy, pr);
    }

    for (int k = 0; k < MAXSHOCK; k++) if (shocks[k].alive && t >= shocks[k].t0) draw_shock(g, &shocks[k], t);

    for (int k = 0; k < ntank; k++) {
        Tank* tk = &tanks[k];
        if (!tk->alive || (tk->land > 0 && t < tk->land - 0.6f)) continue;
        float lift = tk->land > 0 ? 0.5f * sat((tk->land - t) / 0.6f) : 0.0f;
        float alpha = k == survivor ? 1.0f - ramp01(t, 54.0f, 57.0f) : 1.0f;
        if (alpha > 0) draw_tank(g, tk, t, lift, alpha);
    }

    for (int d = 0; d < MAXDEB; d++) {
        Debris* D = &deb[d];
        if (!D->alive) continue;
        float i = 0.95f * (1.0f - D->age / D->ttl);
        g3_line(g, &cam, D->age < 0.5f ? R_WHITE : R_FIRE, i, 1, vsub(D->c, D->hv), vadd(D->c, D->hv));
    }

    for (int m = 0; m < MAXM; m++) {
        if (!ms[m].alive) continue;
        // the last stretch of the arc as a glowing trail
        Missile tmp = ms[m];
        V3 arc[9];
        for (int k = 0; k < 9; k++) {
            tmp.u = fmaxf(0.0f, ms[m].u - 0.025f * k);
            arc[k] = to_world(missile_pos(&tmp));
        }
        g3_poly(g, &cam, R_FIRE, 0.4f, 3, arc, 5);
        g3_poly(g, &cam, R_FIRE, 0.85f, 1, arc, 9);
        V3 p = arc[0];
        float x, y;
        if (cam_proj(g, &cam, p, &x, &y, NULL)) {
            gx_dot(g, R_YELLOW, 1.0f, x, y, 2.0f * g->lw);
            gx_ring(g, R_YELLOW, 0.35f, 1, x, y, 4.0f * g->lw);
        }
    }

    fx_draw_rings(g, &cam);
    fx_draw(g, &cam);
    draw_salvo(g, t);
}

const SceneDef scene_skirmish = { "Orbital Skirmish", DUR, init, update, draw };
