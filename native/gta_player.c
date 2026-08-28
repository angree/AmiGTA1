/* The player on foot. Read gta_player.h first - it carries the reasoning,
 * including why collision is a ground-type lookup rather than a wall test.
 *
 * Licence: MIT (ours).
 */
#include <string.h>

#include "gta_player.h"
#include "gta_trig.h"

#define FP 16

/* Speeds in 16.16 world pixels per tick, where a block is 32 world pixels and
 * a tick is 1/SIM_HZ of a second (50) - NOT a frame.
 *
 * SET BY THE DEVELOPER WATCHING THE SCREEN, which is the only instrument that
 * works for this. The history is worth keeping because it is two mistakes, and
 * the second was mine:
 *
 *   1. The player felt far too slow, and the cause was NOT the speed: the
 *      default was a WALK. GTA 1 has no walk key at all, the player is always
 *      running, and this port had running on shift. Making the run the default
 *      is what actually fixed it - see gta_player.h.
 *
 *   2. The speeds were ALSO raised at the same time, to 1.9 and 4.1 blocks per
 *      second. That was unnecessary, it multiplied with the first fix, and the
 *      result ran at about twice the right pace. Two changes for one symptom -
 *      exactly what this project's "one change per build" rule exists to
 *      prevent, applied to gameplay instead of to builds. The verdict was
 *      "runs 2x too fast", so the raise is halved back out and nothing else
 *      moved: the animation and turn rates below were judged right as they
 *      were and are untouched.
 *
 * The result lands within 10% of the ORIGINAL pre-fix pace converted to the
 * fixed tick rate (0.70 and 1.80 per frame at 40 fps is 28 and 72 world pixels
 * a second; these are 30 and 65), which is a useful cross-check: the speed was
 * never the problem, only the missing run.
 *
 * If SIM_HZ changes, scale these by the same factor. */
#define WALK_SPEED  (39321L)        /* 0.60 world px/tick = 0.94 blocks/s */
#define RUN_SPEED   (85197L)        /* 1.30 world px/tick = 2.03 blocks/s */

/* Angle units per tick. A turn is 256 units, so 5 at 50 Hz is a full circle in
 * just over a second. Judged right on screen and deliberately NOT changed when
 * the speeds were halved. */
#define TURN_RATE   5

/* Half-width of the player's collision box, 16.16 world pixels. The ped sprite
 * is 14x18 SOURCE pixels, which is 7x9 world pixels, so the body is about
 * three and a half wide; 3 keeps him out of walls without wedging him in
 * doorways. Square rather than the sprite's rectangle on purpose - he turns,
 * and a box that turns with him would make the collision depend on which way
 * he happens to face when he arrives. */
#define BODY_R      (3L << FP)

/* Ticks per animation frame. Judged right on screen - "the animation speed
 * seems ok" - and deliberately left alone when the movement speed was halved.
 *
 * That does mean the stride no longer matches the distance covered, because
 * the frame is driven by TIME and not by how far he has moved. Driving it from
 * distance is the proper fix and it also cures the sliding wherever he is
 * slowed by a ramp or a wall; it is small, and it is on the list. */
#define WALK_TICKS  7
#define RUN_TICKS   4

static int ground_at(const gta_map *m, int bx, int by, int z)
{
    gta_block b;
    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM)
        return GTA_GROUND_AIR;
    if (z < 0 || z >= GTA_MAP_LAYERS)
        return GTA_GROUND_AIR;
    if (!gta_map_block(m, bx, by, z, &b))
        return GTA_GROUND_AIR;
    return gta_block_ground_type(&b);
}

static int walkable_at(const gta_map *m, int bx, int by, int z)
{
    int g = ground_at(m, bx, by, z);
    return g >= GTA_GROUND_ROAD && g <= GTA_GROUND_FIELD;
}

/* WHICH WAY A RAMP GOES UP, or -1 if this block is not a ramp.
 *
 * `type_map` bits 8..13 are the slope: 0 flat, 1..8 a 26-degree ramp, 9..40 a
 * 7-degree one in eight steps, 41..44 a 45-degree one, four directions each.
 * Carnage3D groups them N/S/W/E and this port needs to know whether that
 * letter is the direction of ASCENT - which matters, because getting it
 * backwards sends a walker down where he should go up.
 *
 * The map settles it. `gtadump slopes` finds the real ramps - the ones with a
 * walkable ground type of their own, as opposed to the sloping walls and roofs
 * that make up most of the slope values in the file:
 *
 *   (11,44) slope 33   (12,44) 34   ...   (18,44) 40
 *
 * an unbroken run rising eastward with the value climbing 33 to 40, which is
 * exactly the "E" group in ascending order. The run at x=25..32 does the same
 * thing westward with the "W" group. So the letter IS the ascent and the value
 * within a group is the height. */
static int slope_up_dir(const gta_map *m, int bx, int by, int z)
{
    gta_block b;
    int s;

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM)
        return -1;
    if (z < 0 || z >= GTA_MAP_LAYERS)
        return -1;
    if (!gta_map_block(m, bx, by, z, &b))
        return -1;

    s = gta_block_slope(&b);
    if (s == 0)  return -1;
    if (s <= 2)  return 0;      /* 26 degrees, north */
    if (s <= 4)  return 128;
    if (s <= 6)  return 192;
    if (s <= 8)  return 64;
    if (s <= 16) return 0;      /* 7 degrees, eight steps */
    if (s <= 24) return 128;
    if (s <= 32) return 192;
    if (s <= 40) return 64;
    if (s == 41) return 0;      /* 45 degrees */
    if (s == 42) return 128;
    if (s == 43) return 192;
    if (s == 44) return 64;
    return -1;
}

/* Is this ramp block at its HIGHEST step - the one whose surface reaches the
 * top of its own layer?
 *
 * A ramp climbs over several blocks: the 7-degree group takes eight of them,
 * and only the last stands a full layer high. That distinction is what lets a
 * walker stand at layer z over the top of a ramp whose block is at z-1, while
 * still refusing to let him stand a layer up over the middle of it, where the
 * surface is nowhere near that high. */
static int slope_is_top(const gta_map *m, int bx, int by, int z)
{
    gta_block b;
    int s;

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return 0;
    if (z < 0 || z >= GTA_MAP_LAYERS) return 0;
    if (!gta_map_block(m, bx, by, z, &b)) return 0;

    s = gta_block_slope(&b);
    if (s == 2 || s == 4 || s == 6 || s == 8)       return 1;  /* 26 degrees */
    if (s == 16 || s == 24 || s == 32 || s == 40)   return 1;  /* 7 degrees  */
    if (s >= 41 && s <= 44)                         return 1;  /* 45, one block */
    return 0;
}

/* The compass point a movement is mostly along: 0 N, 64 E, 128 S, 192 W. */
static int step_dir(long dx, long dy)
{
    long ax = dx < 0 ? -dx : dx;
    long ay = dy < 0 ? -dy : dy;
    if (ax >= ay) return dx >= 0 ? 64 : 192;
    return dy >= 0 ? 128 : 0;
}

/* Which layer the player ends up on standing in this column.
 *
 * TWO LEVELS ARE WHY THIS IS NOT JUST "PREFER THE LAYER YOU ARE ON".
 *
 * Almost all of Liberty City is one storey, and for a long time this function
 * simply took the current layer, or one either side of it, which is right for
 * a kerb or a step. It is wrong wherever a road passes over another road: at
 * (19,44) the map has ROAD on layer 2 AND on layer 3, the lower street and the
 * bridge above it. Walking up the ramp at x=11..18 and stepping off the top,
 * the old rule found layer 2 walkable, preferred it because it was the layer
 * he was already on, and put the player UNDER the bridge he had just climbed.
 * That is exactly what was reported: "wchodze pod droge".
 *
 * A RAMP IS WHAT CHANGES LEVEL, and the map says which ramps and which way.
 * Leaving a ramp block in its ascent direction lands a layer higher; entering
 * one from the high side steps down onto it. Everything else keeps the old
 * behaviour, which is why kerbs and steps still work.
 *
 * `fz` is the layer being left and (dx, dy) the movement, because "up" only
 * means anything relative to a direction of travel. */
static int resolve_layer_moving(const gta_map *m, int fx, int fy, int fz,
                                int bx, int by, long dx, long dy)
{
    int dir = step_dir(dx, dy);
    int up;

    /* Standing on a ramp and heading up it: the block ahead is a level higher.
     * Checked FIRST, before the prefer-current-layer rule that used to send
     * him under the bridge. */
    up = slope_up_dir(m, fx, fy, fz);
    if (up >= 0 && up == dir && walkable_at(m, bx, by, fz + 1))
        return fz + 1;

    /* Stepping onto a ramp from its high end: down one, onto the slope. */
    up = slope_up_dir(m, bx, by, fz - 1);
    if (up >= 0 && ((up + 128) & 255) == dir && walkable_at(m, bx, by, fz - 1))
        return fz - 1;

    if (walkable_at(m, bx, by, fz))     return fz;
    if (walkable_at(m, bx, by, fz - 1)) return fz - 1;
    if (walkable_at(m, bx, by, fz + 1)) return fz + 1;
    return -1;
}

/* The old signature, for the places that have no direction to offer - placing
 * the player, and the axis-slide fallbacks where the movement is already known
 * to be blocked on the other axis. */
static int resolve_layer(const gta_map *m, int bx, int by, int want)
{
    if (walkable_at(m, bx, by, want))     return want;
    if (walkable_at(m, bx, by, want - 1)) return want - 1;
    if (walkable_at(m, bx, by, want + 1)) return want + 1;
    return -1;
}

/* Every corner of the body box has to be in a walkable block on the SAME
 * layer. Testing only the centre lets him stand with half of himself inside a
 * building, which is exactly what the first version did. */
static int can_stand_ex(const gta_map *m, long x, long y, int z, int climbing)
{
    int i;
    static const long dx[4] = { -BODY_R,  BODY_R, -BODY_R, BODY_R };
    static const long dy[4] = { -BODY_R, -BODY_R,  BODY_R, BODY_R };

    /* Kept in the signature so the call sites still read as intent, and
     * because the distinction may come back if descending ever needs it. */
    (void)climbing;

    for (i = 0; i < 4; i++) {
        int bx = (int)((x + dx[i]) >> (FP + 5));
        int by = (int)((y + dy[i]) >> (FP + 5));
        if (walkable_at(m, bx, by, z))
            continue;
        /* THE TOP OF A RAMP COUNTS AS THE LAYER ABOVE IT.
         *
         * Stepping east off the top of the ramp at (18,44) onto the bridge at
         * (19,44) layer 3, the trailing corners are still over the ramp block -
         * and THAT block's layer 3 is air, because the ramp is layer 2 and its
         * surface merely rises to the top of it. The plain four-corner test
         * refused the move, the axis fallbacks put him back on layer 2, and he
         * walked under the bridge he had just climbed.
         *
         * The first fix allowed it only during the step that changed layer,
         * and that was not enough: the very next step is not a climb, so he
         * stopped dead a fraction of a block onto the bridge with both axes
         * blocked. The real condition is not "is he climbing" but "is that the
         * TOP of a ramp", which is a fact about the map and true every tick. */
        if (slope_is_top(m, bx, by, z - 1) && walkable_at(m, bx, by, z - 1))
            continue;
        return 0;
    }
    return 1;
}

/* Every corner of the body box has to be in a walkable block on the SAME
 * layer. Testing only the centre lets him stand with half of himself inside a
 * building, which is exactly what the first version did. */
static int can_stand(const gta_map *m, long x, long y, int z)
{
    return can_stand_ex(m, x, y, z, 0);
}

int gta_player_init(gta_player *p, const gta_map *m, const gta_tiles *t,
                    int bx, int by)
{
    int z, found = -1;

    memset(p, 0, sizeof(*p));
    p->x = ((long)bx * 32 + 16) << FP;
    p->y = ((long)by * 32 + 16) << FP;
    /* 128 is south, down the screen. The ped art is drawn facing the bottom of
     * the sheet, so this is the angle at which the sprite is unrotated - the
     * one place a rotation bug cannot hide. */
    p->angle = 128;
    p->anim = GTA_ANIM_STAND;

    p->ped_base  = gta_tiles_sprite_base(t, 7);   /* 7 == ped */
    p->ped_count = gta_tiles_sprite_count(t, 7);

    /* Lowest walkable layer, not the highest: the street is what a player
     * expects to start on, and a roof above it is not somewhere he could have
     * walked to. */
    for (z = 0; z < GTA_MAP_LAYERS; z++) {
        if (walkable_at(m, bx, by, z)) { found = z; break; }
    }
    p->layer = found < 0 ? GTA_GREF_LAYER : found;
    p->ground = ground_at(m, bx, by, p->layer);
    return found >= 0;
}

void gta_player_update(gta_player *p, const gta_map *m,
                       int turn, int forward, int walk)
{
    long speed, dx, dy, nx, ny;
    int z;

    p->blocked_x = p->blocked_y = 0;

    if (turn)
        p->angle = (p->angle + turn * TURN_RATE) & 255;

    if (!forward) {
        p->anim = GTA_ANIM_STAND;
        p->frame = 0;
        p->frame_tick = 0;
        p->ground = ground_at(m, (int)(p->x >> (FP + 5)),
                                 (int)(p->y >> (FP + 5)), p->layer);
        return;
    }

    speed = walk ? WALK_SPEED : RUN_SPEED;
    if (forward < 0)
        speed = -(speed / 2);           /* backing up is slower, as it should be */

    /* (sin * speed) >> 14 would be 1.9 billion at full run - inside a signed
     * 32-bit int, but only just, and a future faster vehicle would push it
     * out. Shifting the speed down first leaves eight bits of headroom and
     * costs a 4096th of a pixel. */
    dx =  ((long)gta_sin(p->angle) * (speed >> 4)) >> 10;
    dy = -((long)gta_cos(p->angle) * (speed >> 4)) >> 10;

    /* Try the whole move, then each axis alone. Sliding along a wall instead
     * of stopping dead at it is most of what makes walking feel right, and it
     * is three tests rather than one. */
    nx = p->x + dx;
    ny = p->y + dy;
    /* The full move knows where it came from and which way it is going, so it
     * is the one that can climb a ramp. The two axis-only fallbacks below are
     * slides along a wall and keep the simpler rule. */
    z = resolve_layer_moving(m, (int)(p->x >> (FP + 5)), (int)(p->y >> (FP + 5)),
                             p->layer,
                             (int)(nx >> (FP + 5)), (int)(ny >> (FP + 5)),
                             dx, dy);
    /* A move that changes layer is a ramp move, and the body straddles the
     * ramp block for one step - see can_stand_ex(). */
    if (z >= 0 && can_stand_ex(m, nx, ny, z, z != p->layer)) {
        p->x = nx; p->y = ny; p->layer = z;
    } else {
        int moved = 0;
        z = resolve_layer(m, (int)(nx >> (FP + 5)),
                             (int)(p->y >> (FP + 5)), p->layer);
        if (z >= 0 && can_stand(m, nx, p->y, z)) {
            p->x = nx; p->layer = z; moved = 1;
        } else {
            p->blocked_x = 1;
        }
        z = resolve_layer(m, (int)(p->x >> (FP + 5)),
                             (int)(ny >> (FP + 5)), p->layer);
        if (z >= 0 && can_stand(m, p->x, ny, z)) {
            p->y = ny; p->layer = z; moved = 1;
        } else {
            p->blocked_y = 1;
        }
        if (!moved) {
            /* Facing a wall head-on. He still animates - a ped who freezes
             * mid-stride against a wall reads as a crash rather than as a
             * collision. */
        }
    }

    p->anim = walk ? GTA_ANIM_WALK : GTA_ANIM_RUN;
    if (++p->frame_tick >= (walk ? WALK_TICKS : RUN_TICKS)) {
        p->frame_tick = 0;
        p->frame++;
        if (p->frame >= GTA_PED_WALK_FRAMES)
            p->frame = 0;
    }

    p->ground = ground_at(m, (int)(p->x >> (FP + 5)),
                             (int)(p->y >> (FP + 5)), p->layer);
}

const unsigned char gta_ped_enter_seq[GTA_PED_ENTER_STEPS] = {
    26, 26, 26, 25, 25, 29, 30, 31, 32, 33
};

int gta_player_sprite(const gta_player *p)
{
    int f;

    switch (p->anim) {
    case GTA_ANIM_WALK: f = GTA_PED_WALK_FIRST + p->frame; break;
    case GTA_ANIM_RUN:  f = GTA_PED_RUN_FIRST  + p->frame; break;
    case GTA_ANIM_ENTER_CAR:
        f = gta_ped_enter_seq[p->frame < GTA_PED_ENTER_STEPS
                              ? p->frame : GTA_PED_ENTER_STEPS - 1];
        break;
    case GTA_ANIM_EXIT_CAR:
        f = GTA_PED_EXITCAR_FIRST
          + (p->frame < GTA_PED_EXITCAR_FRAMES
             ? p->frame : GTA_PED_EXITCAR_FRAMES - 1);
        break;
    default:            f = GTA_PED_STAND;                 break;
    }
    if (f >= p->ped_count)
        f = 0;
    return p->ped_base + f;
}
