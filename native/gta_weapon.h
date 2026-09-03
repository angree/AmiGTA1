/* Weapons - Phase 5 item 5: what leaves the gun, and what it does when it
 * arrives.
 *
 * THE RULES ARE THE ORIGINAL'S. Its projectiles are ordinary map objects
 * carrying a heading and a speed and no velocity vector, and there are three
 * of them:
 *
 *   THE BULLET (object 0x4a), fired by the pistol and the machine gun. It is
 *   born 2 units BEHIND the shooter's centre when he stands, 3 back and 4 to
 *   the gun hand when he runs, moves 15 units a tick, and is dead after 20 of
 *   them - four and a half blocks. Every tick it is tested in a box four
 *   units wide and a speed long: the first pedestrian, then the cars, then
 *   the block under it, where a building wall or the water stops it and
 *   leaves a splat (object 0xd). There is no muzzle flash; the tracer is the
 *   bullet's own sprite.
 *
 *   THE ROCKET (object 0x1f) starts 12 units ahead, accelerates from 10 units
 *   a tick to 15, has NO range limit, and explodes on the first thing it
 *   meets. The explosion is four quadrant sprites of twelve frames and it
 *   kills outright inside 35 units, sets alight out to 61, and writes off any
 *   car within 65.
 *
 *   THE FLAME (object 0x4b) is a puff a tick: it starts at the shooter's own
 *   speed plus four, accelerates to seven, lives ten frames - about a block
 *   and a bit - and sets alight every pedestrian it passes through rather
 *   than stopping on the first. A burning man runs, and burns to death in a
 *   hundred ticks.
 *
 * Units: the original's 64 per block are half-pixels here (32 world px a
 * block, 16.16), so "15 units a tick" is 7.5 px and "2 units behind" is one
 * pixel. Ticks are ticks - the projectile update runs once per game tick in
 * the original as well, so no 1.5 scaling applies to a bullet's life.
 *
 * Not built yet: the crates that hold the weapons, the friendly-fire groups,
 * armour, the free-standing fires a rocket leaves against a wall, and the
 * chain explosion of a wrecked car.
 *
 * Portable C89, no floats, no Amiga headers. Licence: MIT (ours).
 */
#ifndef GTA_WEAPON_H
#define GTA_WEAPON_H

#include "gta_nav.h"
#include "gta_tiles.h"
#include "gta_render.h"
#include "gta_peds.h"
#include "gta_traffic.h"
#include "gta_score.h"

/* The weapons, in the original's own order - the order X and Z cycle. */
#define GTA_WEAPON_FIST    0
#define GTA_WEAPON_PISTOL  1
#define GTA_WEAPON_MG      2
#define GTA_WEAPON_ROCKET  3
#define GTA_WEAPON_FLAME   4
#define GTA_WEAPON_COUNT   5

/* What a crate of each holds, and how many shots one unit of ammunition is
 * good for (the machine gun and the flamethrower spend one unit per five).
 *
 * THREE TIMES THE ORIGINAL'S, on the developer's instruction ("dodaj tez 3x
 * tyle amunicji do kazdej broni"). The original's numbers are 20/20/5/10 and
 * they are sized for a city full of crates to top up from; this port has no
 * crates yet, so what you are given at the start is all there is. */
#define GTA_AMMO_PISTOL   60
#define GTA_AMMO_MG       60
#define GTA_AMMO_ROCKET   15
#define GTA_AMMO_FLAME    30
#define GTA_AMMO_PER_UNIT  5

/* Ticks between shots: the pistol 10 (15 for an AI shooter), the machine gun
 * and the flamethrower every other tick, the rocket 20. */
#define GTA_COOL_PISTOL   10
#define GTA_COOL_MG        1
#define GTA_COOL_ROCKET   20
#define GTA_COOL_FLAME     1

/* The original's pool is forty, for every kind of projectile at once. */
#define GTA_MAX_BULLETS   40
#define GTA_MAX_EXPLOSIONS 4

/* Projectile kinds. */
#define GTA_PROJ_BULLET   0
#define GTA_PROJ_ROCKET   1
#define GTA_PROJ_FLAME    2

/* A speed of one unit is half a world pixel, and the step is taken as
 * `gta_sin(a) * GTA_SPEED_Q14(units)` in 16.16 - see gta_weapon.c. */
#define GTA_SPEED_Q14(units)  ((units) * 2)
#define GTA_BULLET_SPEED  15
#define GTA_BULLET_LIFE   20
#define GTA_ROCKET_SPEED  10
#define GTA_ROCKET_SPEED_MAX 15
#define GTA_FLAME_FRAMES  10

/* The explosion: twelve frames a quadrant, two ticks a frame. */
#define GTA_EXPL_FRAMES   12
#define GTA_EXPL_TICKS     2
/* Its reach, in world pixels (the original's 35 / 61 / 65 units). */
#define GTA_EXPL_KILL_PX  17
#define GTA_EXPL_BURN_PX  30
#define GTA_EXPL_CAR_PX   32

/* Where a bullet stopped: the splat object 0xd. The original leaves it as a
 * map object; the developer does not want a trail of them on the map
 * ("slad strzalow zostaje na mapie", 2026-09-02), so here it is a brief
 * flash where the round struck. */
#define GTA_MAX_SPLATS    16
#define GTA_SPLAT_TICKS   8

/* The object types whose sprites the weapons draw - indices into the .til's
 * object table (gta_tiles.objects), which is the game's own object_info:
 * `gtadump objinfo` prints it. */
#define GTA_OBJ_SPLAT     0x0d
#define GTA_OBJ_ROCKET    0x1f
#define GTA_OBJ_FIRE      0x2e
#define GTA_OBJ_BULLET    0x4a
#define GTA_OBJ_FLAME     0x4b

typedef struct {
    long x, y;              /* 16.16 world px */
    int  layer;
    int  heading;           /* 0..255, the direction of travel */
    int  kind;              /* GTA_PROJ_* */
    int  speed;             /* units a tick */
    int  frame;             /* 1..n, the sprite within the object's run */
    int  life;              /* ticks left; 0 = no limit (the rocket) */
    int  owner;             /* ped index of the shooter, -1 = the player */
    int  grace;             /* ticks before it can hit anything - so a running
                             * shooter does not run into his own shot */
    int  alive;
} gta_bullet;

typedef struct {
    long x, y;
    int  layer;
    int  ticks;             /* 0 = free */
} gta_splat;

typedef struct {
    long x, y;
    int  layer;
    int  frame;             /* 1..GTA_EXPL_FRAMES, 0 = free */
    int  tick;
} gta_explosion;

typedef struct {
    gta_bullet b[GTA_MAX_BULLETS];
    gta_splat  s[GTA_MAX_SPLATS];
    gta_explosion x[GTA_MAX_EXPLOSIONS];
    int splat_next;
    /* Resolved once from the tile set, so nothing downstream has to know
     * which object type draws what. -1 when the style has no such object. */
    int spr_bullet, spr_rocket, spr_flame, spr_splat, spr_expl;
    long stat_fired, stat_ped, stat_car, stat_wall, stat_expired, stat_expl;
} gta_weapons;

void gta_weapons_init(gta_weapons *w, const gta_tiles *t);

/* One shot of `weapon` from a shooter at (x,y) facing `angle`; `running`
 * picks the original's running muzzle offset and grace, and is also the
 * shooter's speed for the flame. `owner` is the shooter's ped index, -1 for
 * the player. Returns 0 when nothing was fired (the pool is full, or the
 * weapon does not throw anything). */
int gta_weapons_fire(gta_weapons *w, int weapon, long x, long y, int layer,
                     int angle, int running, int owner);

/* How long the shooter must wait before `weapon` fires again. */
int gta_weapons_cooldown(int weapon);

/* One tick: every projectile moves and is tested, in the original's order,
 * against the pedestrians, the fleet's cars and the block under it; the
 * explosions and the splats age. A hit scores through `sc` - the original
 * awards the kill where the shot lands, not where the body comes to rest. */
void gta_weapons_tick(gta_weapons *w, const gta_nav *nav, gta_peds *peds,
                      gta_traffic *tr, const gta_tiles *t, gta_score *sc);

/* Two passes, because a splat belongs under the traffic and a rocket over
 * it: the ground marks first, then everything in the air. */
void gta_weapons_draw_ground(const gta_weapons *w, gta_view *v);
void gta_weapons_draw_air(const gta_weapons *w, gta_view *v);

int gta_weapons_alive(const gta_weapons *w);

/* A blast at (x,y) - the rocket's, and a wrecked car's. Kills pedestrians
 * inside GTA_EXPL_KILL_PX, sets them alight out to GTA_EXPL_BURN_PX, and
 * writes off cars within GTA_EXPL_CAR_PX (which arms their own fuses). */
void gta_weapons_explode(gta_weapons *w, long x, long y, int layer,
                         gta_peds *peds, gta_traffic *tr, gta_score *sc,
                         int by_player);

/* The five bursts of a car coming apart: its centre and its four corners,
 * as the original does it. */
void gta_weapons_wreck_car(gta_weapons *w, const gta_car_info *ci,
                           long cx, long cy, int face, int layer,
                           gta_peds *peds, gta_traffic *tr, gta_score *sc,
                           int by_player);

#endif /* GTA_WEAPON_H */
