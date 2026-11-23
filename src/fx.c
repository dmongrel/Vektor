// fx.c - burning particles with fading trails, and expanding rings/flash discs.
// Fixed pools: nothing is allocated while a scene runs.

#include "vektor.h"

#define MAXP 3000
#define TRAIL 10
#define TRAIL_DT (1.0f / 30.0f)
#define MAXR 160
#define MAXIMPACT 64

typedef struct {
    V3 p, v;
    float age, ttl, heat, drag, size, trail_clock;
    uint8_t ramp, grav, trail_len, trail_n, alive;
    V3 tr[TRAIL];          // tr[0] newest
} Particle;

typedef struct {
    V3 c, n;
    float r0, r1, age, ttl, i;
    uint8_t ramp, mode, alive;
} Ring;

FxWorld fx_world;
static Particle g_p[MAXP];
static int g_hi;           // one past the highest live slot
static int g_free_hint;
static int g_count;
static Ring g_r[MAXR];
static V3 g_impact[MAXIMPACT];
static int g_nimpact;

void fx_reset(void) {
    for (int k = 0; k < MAXP; k++) g_p[k].alive = 0;
    for (int k = 0; k < MAXR; k++) g_r[k].alive = 0;
    g_hi = g_free_hint = g_count = g_nimpact = 0;
    fx_world.g = v3(0, 0, 0);
    fx_world.center = v3(0, 0, 0);
    fx_world.gm = 0;
    fx_world.kill_r = 0;
    fx_world.atmo_r = 0;
    fx_world.atmo_k = 0;
    fx_world.fade = 1.0f;
}

int fx_count(void) { return g_count; }

void fx_spawn(const PSpawn* s) {
    int k = -1;
    for (int n = 0; n < MAXP; n++) {
        int j = (g_free_hint + n) % MAXP;
        if (!g_p[j].alive) { k = j; break; }
    }
    if (k < 0) return;     // pool full: drop it
    g_free_hint = (k + 1) % MAXP;
    if (k >= g_hi) g_hi = k + 1;
    Particle* p = &g_p[k];
    p->p = s->p;
    p->v = s->v;
    p->age = s->age;
    p->ttl = s->ttl;
    p->heat = s->heat;
    p->drag = s->drag;
    p->size = s->size > 0 ? s->size : 1.0f;
    p->ramp = s->ramp;
    p->grav = s->grav;
    p->trail_len = s->trail_len > TRAIL ? TRAIL : s->trail_len;
    p->trail_n = 0;
    p->trail_clock = 0;
    p->alive = 1;
    g_count++;
}

void fx_burst(Rng* r, V3 p, V3 vel, int n, float smin, float smax, float ttl, int ramp, float heat, uint8_t grav) {
    PSpawn s = { 0 };
    for (int k = 0; k < n; k++) {
        s.p = p;
        s.v = vadd(vel, vmul(rand_unit(r), rr(r, smin, smax)));
        s.ttl = ttl * rr(r, 0.6f, 1.2f);
        s.heat = heat * rr(r, 0.85f, 1.0f);
        s.drag = 0.6f;
        s.size = rr(r, 0.8f, 1.4f);
        s.ramp = (uint8_t)ramp;
        s.grav = grav;
        s.trail_len = 8;
        fx_spawn(&s);
    }
}

bool fx_pop_impact(V3* where) {
    if (g_nimpact == 0) return false;
    *where = g_impact[--g_nimpact];
    return true;
}

void fx_update(float dt) {
    int hi = 0;
    for (int k = 0; k < g_hi; k++) {
        Particle* p = &g_p[k];
        if (!p->alive) continue;
        p->age += dt;
        if (p->age >= p->ttl) { p->alive = 0; g_count--; continue; }
        V3 a = v3(0, 0, 0);
        float drag = p->drag;
        if (p->grav == GRAV_DIR) a = fx_world.g;
        else if (p->grav == GRAV_CENTER) {
            V3 d = vsub(fx_world.center, p->p);
            float r2 = vdot(d, d), r = sqrtf(r2);
            if (r < fx_world.kill_r) {
                if (g_nimpact < MAXIMPACT) g_impact[g_nimpact++] = p->p;
                p->alive = 0;
                g_count--;
                continue;
            }
            a = vmul(d, fx_world.gm / (fmaxf(r2, 0.04f) * r));
            if (r < fx_world.atmo_r) {
                float depth = (fx_world.atmo_r - r) / (fx_world.atmo_r - fx_world.kill_r + 1e-3f);
                drag += fx_world.atmo_k * depth;
                p->heat = fminf(1.0f, p->heat + depth * dt * 0.8f);   // re-entry glow
            }
        }
        p->v = vadd(vmul(p->v, 1.0f / (1.0f + drag * dt)), vmul(a, dt));
        p->p = vmad(p->p, p->v, dt);
        p->trail_clock += dt;
        if (p->trail_len && p->trail_clock >= TRAIL_DT) {
            p->trail_clock -= TRAIL_DT;
            int n = p->trail_n < p->trail_len ? p->trail_n + 1 : p->trail_len;
            for (int j = n - 1; j > 0; j--) p->tr[j] = p->tr[j - 1];
            p->tr[0] = p->p;
            p->trail_n = (uint8_t)n;
        }
        hi = k + 1;
    }
    g_hi = hi;
    for (int k = 0; k < MAXR; k++) {
        Ring* r = &g_r[k];
        if (r->alive && (r->age += dt) >= r->ttl) r->alive = 0;
    }
}

void fx_draw(Gfx* g, const Cam* c) {
    float fade = fx_world.fade;
    if (fade <= 0) return;
    for (int k = 0; k < g_hi; k++) {
        Particle* p = &g_p[k];
        if (!p->alive) continue;
        float life = 1.0f - p->age / p->ttl;
        float heat = p->heat * fade * (0.25f + 0.75f * life);
        float hx, hy, z;
        if (!cam_proj(g, c, p->p, &hx, &hy, &z)) continue;
        // Trail: newest segments hottest, cooling toward the tail (orange -> red -> dark
        // on the fire ramp, dimming on the others).
        if (p->trail_n > 0) {
            POINT pts[TRAIL + 1];
            pts[0].x = (LONG)hx;
            pts[0].y = (LONG)hy;
            int m = 1;
            for (int j = 0; j < p->trail_n; j++) {
                float x, y;
                if (!cam_proj(g, c, p->tr[j], &x, &y, NULL)) break;
                pts[m].x = (LONG)x;
                pts[m].y = (LONG)y;
                m++;
            }
            // three bands, each one polyline
            int b1 = m > 3 ? m / 3 + 1 : m, b2 = m > 3 ? (2 * m) / 3 + 1 : m;
            gx_hair(g, p->ramp, heat * 0.80f, pts, b1);
            if (b2 > b1) gx_hair(g, p->ramp, heat * 0.58f, pts + b1 - 1, b2 - b1 + 1);
            if (m > b2) gx_hair(g, p->ramp, heat * 0.36f, pts + b2 - 1, m - b2 + 1);
        }
        float rp = p->size * g->lw * (c->ortho ? 1.0f : clampf(3.0f / z, 0.5f, 2.0f));
        gx_dot(g, p->ramp, heat, hx, hy, rp);
    }
    gx_hair_flush(g);
}

void fx_ring(V3 c, V3 n, float r0, float r1, float ttl, int ramp, float i, int mode) {
    for (int k = 0; k < MAXR; k++) {
        Ring* r = &g_r[k];
        if (r->alive) continue;
        r->c = c;
        r->n = vnorm(n);
        r->r0 = r0;
        r->r1 = r1;
        r->age = 0;
        r->ttl = ttl;
        r->i = i;
        r->ramp = (uint8_t)ramp;
        r->mode = (uint8_t)mode;
        r->alive = 1;
        return;
    }
}

void fx_draw_rings(Gfx* g, const Cam* c) {
    float fade = fx_world.fade;
    for (int k = 0; k < MAXR; k++) {
        Ring* r = &g_r[k];
        if (!r->alive) continue;
        float u = r->age / r->ttl;
        float rad = lerpf(r->r0, r->r1, 1.0f - (1.0f - u) * (1.0f - u));
        float i = r->i * (1.0f - u) * fade;
        if (r->mode == RING_PLANE) {
            V3 a = vperp(r->n), b = vcross(r->n, a);
            V3 pts[33];
            for (int j = 0; j <= 32; j++) {
                float t = TAU * j / 32.0f;
                pts[j] = vadd(r->c, vadd(vmul(a, cosf(t) * rad), vmul(b, sinf(t) * rad)));
            }
            g3_poly(g, c, r->ramp, i, 1, pts, 33);
            continue;
        }
        float x, y, z;
        if (!cam_proj(g, c, r->c, &x, &y, &z)) continue;
        float pr = rad * cam_scale(g, c, z);
        if (r->mode == RING_DISC) {
            gx_disc(g, r->ramp, i, x, y, pr);
        } else {
            gx_ring(g, r->ramp, i * 0.4f, 3, x, y, pr);
            gx_ring(g, r->ramp, i, 1, x, y, pr);
        }
    }
}
