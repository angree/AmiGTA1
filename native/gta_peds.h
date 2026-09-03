/* Pedestrians - Phase 5 item 4: the population and its brain.
 *
 * THE RULES ARE THE ORIGINAL'S, THE CROWDING IS NOT.
 *
 * The original's behaviour was worked out rule by rule and this module
 * keeps it: nobody decides anything at a block centre - a
 * ped walks straight along a heading and the heading is nudged by a handful
 * of rules; the gait comes from a global 1..20 counter (wander at speed 1,
 * brisk at 2, a jog at 3, a stop); the heading wobbles by up to eleven
 * degrees with the sign forced to alternate; a corner is seen three quarters
 * of a block ahead and turned by aiming at the pavement to the side; a
 * road is stepped onto only when there is pavement a block and a half
 * beyond it, and a ped ON a road is in the flee mode - running straight
 * away from where it stepped on; a fast car kills outright and leaves the
 * body where it was, a slow one shoves; peds are born on the edge of the
 * view a block out, ahead of the player, eight in ten walking into view,
 * dressed from a 22-entry list, and are let go thirty frames after leaving
 * the screen.
 *
 * And four things are OURS, on the developer's instruction ("pamietaj ze
 * przechodnie w oryginale stackowali sie na rogu skrzyzowan. musimy zrobic
 * wlasna inteligencje przechodniow ktora nie bedzie tego robic"):
 *
 *   1. A corner target is SPREAD across the pavement - a random lateral
 *      and forward offset - where the original aims everybody at one of
 *      four fixed points and they pile up on it.
 *   2. Peds keep a little distance: one within a few pixels ahead of
 *      another slows and sidesteps, and two who overlap are pushed apart.
 *      NOBODY EVER STOPS FOR ANYBODY - an earlier version held them still
 *      for each other and a knot of three then stood in the same doorway
 *      for a minute at a time. The original's ambient peds walk straight
 *      through each other and stack.
 *   3. Nobody stops to stand within sight of a corner; the stop happens
 *      mid-block.
 *   4. A T-junction is a per-ped coin flip, not the original's global
 *      alternation, and the flee mode ENDS when the ped is back on pavement
 *      a block away from where it stepped off - the original's peds run
 *      until they are culled.
 *
 * Units: the original's 64 per block and "speed N per frame" become the
 * port's 32 world px per block at 50 ticks a second; one speed unit is a
 * quarter of the player's calibrated run (gta_player.h), one game frame is
 * a tick and a half for timers.
 *
 * The player's own sprite category (7, the ped sheet) and his 8-frame walk
 * and run cycles are reused frame for frame - one sheet animates the whole
 * city. Portable C89, no floats, no Amiga headers. Licence: MIT (ours).
 */
#ifndef GTA_PEDS_H
#define GTA_PEDS_H

#include "gta_map.h"
#include "gta_nav.h"
#include "gta_tiles.h"
#include "gta_render.h"

#define GTA_MAX_PEDS 12

/* How long somebody pulled out of a car stays down before getting up and
 * walking off - kept for gta_peds_drop(), the generic "thrown out" entry. */
#define GTA_STUN_TICKS 125

/* The brain's modes - the original's `ped+0x66` values that matter here. */
#define GTA_PED_MODE_FLEE  1
#define GTA_PED_MODE_IDLE  2

/* The gait sub-modes - the original's `ped+0x72`. */
#define GTA_PED_SUB_WANDER 2      /* speed 1, the default */
#define GTA_PED_SUB_BRISK  4      /* speed 2 */
#define GTA_PED_SUB_JOG    5      /* speed 3 - the run cycle */
#define GTA_PED_SUB_BLIP   7      /* one frame at 1 with a wobble, re-pick */
#define GTA_PED_SUB_STAND  8      /* speed 0, standing_still */
#define GTA_PED_SUB_VEER_L 9      /* off the pavement's edge: 45 degrees back */
#define GTA_PED_SUB_VEER_R 10

typedef struct {
    long x, y;              /* 16.16 world px */
    int  layer;
    int  angle;             /* 0..255, walking heading */
    int  frame;             /* index within the walk or run cycle */
    int  frame_tick;
    /* WHAT THIS PERSON IS WEARING - a palette remap table. The original's
     * 22-entry list, round robin: three of its entries are below the
     * 128..187 range the port used to assume. */
    int  remap;
    int  alive;

    /* THE BRAIN. */
    int  mode;              /* GTA_PED_MODE_* */
    int  sub;               /* GTA_PED_SUB_* */
    int  speed;             /* 0..4, the original's units per frame */
    int  timer;             /* ticks left in the current gait */
    int  wobble;            /* the last wobble, for the sign alternation */
    long tx, ty;            /* walk target, 0 = none */
    long gx, gy;            /* flee point */
    int  stuck;             /* ticks the step went nowhere */
    int  offscreen;         /* ticks outside the view rect */
    int  flee_aim;          /* the "every 5th frame" re-aim counter */

    /* ON THE GROUND. down > 0: ticks left lying (stunned or pulled out);
     * corpse: dead for good, lies until GTA_CORPSE_TICKS off-screen. */
    int  down;
    int  corpse;

    /* BEING PULLED OUT OF A CAR - the original's states 0x93..0x98
     * (LEFTOFF.md "THE CARJACK VICTIM"). -1 = not; 0..5 = the state, each
     * GTA_PULL_TICKS long, the ped placed against the car every tick from
     * the car's geometry kept here, sprites 80,80,80,46,45,45, then down. */
    int  pull, pull_tick;
    long pull_cx, pull_cy;
    int  pull_face, pull_model;

    /* SHOT - the original's 0x89..0x8c (from the front: faces the shooter,
     * sprites 38,38,38,39, then 0x2d = body 43) or 0x8d..0x90 (from behind:
     * heading = the bullet's, 14,14,14,41, then 0x2c = body 44). Four states
     * of GTA_SHOT_STATE_TICKS, the body carried along the bullet's line for
     * the first three. 0 = not shot, 1 = front, 2 = behind. */
    int  shot, shot_step, shot_tick, shot_dir;
    /* PUNCHED DOWN - state 0xaf: sprite 38 for GTA_FALL_TICKS, moving back a
     * unit a tick, then lying alive (`down`), then up turned 90. */
    int  fall;
    /* PANIC from gunfire: the flee mode with this many ticks before the
     * pavement exit may end it (ours; the original's runs until culled). */
    int  panic;
    /* ON FIRE - the original attaches a fire object to the ped and takes a
     * point of health a tick, so a hundred ticks from full health to a body,
     * and an AI ped runs at full speed the whole time. `burn` is what is
     * left of that; `burn_frame` animates the fire drawn over him. */
    int  burn, burn_frame, burn_tick;
} gta_ped;

/* A hundred of the original's ticks, one health point each. */
#define GTA_BURN_TICKS      100
/* The fire object's own run of frames, three ticks each. */
#define GTA_FIRE_FRAMES       7
#define GTA_FIRE_FRAME_TICKS  3

#define GTA_SHOT_STATES     4
#define GTA_SHOT_STATE_TICKS 4
#define GTA_FALL_TICKS      12       /* three states of 0xaf */
/* Lying after a punch: the original's 0x2b, 20 of its ticks. */
#define GTA_PUNCH_DOWN_TICKS 80
#define GTA_PANIC_TICKS     150

#define GTA_PULL_STATES   6
#define GTA_PULL_TICKS    4      /* 2 of the original's game frames */
/* Lying after the pull: the original's state 0x2b counts 21 of its ticks
 * = 42 game frames, then stands. */
#define GTA_PULL_DOWN_TICKS (21 * GTA_PULL_TICKS)
/* A body stays until it has been off-screen this long - the original's
 * 1000 frames. */
#define GTA_CORPSE_TICKS  1500
/* An ambient ped is let go this long after leaving the view - 30 frames. */
#define GTA_PED_RETIRE_TICKS 45

typedef struct {
    gta_ped p[GTA_MAX_PEDS];
    const gta_tiles *tiles;
    const gta_nav *nav;
    int ped_base, ped_count;
    unsigned long rng;
    /* THE VIEW, in blocks either side of the camera block, and what the
     * player is doing - the spawner puts people on the edge ahead of him. */
    int view_hw, view_hh;
    int player_angle, player_moving;
    /* The original's global counters, kept global here too: the gait wheel
     * 1..20, the spawn edge for a standing player, the 8-in-10 heading
     * counter, the remap round robin, the standing fidget flag. */
    int gait_wheel;
    int spawn_edge;
    int spawn_dir;
    int remap_next;
    int fidget;
    int spawned_since_retire;
    /* The fire object's first sprite, resolved once from the tile set: a
     * burning man is drawn with his own frame and this on top. */
    int fire_sprite;
    /* The original's global 0..12 punch counter: 10 of 13 punches land. */
    int punch_wheel;
    long stat_spawned, stat_runover, stat_killed, stat_shot, stat_punched;
} gta_peds;

void gta_peds_init(gta_peds *ps, const gta_tiles *t, unsigned long seed);
void gta_peds_set_nav(gta_peds *ps, const gta_nav *nav);

/* Every tick before gta_peds_tick(): the view's half-extents in blocks and
 * the player's heading and whether he is moving (for the spawn edge). */
void gta_peds_set_view(gta_peds *ps, int half_w_blocks, int half_h_blocks,
                       int player_angle, int player_moving);

/* One 50 Hz tick: the brain for everybody, the pull-outs, the bodies, the
 * spawner. */
void gta_peds_tick(gta_peds *ps, const gta_map *m, long cam_x, long cam_y);

/* The player's car against the crowd. `speed` is the car's, in the physics'
 * 16.16 px per step: at or above five units (gta_vehphys.h) a ped inside the
 * box is killed where it stands, below that it is shoved along its own
 * heading. Returns how many were hit this tick. */
int gta_peds_ram(gta_peds *ps, long px, long py, int pface, int phl, int phw,
                 int layer, long speed);

/* THROWN OUT ONTO THE ROAD at (x,y) facing `angle`, stunned for `stun`
 * ticks, then up and walking like anybody else. `remap` -1 = pick one.
 * Returns 0 if the pool is full. */
int gta_peds_drop(gta_peds *ps, long x, long y, int layer, int angle,
                  int remap, int stun);

/* The driver of the car at (cx,cy) facing `face`, model `model`, is dragged
 * out of the driver's door: created in the seat and walked through the
 * original's six pull states, then left lying beside the car for
 * GTA_PULL_DOWN_TICKS, then up and walking like anyone else. Returns 0 when
 * the pool is full. */
int gta_peds_pull(gta_peds *ps, long cx, long cy, int face, int model,
                  int layer, int remap);

void gta_peds_draw(gta_peds *ps, gta_view *v);

/* ---- GUNS AND FISTS (Phase 5 item 5) -------------------------------- */

/* The first ped on `layer` inside a box from (x,y) `len_px` along `heading`
 * and `hw_px` either side (plus his own body), nearest first; `skip` is a
 * ped index not to count (the shooter), -1 for none. Returns the index or
 * -1. Bodies and people being dragged out of cars do not count. */
int gta_peds_hit_segment(const gta_peds *ps, long x, long y, int heading,
                         int len_px, int hw_px, int layer, int skip);

/* Ped `i` is hit by a bullet travelling along `heading`: the original's
 * front/behind split on his own heading, the fall, then a body. */
void gta_peds_shoot(gta_peds *ps, int i, int heading);

/* A punch thrown from (x,y) along `angle`: the original's 6x6-unit box 8
 * units ahead and its 10-in-13 counter. Returns the victim's index, -1 for
 * nobody there, -2 for a miss on the counter. */
int gta_peds_punch(gta_peds *ps, long x, long y, int angle, int layer);

/* Gunfire at (x,y): every walking or standing ped within a block runs from
 * it, as the original's gunfire panic does. */
void gta_peds_panic(gta_peds *ps, long x, long y, int layer);

/* Ped `i` catches fire, from a flame or an explosion at (fx,fy): he runs -
 * away from it - and burns to death in GTA_BURN_TICKS. Does nothing to
 * somebody already alight, dead or being dragged out of a car. */
void gta_peds_burn(gta_peds *ps, int i, long fx, long fy);

/* Ped `i` is killed where he stands - the explosion's inner ring, which the
 * original kills outright rather than knocking down. */
void gta_peds_kill(gta_peds *ps, int i);

#endif /* GTA_PEDS_H */
