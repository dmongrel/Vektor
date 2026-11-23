// vektor.h - shared types for the Vektor screensaver: math, RNG, drawing, effects, scenes.
#pragma once

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>

#define PI  3.14159265358979f
#define TAU 6.28318530717959f

// ---- scalar helpers ------------------------------------------------------------------

static inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
static inline float sat(float x) { return clampf(x, 0.0f, 1.0f); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smooth(float t) { t = sat(t); return t * t * (3.0f - 2.0f * t); }
// 0 before a, 1 after b, smooth between.
static inline float ramp01(float x, float a, float b) { return smooth((x - a) / (b - a)); }
static inline float lin01(float x, float a, float b) { return sat((x - a) / (b - a)); }

// ---- vectors -----------------------------------------------------------------------

typedef struct { float x, y, z; } V3;

static inline V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static inline V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 vmul(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline V3 vmad(V3 a, V3 b, float s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
static inline float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 vcross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float vlen(V3 a) { return sqrtf(vdot(a, a)); }
static inline V3 vnorm(V3 a) { float l = vlen(a); return l > 1e-12f ? vmul(a, 1.0f / l) : v3(0, 1, 0); }
static inline V3 vlerp(V3 a, V3 b, float t) { return vmad(a, vsub(b, a), t); }

// Rotate v about unit axis k by angle a (Rodrigues).
static inline V3 vrot(V3 v, V3 k, float a) {
    float c = cosf(a), s = sinf(a);
    return vadd(vadd(vmul(v, c), vmul(vcross(k, v), s)), vmul(k, vdot(k, v) * (1.0f - c)));
}

// Any unit vector perpendicular to unit n.
static inline V3 vperp(V3 n) {
    V3 a = fabsf(n.x) < 0.9f ? v3(1, 0, 0) : v3(0, 1, 0);
    return vnorm(vcross(n, a));
}

// 3x3 rotation, rows r[0..2]: world = M * local.
typedef struct { V3 r[3]; } M3;

static inline M3 m3_id(void) { M3 m = { { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } }; return m; }
static inline V3 m3_mul(const M3* m, V3 v) { return v3(vdot(m->r[0], v), vdot(m->r[1], v), vdot(m->r[2], v)); }
static inline V3 m3_tmul(const M3* m, V3 v) {   // transpose: local = M^T * world
    return vadd(vadd(vmul(m->r[0], v.x), vmul(m->r[1], v.y)), vmul(m->r[2], v.z));
}
static inline M3 m3_axis(V3 k, float a) {
    M3 m;
    V3 cx = vrot(v3(1, 0, 0), k, a), cy = vrot(v3(0, 1, 0), k, a), cz = vrot(v3(0, 0, 1), k, a);
    m.r[0] = v3(cx.x, cy.x, cz.x);
    m.r[1] = v3(cx.y, cy.y, cz.y);
    m.r[2] = v3(cx.z, cy.z, cz.z);
    return m;
}
static inline M3 m3_mulm(const M3* a, const M3* b) {   // a * b
    M3 m;
    for (int i = 0; i < 3; i++) {
        V3 r = a->r[i];
        m.r[i] = v3(r.x * b->r[0].x + r.y * b->r[1].x + r.z * b->r[2].x,
                    r.x * b->r[0].y + r.y * b->r[1].y + r.z * b->r[2].y,
                    r.x * b->r[0].z + r.y * b->r[1].z + r.z * b->r[2].z);
    }
    return m;
}

// ---- random ------------------------------------------------------------------------

typedef struct { uint64_t s; } Rng;

static inline uint64_t rng_next(Rng* r) {          // splitmix64
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static inline float rf(Rng* r) { return (float)(rng_next(r) >> 40) * (1.0f / 16777216.0f); }
static inline float rr(Rng* r, float a, float b) { return a + (b - a) * rf(r); }
static inline int ri(Rng* r, int n) { return (int)(rf(r) * (float)n) % (n > 0 ? n : 1); }
static inline V3 rand_unit(Rng* r) {
    float z = rr(r, -1, 1), a = rr(r, 0, TAU), s = sqrtf(1 - z * z);
    return v3(s * cosf(a), s * sinf(a), z);
}
// Cheap smooth 1D noise in [-1,1], periodic over `period` integer cells.
float noise1(uint32_t seed, float x, int period);

// ---- colour ramps ------------------------------------------------------------------
// Every colour is a ramp and an intensity. Intensity 0 is black, 0.75 is the pure hue,
// 1.0 is the hue pushed toward white (the "hot" core).

enum { R_CYAN, R_MAGENTA, R_YELLOW, R_GREEN, R_RED, R_WHITE, R_FIRE, R_ORANGE, NRAMP };
#define NLEVEL 32
#define HUE 0.74f

// ---- drawing -----------------------------------------------------------------------
// The view spans the whole drawing surface. One view unit is half the surface height,
// y is up, (0,0) is the centre.

typedef struct {
    HDC dc;
    int w, h;
    float cx, cy, unit;
    float aspect;          // half-width in view units
    int lw;                // base stroke width in pixels for this resolution
    int pen_key;           // currently selected pen, -1 none
} Gfx;

void gfx_startup(void);
void gfx_shutdown(void);
void gfx_begin(Gfx* g, HDC dc, int w, int h);   // clears to black, sets up state
void gfx_end(Gfx* g);

COLORREF ramp_color(int ramp, float i);

static inline float gx_px(const Gfx* g, float vx) { return g->cx + vx * g->unit; }
static inline float gx_py(const Gfx* g, float vy) { return g->cy - vy * g->unit; }

// Pixel-space primitives. `w` is a weight: the pixel width is w * g->lw.
void gx_line(Gfx* g, int ramp, float i, int w, float x0, float y0, float x1, float y1);
void gx_poly(Gfx* g, int ramp, float i, int w, const POINT* p, int n);
void gx_hair(Gfx* g, int ramp, float i, const POINT* p, int n);   // 1 px, batched by colour
void gx_hair_flush(Gfx* g);
void gx_glow_poly(Gfx* g, int ramp, float i, int w, const POINT* p, int n);
void gx_dot(Gfx* g, int ramp, float i, float x, float y, float rpx);
void gx_ring(Gfx* g, int ramp, float i, int w, float x, float y, float r);
void gx_disc(Gfx* g, int ramp, float i, float x, float y, float r);
void gx_flash(Gfx* g, int ramp, float i, float alpha);   // whole-surface tint

// ---- camera ------------------------------------------------------------------------

typedef struct {
    V3 pos, fwd, right, up;
    float focal;           // view units per unit of x/z
    float znear;
    bool ortho;            // 2D scenes: world x,y are view units
} Cam;

void cam_look(Cam* c, V3 pos, V3 target, V3 up_hint, float fov_deg);
void cam_ortho(Cam* c);
// Projects to pixels. Returns false if behind the near plane. *depth is camera-space z.
bool cam_proj(const Gfx* g, const Cam* c, V3 p, float* x, float* y, float* depth);
// Pixel size of a world length at depth z.
static inline float cam_scale(const Gfx* g, const Cam* c, float z) {
    return c->ortho ? g->unit : g->unit * c->focal / (z > 1e-4f ? z : 1e-4f);
}
// True if the segment from the camera to p passes through the sphere (center, r) first.
bool sphere_occludes(V3 cam, V3 center, float r, V3 p);

void g3_line(Gfx* g, const Cam* c, int ramp, float i, int w, V3 a, V3 b);
// Polyline through 3D points; breaks where a point is behind the camera.
void g3_poly(Gfx* g, const Cam* c, int ramp, float i, int w, const V3* p, int n);

// Lat/long wireframe sphere with back lines hidden.
typedef struct {
    V3 center;
    float radius;
    M3 rot;                // local -> world
    int nlat, nlon;        // number of latitude rings and meridians
    int ramp;
    float i;
    int w;
    float reveal;          // 0..1: lines appear one at a time (intro); 1 = all
    bool from_equator;     // reveal spreads outward from the equator instead
    bool clip;             // keep only local points with dot(p, clip_n) * clip_side >= 0
    V3 clip_n;
    float clip_side;
} SphereGrid;

void draw_sphere_grid(Gfx* g, const Cam* c, const SphereGrid* s);
void sphere_grid_defaults(SphereGrid* s);

// Jagged shard outline (2D, view units) shared by the debris scenes.
int shard_shape(uint32_t seed, float size, float rot, float x, float y, float* out_xy, int max);
// Outline of a shard at view-unit position (x,y): white at intensity i, with a fire glow.
void draw_shard(Gfx* g, float x, float y, float size, float rot, uint32_t seed, float heat, float i);
#define SHARD_I 0.62f

// ---- particles and rings -----------------------------------------------------------

enum { GRAV_NONE, GRAV_DIR, GRAV_CENTER };

typedef struct {
    V3 g;                  // GRAV_DIR acceleration
    V3 center;             // GRAV_CENTER attractor
    float gm;              // GRAV_CENTER strength (a = gm / r^2, capped)
    float kill_r;          // GRAV_CENTER: particles inside this radius die and log an impact
    float atmo_r, atmo_k;  // extra drag and heat inside atmo_r
    float fade;            // global intensity multiplier
} FxWorld;

extern FxWorld fx_world;

typedef struct {
    V3 p, v;
    float age, ttl;
    float heat;            // starting intensity
    float drag;
    float size;            // head radius in base-width units
    uint8_t ramp, grav, trail_len;
} PSpawn;

void fx_reset(void);
void fx_spawn(const PSpawn* s);
// A burst of n particles around p with speed range, inheriting velocity vel.
void fx_burst(Rng* r, V3 p, V3 vel, int n, float smin, float smax, float ttl, int ramp, float heat, uint8_t grav);
void fx_update(float dt);
void fx_draw(Gfx* g, const Cam* c);
int fx_count(void);
bool fx_pop_impact(V3* where);

// Expanding rings and flash discs.
enum { RING_SCREEN, RING_PLANE, RING_DISC };
void fx_ring(V3 c, V3 n, float r0, float r1, float ttl, int ramp, float i, int mode);
void fx_draw_rings(Gfx* g, const Cam* c);

// ---- scenes ------------------------------------------------------------------------

typedef struct {
    float x, y, vx, vy, size, rot, spin;
    uint32_t seed;
} Shard2;

#define MAX_HAND_SHARDS 96

// State passed from one scene to the next so a transition can end exactly where the
// next scene begins.
typedef struct {
    bool has12, has34, has45;
    uint32_t jag_seed;                  // 1 -> 2: jagged mouth of the wormhole
    int n_stations;                     // 3 -> 4: salvo missiles become stations
    float ring_phase;
    int n_shards;                       // 4 -> 5: escaped fragments, view units
    Shard2 shards[MAX_HAND_SHARDS];
} Handoff;

typedef struct {
    const char* name;
    float duration;
    void (*init)(Handoff* h, uint64_t seed);
    void (*update)(float t, float dt);
    void (*draw)(Gfx* g, float t);
} SceneDef;

extern const SceneDef scene_planet, scene_wormhole, scene_skirmish, scene_stations, scene_harvester;

// Geometry each scene exports for the transition into it.
// Screen points of mouth ring `ring` (0 nearest) at the wormhole's first frame; returns its
// intensity. n points, evenly spaced in angle starting at angle 0.
#define MOUTH_RINGS 7
float wormhole_mouth_points(const Gfx* g, const Handoff* h, int ring, POINT* out, int n);
int skirmish_equator_points(const Gfx* g, POINT* out, int n);
void stations_camera(Cam* c, float t);
V3 stations_slot(const Handoff* h, int i, float t);
float stations_planet_r(void);
extern const float STATIONS_ELEV;

// ---- director ----------------------------------------------------------------------

#define SIM_STEP (1.0f / 120.0f)

// Width / height of the drawing surface; set by the shell before director_start.
extern float view_aspect;

void director_start(int scene, uint64_t seed);
void director_advance(float dt);
void director_draw(Gfx* g);
int director_scene(void);
float director_time(void);
