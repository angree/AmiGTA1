/* Tyre marks and wheel smoke - what a sliding car leaves behind.
 *
 * FROM THE ORIGINAL: the original's routine runs for a car whose slide force is over the
 * limit and lays, at each wheel line, a smoke puff (object 0x0a) and a mark
 * (object 0x0b). The wheel lines are a quarter of the car's width either side
 * of its centre; a bike has one, on its centre. the original's routine is the spawner:
 *
 *   mark   only on road, pavement or field (ground 2, 3, 4), never on a block
 *          with the nav's 0x80 bit and never in the air; at most 16 in one
 *          two-block cell and 200 in the world, and over either limit there is
 *          simply no new mark. Four frames of 128 original ticks, then gone.
 *   smoke  only on road; at most 2 in a two-block cell. Eleven frames of 2
 *          original ticks, drifting one original unit a tick towards +x, -y.
 *
 * THE CLOCK: the original's objects run at 25 Hz, so all of this steps on every
 * second tick of the port's 50.
 *
 * NOT DONE, each needing something the port does not have yet:
 *   - mark 0x11 on the lids listed in the original's table: table not read;
 *   - cars other than the player's: only his runs the physics that slides.
 *
 * BLOOD, from the same object system. A ped that dies gets a pool, object 0x3f
 * (12 frames of 24 original ticks, the state change to 0x2d in the original's routine,
 * the original's routine, the original's routine), and on the first tick of the pool's fifth frame
 * the pool spawns the big splat 0x40 (the original's routine). A body shot along the
 * ground leaves a trail of 0x4d at every step of its slide. Both kinds stay for
 * as long as the body lies there and go when it goes (their clock does not run
 * in the dead state 0x17, and state 0x18 ends them, the original's routine). A ped who
 * burned to death does not bleed here: inferred, not traced.
 *
 * SMOKE OF A FIRE AND OF AN EXPLOSION. A fire on its first frame tries to lay
 * the same road-only puff as a wheel (the original's routine). An explosion at frame 10
 * makes one black cloud, object 0x33: 12 frames of 2 original ticks, drifting
 * like the puff, drawn above the fireball (the original's routine). Both also RISE one
 * unit a tick in the original; a sprite here sits on a whole layer, and 22
 * units is a third of one, so they do not.
 *
 * THE BLOODY MARK. A wheel line within 8 original units (4 px) of a pool or a
 * splat arms 12..21 marks of 0x0c in place of 0x0b (the original's routine, the original's routine,
 * and every line laid uses one up). Like everything else here it only happens
 * while sliding: the original's routine is the slide. Object 0x0f also arms it and is
 * unidentified.
 *
 * Portable C89, no floats, no Amiga headers. Licence: MIT (ours). */
#ifndef GTA_MARKS_H
#define GTA_MARKS_H

#include "gta_nav.h"
#include "gta_tiles.h"
#include "gta_render.h"
#include "gta_peds.h"
#include "gta_weapon.h"

#define GTA_MAX_MARKS     200
#define GTA_MAX_SMOKES    24
#define GTA_MARK_CELLS    128   /* two-block cells a side */
#define GTA_MAX_STAINS    48
#define GTA_MAX_CLOUDS    GTA_MAX_EXPLOSIONS
#define GTA_MAX_DEBRIS    100   /* five a wall: the bank job blows sixteen */

typedef struct {
    long x, y;
    short left;                 /* original ticks to live; 0 = free */
    unsigned char layer, angle;
    short sprite;
} gta_mark;

typedef struct {
    long x, y;
    short left;                 /* a pool's original ticks; 0 = free */
    short ped;                  /* the body a splat belongs to */
    unsigned char layer, kind;  /* 0x3f, 0x40 or 0x4d; 0 = free */
} gta_stain;

typedef struct {
    gta_mark m[GTA_MAX_MARKS];
    gta_stain st[GTA_MAX_STAINS];
    gta_mark cloud[GTA_MAX_CLOUDS];
    int spr_cloud, spr_bloody, spr_splash;
    int bloody;                 /* bloody marks still to lay */
    int st_next;
    int spr_pool, spr_splat, spr_trail;
    long stat_pools;            /* for the log */
    gta_mark s[GTA_MAX_SMOKES];
    unsigned char cell[GTA_MARK_CELLS * GTA_MARK_CELLS];
    int count;
    int next;                   /* where the search for a free mark starts */
    int half;
    int spr_mark, spr_smoke;
    long stat_smoke;            /* puffs laid since the start: for the log */
    /* THE RUBBLE of a blown wall (the original's routine): static objects, they stay */
    gta_mark debris[GTA_MAX_DEBRIS];
    int debris_next;
    int spr_rubble[3], spr_lump;
    /* SPARKS (0x0d, 5 frames of one original tick): a car coming down */
    gta_mark spark[8];
    int spr_spark;
    long stat_sparks;
} gta_marks;

void gta_marks_init(gta_marks *g, const gta_tiles *t);

/* The car is sliding this tick. (x, y) is its centre, `wid` its width in world
 * pixels, `heading` the 256-step body angle, (vx, vy) its velocity. */
void gta_marks_slide(gta_marks *g, const gta_nav *nav, long x, long y, int layer,
                     int wid, int heading, long vx, long vy, int single);

/* A pool of blood not tied to a ped in the pool (the player's); its splat stays
 * the original's 1000 ticks. */
void gta_marks_pool(gta_marks *g, long x, long y, int layer);

void gta_marks_tick(gta_marks *g, const gta_nav *nav, gta_peds *peds,
                    const gta_weapons *w);

/* The splash of something going into the water, object 0x36: 12 frames of 2
 * original ticks, standing still (the original's routine). */
void gta_marks_splash(gta_marks *g, long x, long y, int layer);

/* A wall blown by the script's EXPLODE (the original's routine): block (bx,by), face 0
 * west 1 east 2 north 3 south. The three pieces 0x2c 0x2d 0x2b a sixteenth
 * of a block out from the face, and two 0x05 a block out, turned round. */
void gta_marks_debris(gta_marks *g, int bx, int by, int layer, int face);

/* A car landed on something that is not water (the original's routine): a spark at
 * each of its four corners, each turned at random. `len`, `wid` in world
 * pixels, `heading` the 256-step body angle. */
void gta_marks_sparks(gta_marks *g, long x, long y, int layer, int len, int wid,
                      int heading);

/* The black clouds: call after the fireballs. */
void gta_marks_draw_air(const gta_marks *g, gta_view *v);

/* Under everybody: call before anything that stands on the ground. */
void gta_marks_draw(const gta_marks *g, gta_view *v);

#endif
