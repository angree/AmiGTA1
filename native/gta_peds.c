/* Pedestrians - see gta_peds.h for what is the original's and what is ours.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include "gta_peds.h"
#include "gta_style.h"
#include "gta_trig.h"
#include "gta_player.h"     /* GTA_SPRITE_ART_SOUTH, the frame numbers, RUN */

/* ONE SPEED UNIT. The original moves a ped `speed` of its units (a 64th of
 * a block) per game frame and its run is speed 4; the port's run was
 * calibrated against the PC at 2.03 blocks a second (gta_player.c). So a
 * unit is a quarter of that, 16.16 px a tick. */
#define PED_UNIT        (GTA_RUN_SPEED_FP / 4)
/* A game frame in ticks, for the original's timers: 3/2. */
#define FRAMES(n)       (((n) * 3) / 2)
#define PED_WALK_TICKS  7           /* the cycles' cadence, the player's own */
#define PED_RUN_TICKS   4
#define PED_DOWN_TICKS  250         /* stunned by a shove: five seconds */

/* The original's dress list, 22 entries, round robin. */
static const unsigned char ped_remaps[22] = {
    39, 40, 41, 132, 133, 134, 136, 137, 138, 139, 140,
    177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187
};

static unsigned long rng_next(gta_peds *ps)
{
    ps->rng = ps->rng * 1103515245UL + 12345UL;
    return (ps->rng >> 16) & 0x7FFF;
}

static int ground_at(const gta_peds *ps, long x, long y, int z)
{
    int bx = (int)(x >> 21), by = (int)(y >> 21);
    if (bx < 0 || by < 0 || bx >= 256 || by >= 256)
        return GTA_GROUND_BUILDING;
    return gta_nav_ground(gta_nav_at_m(ps->nav, bx, by, z));
}

static int pavement_at(const gta_peds *ps, long x, long y, int z)
{
    return ground_at(ps, x, y, z) == GTA_GROUND_PAVEMENT;
}

/* The point `px` world pixels along heading `a` from (x,y). */
static void ahead(long x, long y, int a, long px, long *ox, long *oy)
{
    *ox = x + (long)gta_sin(a) * px * 4;
    *oy = y - (long)gta_cos(a) * px * 4;
}

static int snap_cardinal(int a)
{
    return ((a + 32) & 255) & ~63;
}

static int angle_to(long fx, long fy, long tx, long ty)
{
    return (int)(gta_dir16(tx - fx, ty - fy) >> 16) & 255;
}

void gta_peds_init(gta_peds *ps, const gta_tiles *t, unsigned long seed)
{
    int i;

    for (i = 0; i < GTA_MAX_PEDS; i++)
        ps->p[i].alive = 0;
    ps->tiles = t;
    ps->nav = 0;
    ps->ped_base = gta_tiles_sprite_base(t, 7);
    ps->ped_count = gta_tiles_sprite_count(t, 7);
    ps->rng = seed ? seed : 1UL;
    ps->view_hw = 6; ps->view_hh = 4;
    ps->player_angle = 0; ps->player_moving = 0;
    ps->gait_wheel = 0;
    ps->spawn_edge = 0;
    ps->spawn_dir = 0;
    ps->remap_next = 0;
    ps->fidget = 0;
    ps->spawned_since_retire = 0;
    ps->punch_wheel = 0;
    ps->fire_sprite = gta_tiles_object_sprite(t, 0x2e);
    ps->stat_spawned = ps->stat_runover = ps->stat_killed = 0;
    ps->stat_shot = ps->stat_punched = 0;
}

void gta_peds_set_nav(gta_peds *ps, const gta_nav *nav)
{
    ps->nav = nav;
}

void gta_peds_set_view(gta_peds *ps, int half_w_blocks, int half_h_blocks,
                       int player_angle, int player_moving)
{
    ps->view_hw = half_w_blocks;
    ps->view_hh = half_h_blocks;
    ps->player_angle = player_angle & 255;
    ps->player_moving = player_moving;
}

/* A fresh ped in slot i: the original's reset plus what the
 * spawner sets - walking, wander, speed 1, timer 0 so the wheel is read at
 * once. */
/* CAN HE STEP THAT WAY? Only the hard refusals - a building, thin air,
 * water. The pavement etiquette (a road wants pavement beyond it, a field is
 * not walked on) is the CROWD's and is not applied to somebody the script is
 * steering; see the head of this change. */
static int step_free(gta_peds *ps, gta_ped *p, int ang, int dist);

static void ped_reset(gta_peds *ps, gta_ped *p, long x, long y, int layer,
                      int angle, int remap)
{
    p->x = x; p->y = y; p->layer = layer;
    p->angle = angle & 255;
    p->frame = (int)(rng_next(ps) & 7);
    p->frame_tick = 0;
    p->remap = remap >= 0 ? remap
             : ped_remaps[(ps->remap_next++) % 22];
    p->alive = 1;
    /* AND NOT THE LAST OCCUPANT'S MARKS. A slot is reused, and one that had
     * held the script's own person would come back as a pedestrian the
     * recycler would not touch and the script still believed in. */
    p->mission = 0;
    p->hunt_pl = 0;
    p->serial = 0;
    p->mode = GTA_PED_MODE_IDLE;
    p->sub = GTA_PED_SUB_WANDER;
    p->speed = 1;
    p->timer = 0;
    p->wobble = 0;
    p->tx = p->ty = 0;
    p->gx = p->gy = 0;
    p->stuck = 0;
    p->dodge = 0; p->dodge_side = 1;
    p->offscreen = 0;
    p->flee_aim = 0;
    p->down = 0;
    p->corpse = 0;
    p->pull = -1;
    p->pull_tick = 0;
    p->shot = p->shot_step = p->shot_tick = p->shot_dir = 0;
    p->fall = 0;
    p->panic = 0;
    p->burn = p->burn_frame = p->burn_tick = 0;
    p->cop = 0;
    p->arrest = 0;
    p->cop_cool = 0;
    p->shoot_req = 0;
    p->shoot_angle = 0;
    p->execute = 0;
    p->post = 0;
    p->cross_axis_x = 0;
}

static int step_free(gta_peds *ps, gta_ped *p, int ang, int dist)
{
    long nx, ny;
    int g;
    ahead(p->x, p->y, ang, dist, &nx, &ny);
    g = ground_at(ps, nx, ny, p->layer);
    return g != GTA_GROUND_BUILDING && g != GTA_GROUND_AIR
        && g != GTA_GROUND_WATER;
}

/* The traffic hint of the block under (x,y) on layer z - 1 is a light.
 * The nav byte has no room for it (bit 7 is the slope), so this goes to
 * the map through the pointer the grid keeps. Asked rarely: only for a
 * ped standing idle, one tick in eleven. */
static int hint_at(const gta_peds *ps, long x, long y, int z)
{
    gta_block b;
    int bx = (int)(x >> 21), by = (int)(y >> 21);
    if (!ps->nav || !ps->nav->map) return 0;
    if (!gta_map_block(ps->nav->map, bx, by, z, &b)) return 0;
    return gta_block_traffic_hint(&b);
}

void gta_peds_set_lights(gta_peds *ps, int (*fn)(void *, int, int, int), void *ctx)
{
    ps->light_green = fn;
    ps->light_ctx = ctx;
}

static int free_slot(gta_peds *ps)
{
    int i, oldest = -1, oldest_off = -1;
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (!ps->p[i].alive)
            return i;
    /* AND NEVER THE SCRIPT'S OWN PERSON, whatever else is short. */
    /* THE POOL IS TWELVE, NOT TWO HUNDRED: a street of bodies would starve
     * the spawner for good, so a corpse gives up its slot to a newcomer -
     * the one that has been out of sight longest. */
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (ps->p[i].corpse && !ps->p[i].mission &&
            ps->p[i].offscreen > oldest_off) {
            oldest = i; oldest_off = ps->p[i].offscreen;
        }
    return oldest;
}

/* A SLOT, EVEN IF SOMEBODY HAS TO GIVE ONE UP.
 *
 * free_slot() recycles corpses and nothing else, which is right for the
 * spawner - a street full of living people should not thin itself out. It is
 * wrong for the carjack: the driver you have just dragged out of a car is the
 * one person on screen the player is definitely looking at, and losing him to
 * "ped pool full" makes the whole manoeuvre look like it did not happen -
 * "w aucie wsiadamy i jedziemy, brakuje tego jakby nie bylo kierowcy".
 *
 * So this falls back to the living pedestrian who has been out of sight the
 * longest. Somebody the player cannot see leaves the world; somebody he is
 * watching arrives in it. */
static int free_slot_forced(gta_peds *ps)
{
    int i, worst = -1, worst_off = -1;
    int slot = free_slot(ps);
    if (slot >= 0)
        return slot;
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (ps->p[i].alive && !ps->p[i].mission &&
            ps->p[i].offscreen > worst_off) {
            worst = i; worst_off = ps->p[i].offscreen;
        }
    return worst;
}

/* THE SPAWNER, the original's. One attempt: a point on one
 * edge of the view rect grown by a block, the edge ahead of the player,
 * uniformly along it, a little way into its block; pavement only; nobody
 * already there; heading into the view eight times in ten. Returns 1 when
 * somebody was born. */
static int spawn_one(gta_peds *ps, int cbx, int cby)
{
    int i = free_slot(ps);
    int x0 = cbx - ps->view_hw - 1, x1 = cbx + ps->view_hw + 1;
    int y0 = cby - ps->view_hh - 1, y1 = cby + ps->view_hh + 1;
    int edge, bx, by, heading;
    long x, y;

    if (i < 0)
        return 0;

    /* WHICH EDGE: the one ahead of the player, or all four in turn when
     * he stands still. The port's 0 is north, so within 32 of 0 is the top. */
    if (ps->player_moving) {
        int a = ps->player_angle;
        if (a < 32 || a >= 224)      edge = 3;     /* top    */
        else if (a < 96)             edge = 1;     /* right  */
        else if (a < 160)            edge = 2;     /* bottom */
        else                         edge = 0;     /* left   */
    } else {
        edge = ps->spawn_edge;
        ps->spawn_edge = (ps->spawn_edge + 1) & 3;
    }
    /* Into the view 8 times in 10 (the original's 0..9 counter). */
    ps->spawn_dir = (ps->spawn_dir + 1) % 10;
    switch (edge) {
    case 0:  bx = x0; by = y0 + (int)(rng_next(ps) % (unsigned long)(y1 - y0 + 1));
             heading = ps->spawn_dir < 8 ? 64 : 192; break;
    case 1:  bx = x1; by = y0 + (int)(rng_next(ps) % (unsigned long)(y1 - y0 + 1));
             heading = ps->spawn_dir < 8 ? 192 : 64; break;
    case 2:  by = y1; bx = x0 + (int)(rng_next(ps) % (unsigned long)(x1 - x0 + 1));
             heading = ps->spawn_dir < 8 ? 0 : 128; break;
    default: by = y0; bx = x0 + (int)(rng_next(ps) % (unsigned long)(x1 - x0 + 1));
             heading = ps->spawn_dir < 8 ? 128 : 0; break;
    }
    if (bx < 1 || by < 1 || bx > 254 || by > 254)
        return 0;
    /* Our spread: anywhere in the middle of the block, not the original's
     * 16..48 counter that lined people up. */
    x = ((long)bx * 32 + 4 + (long)(rng_next(ps) % 24)) << 16;
    y = ((long)by * 32 + 4 + (long)(rng_next(ps) % 24)) << 16;
    if (!pavement_at(ps, x, y, 2))
        return 0;
    {
        /* nothing already standing there */
        int j;
        for (j = 0; j < GTA_MAX_PEDS; j++) {
            long dx, dy;
            if (!ps->p[j].alive || j == i) continue;
            dx = (ps->p[j].x - x) >> 16; dy = (ps->p[j].y - y) >> 16;
            if (dx > -4 && dx < 4 && dy > -4 && dy < 4)
                return 0;
        }
    }
    ped_reset(ps, &ps->p[i], x, y, 2, heading, -1);
    ps->stat_spawned++;
    ps->spawned_since_retire = 1;
    return 1;
}

/* ---- the pull-out ------------------------------------------------------ */

static const signed char pull_along[GTA_PULL_STATES] = { -1, -1, -2, -3, -4, -4 };
static const signed char pull_lat[GTA_PULL_STATES]   = { -3,  0,  2,  2,  2,  2 };

/* THE SAME FLANK AS THE JACKER: -1 for every car, the side the door art
 * opens on. This used to read the door record's rpy sign as a side, exactly
 * as gta_main.c's car_door_side() did, and that sign is not a side - it is a
 * hinge offset a few pixels either way of the centre line (model 0 is -6,
 * model 1 is +7). On those cars the victim was pulled out of the passenger
 * door while the player got in at the driver's, on opposite flanks. */
static int door_side(const gta_car_info *ci)
{
    return ci->n_doors > 0 ? -1 : 1;
}

static void pull_place(const gta_peds *ps, gta_ped *p)
{
    const gta_car_info *ci = &ps->tiles->cars[p->pull_model];
    int  s = p->pull < GTA_PULL_STATES ? p->pull : GTA_PULL_STATES - 1;
    long fx = gta_sin(p->pull_face), fy = -gta_cos(p->pull_face);
    long rx = gta_cos(p->pull_face), ry = gta_sin(p->pull_face);
    long along  = (ci->n_doors > 0 ? (long)ci->doors[0].rpx / 2 : 0)
                + pull_along[s];
    long across = door_side(ci)
                * ((long)gta_car_world_wid(ci) / 2 + pull_lat[s]);
    p->x = p->pull_cx + (fx * along + rx * across) * 4;
    p->y = p->pull_cy + (fy * along + ry * across) * 4;
}

static int gta_peds_pull_i(gta_peds *ps, long cx, long cy, int face, int model,
                           int layer, int remap);

/* HOW FAR A COP IS FROM THE PLAYER, MEASURED TO HIS CAR'S BODY.
 *
 * On foot it is the plain distance. In a car the player sits at the middle
 * of a body 62 px long, and the arrest used to be contact within 22 px of
 * that middle - i.e. a cop had to be THIRTY PIXELS INSIDE THE BONNET before
 * it fired, where the car sprite hides him completely. The offset is
 * projected onto the car's own axes and each component reduced by the half
 * extent, so this is the distance to the nearest point of the bodywork, and
 * `inside` says he is under the car rather than beside it - a cop who
 * appeared there may not arrest at all; he walks out first. */
static long dist_to_player_body(const gta_peds *ps, const gta_ped *p,
                                int *inside)
{
    long dx = (ps->pl_x - p->x) >> 16, dy = (ps->pl_y - p->y) >> 16;
    long fx, fy, rx, ry, along, side;

    if (inside) *inside = 0;
    if (!ps->pl_in_car) {
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        return dx > dy ? dx : dy;
    }
    fx = gta_sin(ps->pl_face); fy = -gta_cos(ps->pl_face);
    rx = gta_cos(ps->pl_face); ry = gta_sin(ps->pl_face);
    along = (dx * (fx >> 6) + dy * (fy >> 6)) >> 8;
    side  = (dx * (rx >> 6) + dy * (ry >> 6)) >> 8;
    if (along < 0) along = -along;
    if (side  < 0) side  = -side;
    if (inside && along < ps->pl_hl && side < ps->pl_hw)
        *inside = 1;
    along -= ps->pl_hl;
    side  -= ps->pl_hw;
    if (along < 0) along = 0;
    if (side  < 0) side  = 0;
    return along > side ? along : side;
}

int gta_peds_pull(gta_peds *ps, long cx, long cy, int face, int model,
                  int layer, int remap)
{
    int r = gta_peds_pull_i(ps, cx, cy, face, model, layer, remap);
    ps->last_index = r;
    return r >= 0;
}

static int gta_peds_pull_i(gta_peds *ps, long cx, long cy, int face, int model,
                           int layer, int remap)
{
    const gta_car_info *ci = &ps->tiles->cars[model];
    /* FORCED: the man coming out of the car takes precedence over a walker
     * the player cannot see. See free_slot_forced(). */
    int i = free_slot_forced(ps);
    gta_ped *p;

    if (i < 0)
        return -1;
    p = &ps->p[i];
    ped_reset(ps, p, cx, cy, layer, (face + door_side(ci) * 64) & 255, remap);
    p->speed = 0;
    p->pull = 0;
    p->pull_tick = 0;
    p->pull_cx = cx;
    p->pull_cy = cy;
    p->pull_face = face & 255;
    p->pull_model = model;
    pull_place(ps, p);
    ps->stat_spawned++;
    return i;
}

int gta_peds_drop(gta_peds *ps, long x, long y, int layer, int angle,
                  int remap, int stun)
{
    int i = free_slot(ps);
    if (i < 0)
        return 0;
    ped_reset(ps, &ps->p[i], x, y, layer, angle, remap);
    ps->p[i].speed = 0;
    ps->p[i].down = stun;
    ps->stat_spawned++;
    return 1;
}

/* ---- the brain --------------------------------------------------------- */

/* THE GAIT WHEEL - the original's global 1..20, stepped on every
 * ped's call, read when a ped's timer runs out. Ours: no stopping within
 * sight of a corner (a ped whose block ahead is not pavement re-rolls). */
static void pick_gait(gta_peds *ps, gta_ped *p, int corner_ahead)
{
    int w = ps->gait_wheel;
    if (w <= 8) {
        p->sub = GTA_PED_SUB_WANDER; p->speed = 1;
        p->timer = FRAMES((int)(rng_next(ps) % 400));
    } else if (w <= 14) {
        p->sub = GTA_PED_SUB_BRISK; p->speed = 2;
        p->timer = FRAMES((int)(rng_next(ps) % 200));
    } else if (w == 15) {
        p->sub = GTA_PED_SUB_JOG; p->speed = 3;
        p->timer = FRAMES((int)(rng_next(ps) % 200));
    } else if (w <= 19) {
        if (p->sub == GTA_PED_SUB_BLIP) {
            p->sub = GTA_PED_SUB_WANDER; p->speed = 1;
            p->timer = FRAMES((int)(rng_next(ps) % 400));
        } else {
            p->sub = GTA_PED_SUB_BLIP; p->speed = 1;
            p->timer = FRAMES((int)(rng_next(ps) % 0x40));
        }
    } else {
        if (corner_ahead) {
            /* ours: not here - a short wander instead */
            p->sub = GTA_PED_SUB_WANDER; p->speed = 1;
            p->timer = FRAMES(60);
        } else {
            p->sub = GTA_PED_SUB_STAND; p->speed = 0;
            p->timer = FRAMES((int)(rng_next(ps) % 200));
        }
    }
}

/* THE WOBBLE: up to eleven degrees (the original's +/-0x20 of 0x400 is +/-8
 * of 256), sign forced to alternate with the last one. */
static void wobble(gta_peds *ps, gta_ped *p)
{
    int w = (int)(rng_next(ps) % 16) - 8;
    if (p->wobble < 0 && w < 0) w = -w;
    if (p->wobble > 0 && w > 0) w = -w;
    p->wobble = w;
    p->angle = (p->angle + w) & 255;
}

/* THE CORNER - the original's three-quarter-block look-ahead, with our
 * spread target. Returns 1 when a target was set. */
static int corner(gta_peds *ps, gta_ped *p)
{
    long lx, ly, rx, ry;
    int a = snap_cardinal(p->angle);
    int left_ok, right_ok, side;
    long sx, sy;

    ahead(p->x, p->y, (a - 64) & 255, 32, &lx, &ly);
    ahead(p->x, p->y, (a + 64) & 255, 32, &rx, &ry);
    left_ok  = pavement_at(ps, lx, ly, p->layer);
    right_ok = pavement_at(ps, rx, ry, p->layer);
    if (!left_ok && !right_ok)
        return 0;
    if (left_ok && right_ok)
        side = (rng_next(ps) & 1) ? -1 : 1;     /* ours: a coin, per ped */
    else
        side = left_ok ? -1 : 1;
    /* The target: the side block's centre line, spread across the
     * pavement (+/-10 px lateral) and a little way along it (0..20 px),
     * where the original cycles four fixed offsets and everybody piles
     * onto them. */
    ahead(p->x, p->y, (a + side * 64) & 255, 32, &sx, &sy);
    {
        int fwd = (int)(rng_next(ps) % 21);
        int lat = (int)(rng_next(ps) % 21) - 10;
        ahead(sx, sy, a, fwd, &sx, &sy);
        ahead(sx, sy, (a + 64) & 255, lat, &sx, &sy);
    }
    p->tx = sx; p->ty = sy;
    p->angle = a;
    return 1;
}

/* OUR SEPARATION: somebody within 5 px ahead - slow to a shuffle and
 * sidestep away; within 3 px - push apart. */
/* THE CROWD RULE, and the one thing it must never do: STOP ANYBODY.
 *
 * It used to. A ped with somebody three pixels ahead held still for a tick,
 * and when two of them faced each other they held for each other's sake for
 * ever - a knot of three stood in the same doorway for a whole minute of
 * play (the developer's screenshot, and `gta: peds ... pairs within 4px`
 * counted it). The men who then walked into them stopped too, so the knot
 * grew. That is precisely the pile-up this deviation from the original
 * exists to avoid, arrived at from the other direction.
 *
 * So nobody waits for anybody: somebody close ahead only slows you to a
 * shuffle and turns you a little. Keeping bodies out of each other is a
 * separate job and it is done after everybody has moved, by relax() below.
 * Returns non-zero when the way ahead is crowded. */
static int separation(gta_peds *ps, gta_ped *p)
{
    int i, crowded = 0;
    int self = (int)(p - ps->p);
    long fx = gta_sin(p->angle), fy = -gta_cos(p->angle);

    for (i = 0; i < GTA_MAX_PEDS; i++) {
        const gta_ped *o = &ps->p[i];
        long dx, dy, along, side;
        if (i == self || !o->alive || o->corpse || o->pull >= 0
            || o->layer != p->layer)
            continue;
        dx = (o->x - p->x) >> 16; dy = (o->y - p->y) >> 16;
        if (dx > 6 || dx < -6 || dy > 6 || dy < -6)
            continue;

        /* AHEAD: slow down and lean away from him. */
        along = (dx * fx + dy * fy) >> 14;
        side  = (dx * fy - dy * fx) >> 14;      /* + = to the right */
        if (along < 0 || along > 5)
            continue;
        crowded = 1;
        p->angle = (p->angle + (side >= 0 ? -6 : 6)) & 255;
    }
    return crowded;
}

/* NOBODY STANDS INSIDE ANYBODY. One pass over every pair once the walking is
 * done: an overlap moves both of them apart, a deep one harder than a
 * shallow one, and two men on exactly the same pixel are split by their slot
 * number because no direction is "away" from a man you are standing in.
 *
 * A push is refused rather than shoving somebody through a wall - and then
 * tried one axis at a time, which is what lets a pair squeezed against a
 * building slide apart ALONG it instead of staying merged.
 *
 * Twelve peds is 66 pairs, and only the ones within six pixels do any work.
 */
#define PED_APART_PX  5         /* nearer than this and they are pushed */
#define PED_DODGE_TICKS 24      /* a sidestep lasts this long - see below */
#define PED_LEAVE_TICKS 100     /* still in the same block after this long, crowded: walk away */
#define PED_DODGE_TURN  64      /* and it is SIDEWAYS: at 45 degrees the step still had a
                                 * half towards the other man, and relax() pushing him
                                 * back out of it was stronger than a walk at speed 1 */

static int ped_walkable(const gta_peds *ps, long x, long y, int z)
{
    int g = ground_at(ps, x, y, z);
    return g >= GTA_GROUND_ROAD && g <= GTA_GROUND_FIELD;
}

static void ped_shove(gta_peds *ps, gta_ped *p, long px, long py)
{
    if (ped_walkable(ps, p->x + px, p->y + py, p->layer)) {
        p->x += px; p->y += py;
    } else if (ped_walkable(ps, p->x + px, p->y, p->layer)) {
        p->x += px;
    } else if (ped_walkable(ps, p->x, p->y + py, p->layer)) {
        p->y += py;
    }
}

static void relax(gta_peds *ps)
{
    int a, b;

    for (a = 0; a < GTA_MAX_PEDS; a++) {
        gta_ped *pa = &ps->p[a];
        if (!pa->alive || pa->corpse || pa->pull >= 0)
            continue;
        for (b = a + 1; b < GTA_MAX_PEDS; b++) {
            gta_ped *pb = &ps->p[b];
            long dx, dy, d2, step, px, py;
            if (!pb->alive || pb->corpse || pb->pull >= 0
                || pb->layer != pa->layer)
                continue;
            dx = (pb->x - pa->x) >> 16;
            dy = (pb->y - pa->y) >> 16;
            if (dx > PED_APART_PX || dx < -PED_APART_PX
                || dy > PED_APART_PX || dy < -PED_APART_PX)
                continue;
            d2 = dx * dx + dy * dy;
            if (d2 >= (long)PED_APART_PX * PED_APART_PX)
                continue;
            /* Deep overlaps get a whole pixel each, shallow ones a half:
             * enough to beat a running man's stride either way. */
            step = (d2 <= 4) ? (1L << 16) : (1L << 15);
            if (dx == 0 && dy == 0) {
                /* standing in each other: the slot number decides */
                px = step; py = 0;
            } else {
                /* ALONG THE LINE BETWEEN THEM, IN PROPORTION. This used to
                 * push by the SIGN of each axis - a full step sideways for
                 * a man one pixel off the line - and that sideways push is
                 * exactly what cancelled a sidestep: two men head-on, five
                 * pixels apart along x and one along y, were pushed a step
                 * apart in y every tick, against the dodge, and stood in
                 * one block for 8600 ticks (the stuck logger's first catch,
                 * `gtadump peds ... 62 60 9000 777`). */
                long adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
                long big = adx > ady ? adx : ady;
                px = step * dx / big;
                py = step * dy / big;
            }
            /* pb away from pa, pa away from pb */
            ped_shove(ps, pb,  px,  py);
            ped_shove(ps, pa, -px, -py);
        }
    }
}

/* THE STUCK LOGGER - see gta_ped.blk_x in the header. Called once a tick per
 * live ped, after his step. "Wants to move" is speed > 0: a man standing for
 * a rest (SUB_STAND, speed 0) is not stuck, and his ticks do not count. */
#define PED_STUCK_TICKS  300    /* six seconds in one block, wanting out: a wanderer at
                                 * speed 1 needs a hundred ticks a block walking straight */
#define PED_STUCK_REPEAT  50    /* then a line a second */

static void stuck_log(gta_peds *ps, gta_ped *p, int i)
{
    int bx = (int)(p->x >> 21), by = (int)(p->y >> 21);
    int k, near_ = 0;
    long ax, ay, lx, ly, rx, ry;
    char hist[16 * 4 + 1];
    int hp = 0;

    if (!p->alive || p->corpse || p->pull >= 0 || p->down || p->fall) {
        p->blk_ticks = 0;
        p->blk_logged = 0;
        p->blk_x = bx; p->blk_y = by;
        return;
    }
    if (bx != p->blk_x || by != p->blk_y) {
        if (p->blk_logged) {
            printf("gta: pedstuck %d FREED after %d ticks in (%d,%d), now "
                   "(%d,%d) mode %d sub %d angle %d dodge %d\n",
                   i, p->blk_ticks, p->blk_x, p->blk_y, bx, by,
                   p->mode, p->sub, p->angle, p->dodge);
            ps->stat_stuck_freed++;
        }
        p->blk_x = bx; p->blk_y = by;
        p->blk_ticks = 0;
        p->blk_logged = 0;
        return;
    }
    if (p->speed <= 0)
        return;
    p->blk_ticks++;
    if ((p->blk_ticks & 3) == 0) {
        p->ang_hist[p->ang_hist_i & 15] = (unsigned char)p->angle;
        p->ang_hist_i++;
    }
    if (p->blk_ticks < PED_STUCK_TICKS ||
        ((p->blk_ticks - PED_STUCK_TICKS) % PED_STUCK_REPEAT) != 0)
        return;

    for (k = 0; k < GTA_MAX_PEDS; k++) {
        const gta_ped *o = &ps->p[k];
        long dx, dy;
        if (k == i || !o->alive || o->corpse || o->layer != p->layer)
            continue;
        dx = (o->x - p->x) >> 16; dy = (o->y - p->y) >> 16;
        if (dx > -8 && dx < 8 && dy > -8 && dy < 8)
            near_++;
    }
    ahead(p->x, p->y, p->angle, 8, &ax, &ay);
    ahead(p->x, p->y, (p->angle - 32) & 255, 8, &lx, &ly);
    ahead(p->x, p->y, (p->angle + 32) & 255, 8, &rx, &ry);
    /* by hand: this file is C89 and snprintf is not (the host build warns);
     * three digits and a space at most per angle, 16 * 4 + 1 bytes */
    for (k = 0; k < 16; k++) {
        int v = (int)p->ang_hist[(p->ang_hist_i + k) & 15];
        if (v >= 100) hist[hp++] = (char)('0' + v / 100);
        if (v >= 10)  hist[hp++] = (char)('0' + (v / 10) % 10);
        hist[hp++] = (char)('0' + v % 10);
        hist[hp++] = ' ';
    }
    hist[hp] = 0;

    printf("gta: pedstuck %d at (%ld,%ld) block (%d,%d) layer %d for %d ticks | "
           "mode %d sub %d speed %d angle %d timer %d | target (%ld,%ld) flee "
           "(%ld,%ld) | stuck %d dodge %d side %d wobble %d cop %d | near %d | "
           "ground here %d ahead %d left %d right %d | angles %s\n",
           i, p->x >> 16, p->y >> 16, bx, by, p->layer, p->blk_ticks,
           p->mode, p->sub, p->speed, p->angle, p->timer,
           p->tx >> 16, p->ty >> 16, p->gx >> 16, p->gy >> 16,
           p->stuck, p->dodge, p->dodge_side, p->wobble, p->cop, near_,
           ground_at(ps, p->x, p->y, p->layer),
           ground_at(ps, ax, ay, p->layer),
           ground_at(ps, lx, ly, p->layer),
           ground_at(ps, rx, ry, p->layer), hist);
    if (!p->blk_logged) {
        p->blk_logged = 1;
        ps->stuck_report = 1;
        ps->stat_stuck_reports++;
    }
}

void gta_peds_tick(gta_peds *ps, const gta_map *m, long cam_x, long cam_y)
{
    int cbx = (int)(cam_x >> 21), cby = (int)(cam_y >> 21);
    int i, alive = 0;

    (void)m;
    if (!ps->nav)
        return;

    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        int bx, by, in_view;

        if (!p->alive)
            continue;
        alive++;

        /* THE VIEW RECT and the two clocks that let people go: 30 frames
         * out of it for the living, 1000 for a body. The original's
         * rule. Plus a hard backstop, far beyond any view. */
        /* The rect that keeps them is the view grown by TWO blocks, one
         * more than the spawn edge: a newborn at wander pace needs a
         * hundred ticks to walk a block, and the original's rect (not
         * grown) with its 30-frame clock would have thrown most of a
         * twelve-strong pool away unborn. */
        bx = (int)(p->x >> 21); by = (int)(p->y >> 21);
        in_view = bx >= cbx - ps->view_hw - 2 && bx <= cbx + ps->view_hw + 2
               && by >= cby - ps->view_hh - 2 && by <= cby + ps->view_hh + 2;
        if (in_view) {
            p->offscreen = 0;
        } else {
            p->offscreen++;
            /* THE SCRIPT'S OWN PERSON IS NEVER TOO FAR AWAY: a job puts
             * him at the far end of the city on purpose and the whole point
             * is to drive there. Same rule as the mission car's. */
            if (!p->mission &&
                ((p->corpse && p->offscreen > GTA_CORPSE_TICKS)
                || (!p->corpse && p->pull < 0
                    && p->offscreen > GTA_PED_RETIRE_TICKS)
                || bx < cbx - 24 || bx > cbx + 24
                || by < cby - 24 || by > cby + 24)) {
                p->alive = 0;
                ps->spawned_since_retire = 0;
                continue;
            }
        }

        /* THE GAIT WHEEL turns once for every ped, every tick, like the
         * original's global. */
        ps->gait_wheel = ps->gait_wheel % 20 + 1;

        if (p->pull >= 0) {
            /* DRAGGED OUT: the six states against the car, then down. */
            if (++p->pull_tick >= GTA_PULL_TICKS) {
                p->pull_tick = 0;
                p->pull++;
                if (p->pull == 2)
                    p->angle = p->pull_face;       /* 0x95: back to the car's */
                if (p->pull >= GTA_PULL_STATES) {
                    const gta_car_info *ci = &ps->tiles->cars[p->pull_model];
                    p->pull = -1;
                    p->down = GTA_PULL_DOWN_TICKS;
                    p->angle = (p->pull_face + door_side(ci) * 64) & 255;
                    continue;
                }
            }
            pull_place(ps, p);
            continue;
        }

        if (p->corpse)
            continue;

        /* THE SCRIPT'S OWN PERSON WAITS. He was put down at a place the job
         * names and the job expects to find him there; the wander, the kerb
         * rule and the crossing logic are all for the crowd. He still burns,
         * still falls and can still be run over - he is simply not going
         * anywhere on his own. */
        if (p->mode == GTA_PED_MODE_MISSION && p->burn <= 0 && !p->down) {
            p->speed = 0;
            continue;
        }

        /* BURNING. A point of health a tick, and he runs the whole time -
         * the original gives an AI ped speed 4 and leaves it there. The
         * fire's own frames turn over three times slower than the tick. */
        if (p->burn > 0) {
            if (++p->burn_tick >= GTA_FIRE_FRAME_TICKS) {
                p->burn_tick = 0;
                p->burn_frame = (p->burn_frame + 1) % GTA_FIRE_FRAMES;
            }
            if (--p->burn == 0) {
                p->corpse = 1;
                if (p->cop) ps->stat_cops_killed++;
                p->speed = 0;
                p->tx = p->ty = 0;
                ps->stat_killed++;
                continue;
            }
            if (!p->down && !p->fall && p->pull < 0)
                p->speed = 4;
        }

        if (p->shot) {
            /* SHOT: four states of two frames, the body carried along the
             * bullet's line for the first three (the original's 6 units a
             * tick, mode 5), then a body where it came to rest. A wall
             * stops the slide, nothing else does. */
            if (p->shot_step < GTA_SHOT_STATES - 1) {
                long nx = p->x + (long)gta_sin(p->shot_dir) * 3;
                long ny = p->y - (long)gta_cos(p->shot_dir) * 3;
                if (ground_at(ps, nx, ny, p->layer) != GTA_GROUND_BUILDING) {
                    p->x = nx; p->y = ny;
                }
            }
            if (++p->shot_tick >= GTA_SHOT_STATE_TICKS) {
                p->shot_tick = 0;
                if (++p->shot_step >= GTA_SHOT_STATES) {
                    p->corpse = 1;
                if (p->cop) ps->stat_cops_killed++;
                    p->speed = 0;
                    p->down = 0;
                    p->tx = p->ty = 0;
                    ps->stat_killed++;
                }
            }
            continue;
        }

        if (p->fall > 0) {
            /* PUNCHED DOWN, state 0xaf: a unit back a tick while nothing
             * blocks, then 0x2b lying alive. He faces the puncher, so
             * "back" is away from his own heading. */
            long nx = p->x - (long)gta_sin(p->angle) * 2;
            long ny = p->y + (long)gta_cos(p->angle) * 2;
            if (ground_at(ps, nx, ny, p->layer) != GTA_GROUND_BUILDING) {
                p->x = nx; p->y = ny;
            }
            if (--p->fall == 0)
                p->down = GTA_PUNCH_DOWN_TICKS;
            continue;
        }

        if (p->panic > 0)
            p->panic--;

        if (p->down > 0) {
            /* Lying, alive: up when the count runs out, turned a quarter
             * (the original's 0x2b exit). */
            if (--p->down == 0) {
                p->angle = (p->angle + 64) & 255;
                p->mode = GTA_PED_MODE_IDLE;
                p->sub = GTA_PED_SUB_WANDER;
                p->speed = 1;
                p->timer = 0;
            }
            continue;
        }

        /* ---- the brain proper ---- */
        {
            long nx, ny, ax, ay;
            int here = ground_at(ps, p->x, p->y, p->layer);
            int corner_ahead;

            /* ON A ROAD: the flee mode, straight away from where he
             * stepped on, at a run. Ours: it ends when he is back on
             * pavement a block from that point. */
            if (p->cop) {
                /* THE COP ON FOOT. Straight at the player, running when he
                 * is more than three blocks off, walking when close; the
                 * arrest is contact within GTA_COP_ARREST_PX. A player in a
                 * car is reached at the car: contact with the body of the
                 * car counts, and the game side pulls him out. A cop left
                 * far behind gives up (retired by distance like anyone). */
                int under = 0;
                long d = dist_to_player_body(ps, p, &under);
                p->mode = GTA_PED_MODE_COP;
                if (p->arrest_wait > 0) p->arrest_wait--;
                if (under) {
                    /* UNDER THE CAR - he got out of a police car that had
                     * stopped against the player's. Out first, and no
                     * arrest from in there. */
                    p->angle = angle_to(ps->pl_x, ps->pl_y, p->x, p->y);
                    p->speed = 2;
                    p->arrest = 0;
                    corner_ahead = 0;
                    continue;
                }
                if (p->post && d > 192) {
                    /* AT HIS POST: stands, faces the way he was put. */
                    p->speed = 0;
                    p->arrest = 0;
                    corner_ahead = 0;
                    continue;
                }
                if (p->cop_cool > 0) p->cop_cool--;
                if (ps->cop_shoot && ps->pl_layer == p->layer && d <= 96) {
                    /* SHOOT, DO NOT ARREST: the original's role 0x1f/0x20.
                     * Within three blocks he stands and fires the pistol
                     * at the player every 15 ticks (the AI cooldown), and
                     * at level 4 a player he still reaches is executed. */
                    p->angle = angle_to(p->x, p->y, ps->pl_x, ps->pl_y);
                    p->speed = d > 40 ? 2 : 0;
                    p->arrest = 0;
                    if (p->cop_cool == 0 && d > 8) {
                        p->shoot_req = 1;
                        p->shoot_angle = p->angle;
                        p->cop_cool = 15;
                        ps->stat_cop_shots++;
                    }
                    if (ps->cop_shoot >= 2 && d <= GTA_COP_ARREST_PX)
                        p->execute = 1;
                } else
                if (ps->pl_layer == p->layer && d <= GTA_COP_ARREST_PX &&
                    p->arrest_wait == 0) {
                    if (!p->arrest) {
                        p->arrest = 1;
                        printf("gta: police - the cop has him, %ld px away\n", d);
                        fflush(stdout);
                    }
                    p->speed = 0;
                } else {
                    p->angle = angle_to(p->x, p->y, ps->pl_x, ps->pl_y);
                    p->speed = d > 96 ? 3 : 2;
                    p->arrest = 0;
                }
                corner_ahead = 0;
            } else
            if (p->mode == GTA_PED_MODE_HUNT
                || p->mode == GTA_PED_MODE_SENDTO) {
                /* THE SCRIPT'S OWN PERSON WITH SOMEWHERE TO BE. The
                 * pavement rules are the crowd's: this one walks at his
                 * target across whatever is in the way, as the policeman
                 * does, because the job depends on his arriving. */
                long tx = p->tx, ty = p->ty, dx, dy, d;
                int want;
                if (p->hunt_pl) { tx = ps->pl_x; ty = ps->pl_y; }
                dx = (tx - p->x) >> 16;
                dy = (ty - p->y) >> 16;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                d = dx > dy ? dx : dy;
                /* STRAIGHT AT HIM IF THAT IS WALKABLE, otherwise the nearest
                 * heading either side that is. A right angle is the most it
                 * will take, which is what a building corner needs; anything
                 * beyond that is a route, and a route is the nav module's
                 * job. The side tried first alternates with `wobble` so he
                 * does not stand between two walls swapping every tick. */
                want = angle_to(p->x, p->y, tx, ty);
                if (!step_free(ps, p, want, 12)) {
                    static const int off[6] = { 32, 64, 96, -32, -64, -96 };
                    int k, base = p->wobble < 0 ? 3 : 0;
                    for (k = 0; k < 6; k++) {
                        int o = off[(base + k) % 6];
                        if (step_free(ps, p, (want + o) & 255, 12)) {
                            want = (want + o) & 255;
                            p->wobble = o < 0 ? -1 : 1;
                            break;
                        }
                    }
                }
                p->angle = want;
                if (p->mode == GTA_PED_MODE_SENDTO) {
                    /* THERE. He stands, and WAIT_FOR_PED sees him arrive
                     * by his position, not by a flag - which is what the
                     * original's own check does. */
                    p->speed = d > 8 ? (p->send_speed ? p->send_speed : 2) : 0;
                    if (d <= 8) {
                        p->mode = GTA_PED_MODE_MISSION;
                        p->speed = 0;
                        p->tx = p->ty = 0;
                    }
                } else {
                    /* HE CHASES, AND HE DOES NOT SHOOT. The first version
                     * gave him the policeman's pistol and four of them killed
                     * the player in under three seconds. The the original
                     * says otherwise: the call that makes AI types 0x15..0x2e
                     * sets the role, the target and mode 3 (chase) and
                     * assigns NO WEAPON, where the police roles that shoot -
                     * 0x1f and 0x20 - are given `weapon 1` explicitly beside
                     * the role. A hand-picked rate of fire for a subsystem
                     * that had not been read is exactly the mistake this
                     * project has a note about.
                     *
                     * WHAT WOULD SETTLE IT: the state machine for gang roles
                     * 0x18, 0x1a, 0x1c, 0x1b, 0x1d, 0x1e - the police ones
                     * are written up in the research notes and these are not.
                     * Until then he closes and stands over you, which is
                     * menacing and cannot be wrong by more than a punch. */
                    p->speed = d > 96 ? 3 : (d > 20 ? 2 : 0);
                }
                corner_ahead = 0;
            } else
            if (p->mode == GTA_PED_MODE_CROSS) {
                /* AT THE LIGHTS. Standing at the kerb until the cars along
                 * his own line have the green (then the ones across his
                 * path are stopped), then over at a run, slowing in the
                 * last six pixels, done within four of the far kerb. */
                long dx = (p->tx - p->x) >> 16, dy = (p->ty - p->y) >> 16;
                long d = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
                       ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
                if (p->sub == GTA_PED_SUB_STAND) {
                    int bx = (int)(p->x >> 21), by = (int)(p->y >> 21);
                    if (ps->light_green &&
                        ps->light_green(ps->light_ctx, bx, by, p->cross_axis_x)) {
                        p->sub = GTA_PED_SUB_JOG;
                        p->speed = 4;
                        ps->stat_crossings++;
                        printf("gta: ped %d sets off across at (%ld,%ld) to (%ld,%ld)\n",
                               i, p->x >> 16, p->y >> 16, p->tx >> 16, p->ty >> 16);
                    }
                } else {
                    p->angle = angle_to(p->x, p->y, p->tx, p->ty);
                    p->speed = d > 6 ? 4 : 2;
                    if (d <= 4) {
                        p->mode = GTA_PED_MODE_IDLE;
                        p->sub = GTA_PED_SUB_WANDER;
                        p->speed = 1;
                        p->timer = 0;
                        p->tx = p->ty = 0;
                        p->angle = snap_cardinal(p->angle);
                        ps->stat_crossed++;
                        printf("gta: ped %d crossed\n", i);
                    }
                }
                corner_ahead = 0;
            } else
            if (here == GTA_GROUND_ROAD && p->mode != GTA_PED_MODE_FLEE) {
                p->mode = GTA_PED_MODE_FLEE;
                p->gx = p->x; p->gy = p->y;
                p->flee_aim = 0;
                p->tx = p->ty = 0;
            }
            if (p->mode == GTA_PED_MODE_FLEE) {
                long dgx = (p->x - p->gx) >> 16, dgy = (p->y - p->gy) >> 16;
                if (here == GTA_GROUND_PAVEMENT && p->panic == 0
                    && (dgx > 32 || dgx < -32 || dgy > 32 || dgy < -32)) {
                    p->mode = GTA_PED_MODE_IDLE;
                    p->sub = GTA_PED_SUB_WANDER;
                    p->speed = 1;
                    p->timer = 0;
                    p->angle = snap_cardinal(p->angle);
                } else {
                    p->speed = 4;
                    if (++p->flee_aim >= FRAMES(5)) {
                        p->flee_aim = 0;
                        if (dgx || dgy)
                            p->angle = angle_to(p->gx, p->gy, p->x, p->y);
                    }
                }
            }

            /* A LIT CROSSING. A ped idle on a block the map marks as a
             * light tries, one tick in eleven, one of the four ways in
             * turn: the next block that way must be a light too (the
             * road at the stop line) and there must be pavement within
             * eight blocks beyond it. Then he waits at the kerb. */
            if (p->mode == GTA_PED_MODE_IDLE && p->tx == 0 && !p->down &&
                (rng_next(ps) % 11) == 0 &&
                hint_at(ps, p->x, p->y, p->layer) == 1) {
                static const int sx[4] = { 0, 1, 0, -1 }, sy[4] = { -1, 0, 1, 0 };
                int d = ps->cross_rr & 3;
                int bx = (int)(p->x >> 21), by = (int)(p->y >> 21);
                ps->cross_rr = (ps->cross_rr + 1) & 3;
                if (hint_at(ps, (long)(bx + sx[d]) << 21, (long)(by + sy[d]) << 21,
                            p->layer) == 1 &&
                    ground_at(ps, (long)(bx + sx[d]) << 21,
                              (long)(by + sy[d]) << 21, p->layer) == GTA_GROUND_ROAD) {
                    int k, found = 0;
                    for (k = 2; k <= 8 && !found; k++) {
                        int g = ground_at(ps, (long)(bx + sx[d] * k) << 21,
                                          (long)(by + sy[d] * k) << 21, p->layer);
                        if (g == GTA_GROUND_PAVEMENT) found = k;
                        else if (g != GTA_GROUND_ROAD) break;
                    }
                    if (found) {
                        p->mode = GTA_PED_MODE_CROSS;
                        p->sub = GTA_PED_SUB_STAND;
                        p->speed = 0;
                        p->cross_axis_x = sx[d] != 0;
                        p->tx = (((long)(bx + sx[d] * found) * 32 + 16) << 16);
                        p->ty = (((long)(by + sy[d] * found) * 32 + 16) << 16);
                        p->angle = (d * 64) & 255;
                    }
                }
            }

            if (p->mode == GTA_PED_MODE_IDLE) {
                /* the corner test decides both the turn and whether a stop
                 * is allowed here */
                ahead(p->x, p->y, snap_cardinal(p->angle), 24, &ax, &ay);
                corner_ahead = !pavement_at(ps, ax, ay, p->layer);

                if (p->tx == 0) {
                    /* THE GAIT */
                    if (--p->timer <= 0)
                        pick_gait(ps, p, corner_ahead);
                    if (p->sub == GTA_PED_SUB_STAND) {
                        /* the fidget: +/-8 now and then, through the shared
                         * flag like the original's */
                        if (!ps->fidget) {
                            unsigned long r = rng_next(ps);
                            if (r < 6000) { ps->fidget = 1; p->angle = (p->angle - 8) & 255; }
                            else if (r > 26000) { ps->fidget = 1; p->angle = (p->angle + 8) & 255; }
                        } else if (rng_next(ps) < 16000) {
                            ps->fidget = 0;
                        }
                    } else if (p->sub == GTA_PED_SUB_VEER_L
                            || p->sub == GTA_PED_SUB_VEER_R) {
                        /* four frames of 45 degrees back, then straight */
                        if (p->timer <= FRAMES(100) - FRAMES(4)) {
                            p->angle = snap_cardinal(p->angle);
                            p->sub = GTA_PED_SUB_WANDER;
                            p->speed = 1;
                        }
                    } else {
                        if (p->sub == GTA_PED_SUB_BLIP || rng_next(ps) > 24000)
                            wobble(ps, p);
                        /* THE CORNER: pavement ends within three quarters of
                         * a block - aim at the pavement to the side. */
                        if (corner_ahead) {
                            if (!corner(ps, p)) {
                                /* nowhere to the side either: carry on and
                                 * let the step test decide (a one-block
                                 * alley is crossed, a wall turns him) */
                            }
                        } else if ((p->angle & 63) != 0) {
                            /* DRIFTING OFF THE EDGE at a slant: the ground
                             * six pixels to either side not pavement - veer
                             * 45 degrees back for four frames. */
                            long qx, qy;
                            ahead(p->x, p->y, (p->angle - 64) & 255, 6, &qx, &qy);
                            if (!pavement_at(ps, qx, qy, p->layer)) {
                                p->sub = GTA_PED_SUB_VEER_R;
                                p->angle = (p->angle + 32) & 255;
                                p->timer = FRAMES(100);
                            } else {
                                ahead(p->x, p->y, (p->angle + 64) & 255, 6, &qx, &qy);
                                if (!pavement_at(ps, qx, qy, p->layer)) {
                                    p->sub = GTA_PED_SUB_VEER_L;
                                    p->angle = (p->angle - 32) & 255;
                                    p->timer = FRAMES(100);
                                }
                            }
                        }
                    }
                } else {
                    /* WALKING TO THE CORNER TARGET: aim at it; within 9 px
                     * (the original's 18 units) it is reached - snap to
                     * the cardinal and wander on. */
                    long dx = (p->tx - p->x) >> 16, dy = (p->ty - p->y) >> 16;
                    if (dx > -9 && dx < 9 && dy > -9 && dy < 9) {
                        p->tx = p->ty = 0;
                        p->angle = snap_cardinal(p->angle);
                    } else {
                        p->angle = angle_to(p->x, p->y, p->tx, p->ty);
                    }
                    if (p->speed == 0) p->speed = 1;
                }
            }

            /* OUR SEPARATION, before the step - for the man standing
             * still as well, so a knot round somebody who has stopped for a
             * rest comes apart the same way. */
            {
                int crowded = separation(ps, p);
                if (crowded && p->speed > 1 && p->mode == GTA_PED_MODE_IDLE)
                    p->speed = 1;

                /* THE SIDESTEP IS A STATE, NOT A NUDGE (the developer,
                 * 2026-09-09: three men walking straight into each other
                 * for two minutes). separation()'s six-unit turn lasted
                 * exactly one tick, because every walking mode above
                 * re-aims the heading at its target each tick, so a man
                 * with somewhere to go never turned at all - he walked into
                 * the man ahead, relax() pushed them a pixel apart, and he
                 * walked in again. Now somebody close ahead starts a dodge:
                 * for PED_DODGE_TICKS the aim the mode chose is bent a
                 * quarter turn to one side, and the step goes on. The
                 * right is tried first for everybody, so a head-on pair
                 * bend to opposite absolute sides and pass; the left only
                 * when the right is a wall. A side that is walled while
                 * the dodge runs simply keeps the plain aim for that tick. */
                /* AND WHEN PASSING HAS NOT WORKED, WALK AWAY. Three wanderers
                 * with no target each sidestepped to the right of the next
                 * one and went round each other in one block for 1750 ticks
                 * (the stuck logger's second catch, `gtadump peds ... 64 64
                 * 9000 12345`). The logger's own counter is the signal: a
                 * man who has wanted out of this block for two seconds and
                 * is still crowded turns his back on the nearest body -
                 * the nearest free cardinal to "away" - and keeps that
                 * heading for the length of a dodge, at a brisk step. Only
                 * for a man with nowhere in particular to be; one with a
                 * target keeps trying to get past. */
                if (crowded && p->dodge == 0 && p->speed > 0
                    && p->pull < 0 && !p->arrest
                    && p->blk_ticks > PED_LEAVE_TICKS
                    && p->mode == GTA_PED_MODE_IDLE
                    && p->tx == 0 && p->ty == 0) {
                    int k, best = -1;
                    long bd = 0;
                    for (k = 0; k < GTA_MAX_PEDS; k++) {
                        const gta_ped *o = &ps->p[k];
                        long ddx, ddy, d;
                        if (k == i || !o->alive || o->corpse
                            || o->layer != p->layer)
                            continue;
                        ddx = (o->x - p->x) >> 16; ddy = (o->y - p->y) >> 16;
                        d = ddx * ddx + ddy * ddy;
                        if (best < 0 || d < bd) { best = k; bd = d; }
                    }
                    if (best >= 0) {
                        static const int turn[4] = { 0, 64, -64, 128 };
                        int away = snap_cardinal(angle_to(ps->p[best].x,
                                        ps->p[best].y, p->x, p->y));
                        for (k = 0; k < 4; k++)
                            if (step_free(ps, p, (away + turn[k]) & 255, 12)) {
                                p->angle = (away + turn[k]) & 255;
                                p->dodge = PED_DODGE_TICKS * 2;
                                p->dodge_side = 0;      /* no bend: the heading IS the way out */
                                break;
                            }
                    }
                }
                if (crowded && p->dodge == 0 && p->speed > 0
                    && p->pull < 0 && !p->arrest) {
                    if (step_free(ps, p, (p->angle + PED_DODGE_TURN) & 255, 8)) {
                        p->dodge = PED_DODGE_TICKS; p->dodge_side = 1;
                    } else if (step_free(ps, p, (p->angle - PED_DODGE_TURN) & 255, 8)) {
                        p->dodge = PED_DODGE_TICKS; p->dodge_side = -1;
                    }
                }
                if (p->dodge > 0) {
                    int a = (p->angle + PED_DODGE_TURN * p->dodge_side) & 255;
                    p->dodge--;
                    if (p->dodge_side != 0 && p->speed > 0
                        && step_free(ps, p, a, 8))
                        p->angle = a;
                    /* out of a crowd at a brisk step, not the shuffle the
                     * crowd rule just set: at speed 1 relax() wins */
                    if (p->speed == 1)
                        p->speed = 2;
                }
            }

            /* THE STEP, and what refuses it. */
            if (p->speed > 0) {
                int there, blocked = 0;
                /* Q14 sin x (16.16 px a tick >> 8) >> 6 = 16.16. The
                 * shift binds looser than the +, hence the brackets: the
                 * first build had (x + step) >> 6 and every ped teleported
                 * to the map's corner and was culled on its first tick. */
                {
                    long v = (PED_UNIT * p->speed) >> 8;
                    nx = p->x + (((long)gta_sin(p->angle) * v) >> 6);
                    ny = p->y - (((long)gta_cos(p->angle) * v) >> 6);
                }
                there = ground_at(ps, nx, ny, p->layer);
                if (there == GTA_GROUND_BUILDING || there == GTA_GROUND_AIR
                    || there == GTA_GROUND_WATER)
                    blocked = 1;
                else if (there == GTA_GROUND_FIELD && here != GTA_GROUND_FIELD
                         && !p->cop && p->mode != GTA_PED_MODE_HUNT
                         && p->mode != GTA_PED_MODE_SENDTO)
                    blocked = 1;
                else if (there == GTA_GROUND_ROAD && here != GTA_GROUND_ROAD
                         && p->mode != GTA_PED_MODE_FLEE && !p->cop
                         && p->mode != GTA_PED_MODE_HUNT
                         && p->mode != GTA_PED_MODE_SENDTO
                         && p->mode != GTA_PED_MODE_CROSS) {
                    /* the original's block-and-a-half rule: a road is
                     * stepped onto only with pavement 48 px beyond */
                    long fx, fy;
                    ahead(p->x, p->y, p->angle, 48, &fx, &fy);
                    if (!pavement_at(ps, fx, fy, p->layer))
                        blocked = 1;
                }
                if (blocked && p->mode == GTA_PED_MODE_FLEE) {
                    /* a wall in the flee: turn 90 and keep running */
                    p->angle = (p->angle + 64) & 255;
                } else if (blocked && (p->cop || p->mode == GTA_PED_MODE_HUNT
                                       || p->mode == GTA_PED_MODE_SENDTO)) {
                    /* Round the obstacle, and try the other way next. */
                    p->angle = (p->angle + (p->stuck & 1 ? 48 : -48)) & 255;
                    p->stuck++;
                } else if (blocked && p->mode == GTA_PED_MODE_CROSS) {
                    /* Something in the way on the crossing: back to the
                     * ordinary walk, not to a walk target of (0,0). */
                    printf("gta: ped %d crossing abandoned at (%ld,%ld): ground %d ahead\n",
                           i, p->x >> 16, p->y >> 16, there);
                    p->mode = GTA_PED_MODE_IDLE;
                    p->sub = GTA_PED_SUB_WANDER;
                    p->speed = 1;
                    p->timer = FRAMES(3);
                    p->tx = p->ty = 0;
                    p->angle = (p->angle + 128) & 255;
                } else if (blocked) {
                    /* the original: 135 degrees round, and the gait is
                     * re-rolled three frames later */
                    p->angle = (p->angle - 96) & 255;
                    p->tx = p->ty = 0;
                    p->timer = FRAMES(3);
                    p->stuck = 0;
                } else {
                    if (nx == p->x && ny == p->y) {
                        if (++p->stuck > FRAMES(8)) {
                            p->angle = (p->angle + 80) & 255;
                            p->stuck = 0;
                        }
                    } else {
                        p->stuck = 0;
                    }
                    p->x = nx; p->y = ny;
                }
            }
        }

        stuck_log(ps, p, i);

        /* THE CADENCE: the run cycle from speed 3, the original's rule. */
        if (p->speed > 0
            && ++p->frame_tick >= (p->speed >= 3 ? PED_RUN_TICKS : PED_WALK_TICKS)) {
            p->frame_tick = 0;
            p->frame = (p->frame + 1) & 7;
        }
    }

    /* THE SPAWNER: while the pool has room, one attempt a tick and a second
     * when nothing has been let go since the last birth - the original's
     * own spawn rule. */
    if (alive < GTA_MAX_PEDS) {
        if (!spawn_one(ps, cbx, cby) && !ps->spawned_since_retire)
            spawn_one(ps, cbx, cby);
    }

    /* ...and nobody is left standing inside anybody else. */
    relax(ps);
}

int gta_peds_ram(gta_peds *ps, long px, long py, int pface, int phl, int phw,
                 int layer, long speed)
{
    int i, hits = 0;
    long fx = gta_sin(pface), fy = -gta_cos(pface);
    long rx = gta_cos(pface), ry = gta_sin(pface);
    int fast = speed >= 5L * 32768L || speed <= -5L * 32768L;

    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        long dx, dy, along, side;

        if (!p->alive || p->corpse || p->pull >= 0 || p->layer != layer)
            continue;
        dx = (p->x - px) >> 16;
        dy = (p->y - py) >> 16;
        if (dx > 64 || dx < -64 || dy > 64 || dy < -64)
            continue;
        /* A ped is a point against the car's box - three pixels of body. */
        along = (dx * (fx >> 6) + dy * (fy >> 6)) >> 8;
        side  = (dx * (rx >> 6) + dy * (ry >> 6)) >> 8;
        if (along < 0) along = -along;
        if (side  < 0) side  = -side;
        /* A LYING MAN IS FLAT: the three pixels of body are not there,
         * and he has to be under the car proper. The driver dragged out
         * lies two pixels outside his own car's flank - within the
         * standing man's margin, so driving off used to kill him where
         * the original lets him get up and run. */
        if (p->down && (along > phl - 2 || side > phw - 2))
            continue;
        if (along <= phl + 3 && side <= phw + 3) {
            if (fast) {
                /* KILLED WHERE HE STANDS - the original's 0x2d: no
                 * displacement, the body stays. Blood and the crime report
                 * belong with objects and the wanted level. */
                if (!p->down) {
                    p->corpse = 1;
                if (p->cop) ps->stat_cops_killed++;
                    p->speed = 0;
                    p->tx = p->ty = 0;
                    ps->stat_killed++;
                    ps->stat_runover++;
                    hits++;
                } else {
                    p->corpse = 1;      /* run over while lying: dead too */
                if (p->cop) ps->stat_cops_killed++;
                }
            } else if (!p->down) {
                /* SHOVED - the original's 0x92: along his own heading, a
                 * unit a tick, while the car is on him. */
                p->x += (long)gta_sin(p->angle) * 2;
                p->y -= (long)gta_cos(p->angle) * 2;
                p->speed = 0;
                p->timer = FRAMES(3);
            }
        }
    }
    return hits;
}

void gta_peds_draw(gta_peds *ps, gta_view *v)
{
    int i;

    for (i = 0; i < GTA_MAX_PEDS; i++) {
        const gta_ped *p = &ps->p[i];
        int f;

        if (!p->alive)
            continue;
        /* WHICH FRAME: the pull's own sprites; a body (43, the original's
         * 0x2d) or somebody lying alive (42); standing still (98); the run
         * cycle from speed 3, else the walk cycle. */
        if (p->pull >= 0)
            f = p->pull < 3 ? 80 : p->pull == 3 ? 46 : 45;
        else if (p->corpse)
            f = p->shot == 2 ? 44 : 43;     /* 0x2c shot from behind, else 0x2d */
        else if (p->shot)
            /* 0x89..0x8c: 38,38,38,39; 0x8d..0x90: 14,14,14,41 */
            f = p->shot == 2 ? (p->shot_step < 3 ? 14 : 41)
                             : (p->shot_step < 3 ? 38 : 39);
        else if (p->fall)
            f = GTA_PED_FALL_FIRST;         /* 0xaf: 38 */
        else if (p->down)
            f = GTA_PED_LIES_ON_FLOOR;
        else if (p->speed == 0)
            f = GTA_PED_STAND;
        else
            f = (p->speed >= 3 ? GTA_PED_RUN_FIRST : GTA_PED_WALK_FIRST)
              + p->frame;
        if (f >= ps->ped_count)
            f = 0;
        /* A POLICEMAN IS ANOTHER SET OF SPRITES, not another colour: the
         * original picks `frame + ped+0x15 * 0xbd` (the original's routine), and a cop
         * has +0x15 = 1 with remap 0 - the uniform and the cap are drawn in
         * the second set, 189 sprites on, not painted over the first. He
         * used to be the first set with remap 0, which is the player's own
         * yellow shirt (developer, 2026-09-25). */
        if (p->cop && f + GTA_PED_COP_SET < ps->ped_count)
            f += GTA_PED_COP_SET;
        gta_render_add_sprite_r(v, p->x, p->y, p->layer, p->layer,
                              ps->ped_base + f,
                              (p->angle + GTA_SPRITE_ART_SOUTH) & 255,
                              p->remap);
        /* ...and the fire on top of him, if he is alight. */
        if (p->burn > 0 && ps->fire_sprite >= 0)
            gta_render_add_sprite(v, p->x, p->y, p->layer, p->layer,
                                  ps->fire_sprite + p->burn_frame, 0);
    }
}

/* ---- GUNS AND FISTS (Phase 5 item 5) -------------------------------- */

int gta_peds_hit_segment(const gta_peds *ps, long x, long y, int heading,
                         int len_px, int hw_px, int layer, int skip)
{
    int i, best = -1;
    long best_along = 0;
    long fx = gta_sin(heading), fy = -gta_cos(heading);
    long rx = gta_cos(heading), ry = gta_sin(heading);

    for (i = 0; i < GTA_MAX_PEDS; i++) {
        const gta_ped *p = &ps->p[i];
        long dx, dy, along, side;
        if (!p->alive || p->corpse || p->pull >= 0 || p->shot
            || p->layer != layer || i == skip)
            continue;
        dx = (p->x - x) >> 16;
        dy = (p->y - y) >> 16;
        if (dx > 64 || dx < -64 || dy > 64 || dy < -64)
            continue;
        along = (dx * fx + dy * fy) >> 14;
        side  = (dx * rx + dy * ry) >> 14;
        if (side < 0) side = -side;
        /* two pixels of body either way */
        if (along < -2 || along > len_px + 2 || side > hw_px + 2)
            continue;
        if (best < 0 || along < best_along) {
            best = i;
            best_along = along;
        }
    }
    return best;
}

void gta_peds_shoot(gta_peds *ps, int i, int heading)
{
    gta_ped *p = &ps->p[i];
    int d;
    if (!p->alive || p->corpse || p->shot)
        return;
    /* THE FRONT/BEHIND SPLIT: his heading within a quarter turn of the
     * bullet's (the original's 0x100 of 0x400) -> shot in the back, he
     * keeps the bullet's heading and falls forward; else he is turned to
     * face the shooter and falls back. Either way the body travels along
     * the bullet. */
    d = (p->angle - heading) & 255;
    if (d < 64 || d > 192) {
        p->shot = 2;
        p->angle = heading & 255;
    } else {
        p->shot = 1;
        p->angle = (heading + 128) & 255;
    }
    p->shot_dir = heading & 255;
    p->shot_step = 0;
    p->shot_tick = 0;
    p->speed = 0;
    p->down = 0;
    p->fall = 0;
    p->tx = p->ty = 0;
    ps->stat_shot++;
}

int gta_peds_punch(gta_peds *ps, long x, long y, int angle, int layer)
{
    int i, hit = -1;
    long ax, ay;
    /* the 6x6-unit box 8 units ahead: 3x3 px, 4 px ahead */
    ahead(x, y, angle, 4, &ax, &ay);
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        const gta_ped *p = &ps->p[i];
        long dx, dy;
        if (!p->alive || p->corpse || p->pull >= 0 || p->shot || p->fall
            || p->down || p->layer != layer)
            continue;
        dx = (p->x - ax) >> 16;
        dy = (p->y - ay) >> 16;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx <= 3 && dy <= 3) { hit = i; break; }
    }
    if (hit < 0)
        return -1;
    /* the original's global 0..12 counter: 10 of 13 land; the 11th and
     * 12th are the victim's counter-punch, which is not built yet */
    ps->punch_wheel = (ps->punch_wheel + 1) % 13;
    if (ps->punch_wheel >= 10)
        return -2;
    {
        gta_ped *p = &ps->p[hit];
        p->angle = (angle + 128) & 255;     /* faces the puncher */
        p->fall = GTA_FALL_TICKS;
        p->speed = 0;
        p->tx = p->ty = 0;
        p->mode = GTA_PED_MODE_IDLE;
        p->panic = 0;
        ps->stat_punched++;
    }
    return hit;
}

void gta_peds_panic(gta_peds *ps, long x, long y, int layer)
{
    int i;
    ps->panic_n = 0;
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        long dx, dy;
        if (!p->alive || p->corpse || p->pull >= 0 || p->shot || p->fall
            || p->down || p->layer != layer || p->speed >= 4)
            continue;
        /* WHO HEARS IT: the original's routine walks the original's routine's list, every ped
         * in the two-block cell of the point and the eight around it, with
         * no distance test of its own - so a shot frightens everybody two
         * to four blocks away, not the one block this used to take (208).
         * The port's block is 32 px, 1 << 21 in 16.16; a cell is two. */
        dx = (p->x >> 22) - (x >> 22);
        dy = (p->y >> 22) - (y >> 22);
        if (dx > 1 || dx < -1 || dy > 1 || dy < -1)
            continue;
        dx = (p->x - x) >> 16;
        dy = (p->y - y) >> 16;
        /* mode 1, the threat is the shooter: run straight away from him,
         * re-aimed every fifth frame like the road flee, for at least
         * GTA_PANIC_TICKS before the pavement rule may end it */
        p->mode = GTA_PED_MODE_FLEE;
        p->gx = x; p->gy = y;
        p->flee_aim = 0;
        p->tx = p->ty = 0;
        p->speed = 4;
        p->panic = GTA_PANIC_TICKS;
        if (dx || dy)
            p->angle = angle_to(x, y, p->x, p->y);
        else
            p->angle = (int)(rng_next(ps) & 255);
        if (ps->panic_n < GTA_PANIC_LIST)
            ps->panic_idx[ps->panic_n++] = i;
    }
}

void gta_peds_burn(gta_peds *ps, int i, long fx, long fy)
{
    gta_ped *p;
    if (i < 0 || i >= GTA_MAX_PEDS)
        return;
    p = &ps->p[i];
    if (!p->alive || p->corpse || p->shot || p->pull >= 0 || p->burn > 0)
        return;
    p->burn = GTA_BURN_TICKS;
    p->burn_frame = 0;
    p->burn_tick = 0;
    /* He runs, away from whatever set him alight - the original's mode 1
     * with the fire as the threat. */
    p->mode = GTA_PED_MODE_FLEE;
    p->gx = fx; p->gy = fy;
    p->flee_aim = 0;
    p->panic = GTA_PANIC_TICKS;
    p->tx = p->ty = 0;
    p->speed = 4;
    p->down = 0;
    p->fall = 0;
    if ((p->x >> 16) != (fx >> 16) || (p->y >> 16) != (fy >> 16))
        p->angle = angle_to(fx, fy, p->x, p->y);
}

/* ---- the police on foot ------------------------------------------------ */

void gta_peds_set_player(gta_peds *ps, long x, long y, int layer, int in_car,
                         int hl, int hw, int face)
{
    ps->pl_x = x;
    ps->pl_y = y;
    ps->pl_layer = layer;
    ps->pl_in_car = in_car;
    ps->pl_hl = hl;
    ps->pl_hw = hw;
    ps->pl_face = face;
}

unsigned long gta_peds_spawn_mission(gta_peds *ps, long x, long y, int layer,
                                     int angle)
{
    int i = free_slot(ps);
    if (i < 0)
        i = free_slot_forced(ps);
    if (i < 0) { ps->last_index = -1; return 0; }
    ped_reset(ps, &ps->p[i], x, y, layer, angle, 0);
    ps->p[i].mode = GTA_PED_MODE_MISSION;
    ps->p[i].speed = 0;
    ps->p[i].tx = ps->p[i].ty = 0;
    ps->p[i].mission = 1;
    ps->p[i].serial = ++ps->next_ped_serial;
    ps->stat_spawned++;
    ps->last_index = i;
    return ps->p[i].serial;
}

int gta_peds_find(const gta_peds *ps, unsigned long serial,
                  long *x, long *y, int *layer, int *alive)
{
    int i;
    if (!serial)
        return 0;
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        if (ps->p[i].serial != serial)
            continue;
        if (x) *x = ps->p[i].x;
        if (y) *y = ps->p[i].y;
        if (layer) *layer = ps->p[i].layer;
        if (alive) *alive = ps->p[i].alive && !ps->p[i].corpse;
        return 1;
    }
    return 0;
}

int gta_peds_mission_ai(gta_peds *ps, unsigned long serial, int mode,
                        long tx, long ty, int on_player)
{
    int i;
    if (!serial)
        return 0;
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        if (p->serial != serial || !p->alive || p->corpse)
            continue;
        p->mode = mode;
        p->tx = tx;
        p->ty = ty;
        p->hunt_pl = (unsigned char)(on_player ? 1 : 0);
        p->timer = 0;
        p->stuck = 0;
        if (mode == GTA_PED_MODE_MISSION)
            p->speed = 0;
        return 1;
    }
    return 0;
}

int gta_peds_set_speed(gta_peds *ps, unsigned long serial, int speed)
{
    int i;
    if (!serial || speed < 0)
        return 0;
    if (speed > 4) speed = 4;
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (ps->p[i].serial == serial && ps->p[i].alive) {
            ps->p[i].send_speed = (unsigned char)speed;
            return 1;
        }
    return 0;
}

int gta_peds_set_remap(gta_peds *ps, unsigned long serial, int remap)
{
    int i;
    if (!serial || remap < 0)
        return 0;
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (ps->p[i].serial == serial && ps->p[i].alive) {
            ps->p[i].remap = remap;
            return 1;
        }
    return 0;
}

int gta_peds_at(const gta_peds *ps, unsigned long serial, long x, long y,
                int tol_px)
{
    long px_, py_, dx, dy;
    if (!gta_peds_find(ps, serial, &px_, &py_, 0, 0))
        return 0;
    dx = px_ - x; if (dx < 0) dx = -dx;
    dy = py_ - y; if (dy < 0) dy = -dy;
    return dx <= ((long)tol_px << 16) && dy <= ((long)tol_px << 16);
}

int gta_peds_take_mission(gta_peds *ps, unsigned long serial)
{
    int i;
    if (!serial)
        return 0;
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (ps->p[i].serial == serial && ps->p[i].alive) {
            ps->p[i].alive = 0;
            ps->p[i].mission = 0;
            ps->p[i].serial = 0;
            return 1;
        }
    return 0;
}

int gta_peds_spawn_cop(gta_peds *ps, long x, long y, int layer, int angle)
{
    int i = free_slot(ps);
    if (i < 0)
        i = free_slot_forced(ps);
    if (i < 0) { ps->last_index = -1; return 0; }
    ped_reset(ps, &ps->p[i], x, y, layer, angle, 0);
    ps->p[i].cop = 1;
    ps->p[i].arrest_wait = GTA_COP_ARREST_WAIT;
    ps->p[i].mode = GTA_PED_MODE_COP;
    ps->p[i].speed = 2;
    ps->stat_spawned++;
    ps->stat_cops_out++;
    ps->last_index = i;
    return 1;
}

void gta_peds_post_last_cop(gta_peds *ps)
{
    if (ps->last_index >= 0 && ps->last_index < GTA_MAX_PEDS)
        ps->p[ps->last_index].post = 1;
}

int gta_peds_knock_off(gta_peds *ps, long x, long y, int layer, int angle, int remap)
{
    int i = free_slot_forced(ps);
    gta_ped *p;
    if (i < 0) { ps->last_index = -1; return 0; }
    p = &ps->p[i];
    ped_reset(ps, p, x, y, layer, angle, remap);
    p->fall = GTA_FALL_TICKS;
    p->speed = 0;
    p->tx = p->ty = 0;
    ps->stat_spawned++;
    ps->stat_punched++;
    ps->last_index = i;
    return 1;
}

void gta_peds_set_cop_shoot(gta_peds *ps, int mode)
{
    ps->cop_shoot = mode;
}

int gta_peds_cop_shot(gta_peds *ps, long *x, long *y, int *layer, int *angle)
{
    int i;
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        if (!p->alive || p->corpse || !p->shoot_req) continue;
        p->shoot_req = 0;
        *x = p->x; *y = p->y; *layer = p->layer; *angle = p->shoot_angle;
        return i;
    }
    return -1;
}

int gta_peds_cop_execute(gta_peds *ps)
{
    int i, hit = 0;
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        if (p->alive && p->cop && !p->corpse && p->execute) { p->execute = 0; hit = 1; }
    }
    return hit;
}

void gta_peds_make_cop(gta_peds *ps, int i)
{
    gta_ped *p;
    if (i < 0 || i >= GTA_MAX_PEDS) return;
    p = &ps->p[i];
    p->cop = 1;
    p->arrest_wait = GTA_COP_ARREST_WAIT;
    p->remap = 0;
    p->mode = GTA_PED_MODE_COP;
    ps->stat_cops_out++;
}

int gta_peds_cop_event(gta_peds *ps)
{
    int i, hit = 0;
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        if (p->alive && p->cop && !p->corpse && p->arrest) {
            p->arrest = 0;
            hit = 1;
        }
    }
    return hit;
}

void gta_peds_clear_cops(gta_peds *ps)
{
    int i;
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (ps->p[i].alive && ps->p[i].cop && !ps->p[i].corpse)
            ps->p[i].alive = 0;
}

int gta_peds_cops_out(const gta_peds *ps)
{
    int i, n = 0;
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (ps->p[i].alive && ps->p[i].cop && !ps->p[i].corpse)
            n++;
    return n;
}

void gta_peds_kill(gta_peds *ps, int i)
{
    gta_ped *p;
    if (i < 0 || i >= GTA_MAX_PEDS)
        return;
    p = &ps->p[i];
    if (!p->alive || p->corpse)
        return;
    p->corpse = 1;
                if (p->cop) ps->stat_cops_killed++;
    p->burn = 0;
    p->speed = 0;
    p->down = 0;
    p->fall = 0;
    p->shot = 0;
    p->tx = p->ty = 0;
    ps->stat_killed++;
}
