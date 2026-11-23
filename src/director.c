// director.c - runs the five scenes in a loop on a fixed simulation step.

#include "vektor.h"

static const SceneDef* const k_scenes[] = {
    &scene_planet, &scene_wormhole, &scene_skirmish, &scene_stations, &scene_harvester,
};
#define NSCENES ((int)(sizeof k_scenes / sizeof k_scenes[0]))

float view_aspect = 16.0f / 9.0f;

static int g_cur;
static float g_t;
static float g_acc;
static Handoff g_hand;
static Rng g_seeds;

static void begin_scene(int k) {
    g_cur = ((k % NSCENES) + NSCENES) % NSCENES;
    g_t = 0;
    fx_reset();
    k_scenes[g_cur]->init(&g_hand, rng_next(&g_seeds));
}

void director_start(int scene, uint64_t seed) {
    g_seeds.s = seed;
    g_acc = 0;
    ZeroMemory(&g_hand, sizeof g_hand);
    begin_scene(scene);
}

void director_advance(float dt) {
    if (dt > 0.1f) dt = 0.1f;          // a stall (sleep, resume) must not fire one huge step
    g_acc += dt;
    while (g_acc >= SIM_STEP) {
        g_acc -= SIM_STEP;
        const SceneDef* s = k_scenes[g_cur];
        s->update(g_t, SIM_STEP);
        fx_update(SIM_STEP);
        g_t += SIM_STEP;
        if (g_t >= s->duration) begin_scene(g_cur + 1);
    }
}

void director_draw(Gfx* g) { k_scenes[g_cur]->draw(g, g_t); }
int director_scene(void) { return g_cur; }
float director_time(void) { return g_t; }
