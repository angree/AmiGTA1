/* Pedestrians - Phase 5 item 4, the first cut.
 *
 * The city's other population. Same shape as the traffic: a small pool kept
 * alive around the view, spawned on PAVEMENT, despawned when the camera
 * leaves them behind. A ped WALKS: a straight line along a compass heading,
 * a decision at each block centre - keep going if the next block is still
 * pavement, otherwise turn toward one that is - and an about-face in the
 * pocket where nothing is. No goals, no fleeing, no crossing the road yet:
 * that is the AI half of item 4 and it comes after the developer has seen
 * people on the streets at all.
 *
 * The player's own sprite category (7, the ped sheet) and his 8-frame walk
 * cycle are reused frame for frame - one sheet animates the whole city.
 *
 * Run over: gta_peds_ram() is the player-car half. A hit ped is DOWN (one
 * sprite, no motion) for a few seconds, then the slot respawns elsewhere.
 * Blood and score belong to later items.
 *
 * Portable C89, no floats, no Amiga headers. Licence: MIT (ours).
 */
#ifndef GTA_PEDS_H
#define GTA_PEDS_H

#include "gta_map.h"
#include "gta_nav.h"
#include "gta_tiles.h"
#include "gta_render.h"

#define GTA_MAX_PEDS 12

/* How long somebody pulled out of a car stays down before getting up and
 * walking off. Two and a half seconds at 50 Hz - long enough to read as
 * "he was thrown out", short enough that the street does not fill with
 * bodies. */
#define GTA_STUN_TICKS 125

typedef struct {
    long x, y;              /* 16.16 world px */
    int  layer;
    int  angle;             /* 0..255, walking heading */
    /* MOST PEOPLE WALK. One in eight is in a hurry, which is what keeps a
     * street from looking like a parade - but the default is a walk, and the
     * frames and the speed have to agree about which it is. */
    int  running;
    int  frame;             /* index within the walk or run cycle */
    int  frame_tick;
    /* WHAT THIS PERSON IS WEARING - a palette remap table, 128..187.
     *
     * Without it every pedestrian in the city was drawn through the player's
     * own palette, so the streets were full of clones of him. The range is the
     * .GRY's own: see gta_style.h. Picked once at spawn and kept, because a
     * person who changes colour as they walk is worse than a clone. */
    int  remap;
    int  down;              /* >0: on the ground, ticks left there */
    int  stunned;           /* down because they were pulled out, not run over:
                             * they get up again when the count runs out */
    int  alive;
} gta_ped;

typedef struct {
    gta_ped p[GTA_MAX_PEDS];
    const gta_tiles *tiles;
    const gta_nav *nav;
    int ped_base, ped_count;
    unsigned long rng;
    int spawn_wait;
    long stat_spawned, stat_runover;
} gta_peds;

void gta_peds_init(gta_peds *ps, const gta_tiles *t, unsigned long seed);
void gta_peds_set_nav(gta_peds *ps, const gta_nav *nav);

/* One 50 Hz tick: walk everybody, recycle the down, keep the pool filled
 * around the camera block. */
void gta_peds_tick(gta_peds *ps, const gta_map *m, long cam_x, long cam_y);

/* The player's car against the crowd: any ped inside the car's box goes
 * down. Returns how many were hit this tick. */
int gta_peds_ram(gta_peds *ps, long px, long py, int pface, int phl, int phw,
                 int layer);

/* THROWN OUT OF A CAR. Puts a person on the ground at (x,y) facing `angle`,
 * stunned - flat on the road for `stun` ticks, then up and walking like
 * anybody else. This is the other half of a carjacking: the driver does not
 * evaporate, they land next to their car.
 *
 * `remap` is the colour they wear; pass -1 to have one picked. Returns 0 if
 * every slot is taken, which costs nothing but the driver. */
int gta_peds_drop(gta_peds *ps, long x, long y, int layer, int angle,
                  int remap, int stun);

void gta_peds_draw(gta_peds *ps, gta_view *v);

#endif /* GTA_PEDS_H */
