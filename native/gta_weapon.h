/* Weapons - Phase 5 item 5: what leaves the gun, and what it does when it
 * arrives.
 *
 * THE RULES ARE THE ORIGINAL'S. Its projectiles are ordinary map objects
 * carrying a heading and a speed and no
 * velocity vector: the pistol's bullet is object 0x4a, born 2 units BEHIND
 * the shooter's centre when he stands (3 back and 4 to the right, the gun
 * hand, when he runs), moving 15 units a tick along his heading, dead after
 * 20 ticks, and tested every tick in a box four units wide and a speed's
 * length long against the first pedestrian, then the cars, then the block
 * under it - a building wall or water stops it with a splat (object 0xd),
 * anything else it flies over. There is no muzzle flash: the tracer is the
 * bullet's own sprite. At most forty projectiles of all kinds live at once.
 *
 * Units: the original's 64 per block are half-pixels here (32 world px a
 * block, 16.16), so "15 units a tick" is 7.5 px and "2 units behind" is one
 * pixel. Ticks are ticks - the projectile update runs once per game tick in
 * the original as well, so no 1.5 scaling applies to the bullet's life.
 *
 * Not built yet: the machine gun (the same bullet at cooldown 1), the rocket
 * (0x1f) and its explosion, the flamethrower (0x4b) and fire, the crate
 * opened by a bullet, the friendly-fire groups, armour.
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

/* The original's pool is forty, for every kind of projectile at once. */
#define GTA_MAX_BULLETS   40
/* 15 units a tick = 7.5 px; Q14 sin x 30 is 7.5 px in 16.16. */
#define GTA_BULLET_STEP_Q14  30
#define GTA_BULLET_LIFE   20
/* The player's pistol cooldown, ticks; an AI shooter's is 15. */
#define GTA_PISTOL_COOLDOWN  10
#define GTA_PISTOL_AMMO      20      /* what a crate holds */

/* Where a bullet stopped: the splat object 0xd. The original leaves it as a
 * map object; the developer does not want a trail of them on the map
 * ("slad strzalow zostaje na mapie", 2026-09-02), so here it is a brief
 * flash where the round struck. */
#define GTA_MAX_SPLATS    16
#define GTA_SPLAT_TICKS   8

/* The object types whose sprites the weapons draw - indices into the .til's
 * object table (gta_tiles.objects), which is the .GRY's object_info section:
 * `gtadump objinfo` prints it. */
#define GTA_OBJ_SPLAT     0x0d
#define GTA_OBJ_BULLET    0x4a
#define GTA_OBJ_ROCKET    0x1f
#define GTA_OBJ_FLAME     0x4b

typedef struct {
    long x, y;              /* 16.16 world px */
    int  layer;
    int  heading;           /* 0..255, the direction of travel and the sprite */
    int  life;              /* ticks left */
    int  owner;             /* ped index of the shooter, -1 = the player */
    int  grace;             /* ticks before it can hit anything - the
                             * original's 2-tick grid delay for a running
                             * shooter, so he does not run into his own shot */
    int  alive;
} gta_bullet;

typedef struct {
    long x, y;
    int  layer;
    int  ticks;             /* 0 = free */
} gta_splat;

typedef struct {
    gta_bullet b[GTA_MAX_BULLETS];
    gta_splat  s[GTA_MAX_SPLATS];
    int splat_next;
    long stat_fired, stat_ped, stat_car, stat_wall, stat_expired;
} gta_weapons;

void gta_weapons_init(gta_weapons *w);

/* One pistol round from a shooter at (x,y) facing `angle`; `running` picks
 * the original's running muzzle offset and grace. `owner` is the shooter's
 * ped index, -1 for the player. Returns 0 when the pool is full (the
 * original then fires nothing either). */
int gta_weapons_fire_pistol(gta_weapons *w, long x, long y, int layer,
                            int angle, int running, int owner);

/* One tick: every bullet moves and is tested, in the original's order,
 * against the pedestrians, the fleet's cars and the block under it. */
void gta_weapons_tick(gta_weapons *w, const gta_nav *nav, gta_peds *peds,
                      gta_traffic *tr, const gta_tiles *t);

/* Queue the tracers and the splats. `bullet_sprite` / `splat_sprite` are
 * absolute sprite indices, -1 to draw none. */
void gta_weapons_draw(const gta_weapons *w, gta_view *v, int bullet_sprite,
                      int splat_sprite);

int gta_weapons_alive(const gta_weapons *w);

#endif /* GTA_WEAPON_H */
