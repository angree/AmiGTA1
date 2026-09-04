/* THE CRATES - Phase 5, the pickups.
 *
 * The original places its power-ups from the level script: every entry
 * `N (x,y,z) POWERUP kind amount` in mission.ini's section for the level is
 * a crate at that block. Liberty City has 151 of them - 46 pistols, 34
 * machine guns, then armour, speed, bribes, multipliers, jail-free cards,
 * lives and kill frenzies; no rockets and no flamethrowers, which the
 * missions hand out. Until there is a script interpreter the crates ARE
 * the script's contribution, so this reads exactly those lines and nothing
 * else, and the file is optional: without it the port hands the player the
 * old loadout and says so in the log.
 *
 * A crate (object 0x54) is OPENED by a bullet, a rocket, a flame, an
 * explosion, the player walking into it or driving over it; it then shows
 * the item (0x4e..0x51 for the four weapons, 0x5f..0x65 for the rest) on an
 * open crate (0x55), and the player COLLECTS the item by touching it, on
 * foot or by car. What each kind does to the player is the game's business
 * (gta_main.c): this module only keeps the crates.
 *
 * Portable C89, no floats, no Amiga headers. Licence: MIT (ours).
 */
#ifndef GTA_PICKUP_H
#define GTA_PICKUP_H

#include "gta_tiles.h"
#include "gta_nav.h"
#include "gta_render.h"

#define GTA_MAX_PICKUPS 160

/* The kinds, as the script numbers them. */
#define GTA_PICKUP_PISTOL     1
#define GTA_PICKUP_MG         2
#define GTA_PICKUP_ROCKET     3
#define GTA_PICKUP_FLAME      4
#define GTA_PICKUP_SPEED      6      /* 6, 7 and 8 all mean speed */
#define GTA_PICKUP_BRIBE      9
#define GTA_PICKUP_ARMOUR     10
#define GTA_PICKUP_MULTIPLIER 11
#define GTA_PICKUP_JAILFREE   12
#define GTA_PICKUP_LIFE       13     /* and 15 */
#define GTA_PICKUP_FRENZY     14

#define GTA_PICKUP_CRATE 1           /* state: a closed crate */
#define GTA_PICKUP_OPEN  2           /* opened, the item on show */

typedef struct {
    long x, y;              /* 16.16 world, the block's centre */
    int  layer;
    int  kind, amount;
    int  state;             /* 0 = gone */
} gta_pickup;

typedef struct {
    gta_pickup p[GTA_MAX_PICKUPS];
    int n;
    const gta_tiles *tiles;
    int spr_crate, spr_open;
    int spr_item[16];       /* by kind, -1 = none */
    long stat_opened, stat_taken;
} gta_pickups;

/* Read the POWERUP lines of section [level] of `ini_path`. Every crate is
 * put at the centre of its block, on the lowest layer there that a person
 * can stand on. Returns how many, 0 when the file is absent. */
int gta_pickups_load(gta_pickups *pk, const char *ini_path, int level,
                     const gta_nav *nav, const gta_tiles *t);

/* Queue the crates within `blocks` of the camera for drawing. */
void gta_pickups_draw(gta_pickups *pk, gta_view *v, int blocks);

/* Open every closed crate within `radius` px of (x,y) on `layer`. Returns
 * how many opened. */
int gta_pickups_open_at(gta_pickups *pk, long x, long y, int layer, int radius);

/* Take the first open item within `radius` px of (x,y) on `layer`: it is
 * gone, and its kind and amount are returned. 0 when there is none. */
int gta_pickups_take(gta_pickups *pk, long x, long y, int layer, int radius,
                     int *kind, int *amount);

/* A crate for the test scripts. Returns 0 when the table is full. */
int gta_pickups_add(gta_pickups *pk, long x, long y, int layer, int kind, int amount);

#endif /* GTA_PICKUP_H */
