/* Pedestrians - see gta_peds.h for what is the original's and what is ours.
 *
 * Licence: MIT (ours).
 */
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
    p->mode = GTA_PED_MODE_IDLE;
    p->sub = GTA_PED_SUB_WANDER;
    p->speed = 1;
    p->timer = 0;
    p->wobble = 0;
    p->tx = p->ty = 0;
    p->gx = p->gy = 0;
    p->stuck = 0;
    p->offscreen = 0;
    p->flee_aim = 0;
    p->down = 0;
    p->corpse = 0;
    p->pull = -1;
    p->pull_tick = 0;
    p->shot = p->shot_step = p->shot_tick = p->shot_dir = 0;
    p->fall = 0;
    p->panic = 0;
}

static int free_slot(gta_peds *ps)
{
    int i, oldest = -1, oldest_off = -1;
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (!ps->p[i].alive)
            return i;
    /* THE POOL IS TWELVE, NOT TWO HUNDRED: a street of bodies would starve
     * the spawner for good, so a corpse gives up its slot to a newcomer -
     * the one that has been out of sight longest. */
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (ps->p[i].corpse && ps->p[i].offscreen > oldest_off) {
            oldest = i; oldest_off = ps->p[i].offscreen;
        }
    return oldest;
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

static int door_side(const gta_car_info *ci)
{
    return ci->n_doors > 0 ? (ci->doors[0].rpy < 0 ? 1 : -1) : 1;
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

int gta_peds_pull(gta_peds *ps, long cx, long cy, int face, int model,
                  int layer, int remap)
{
    const gta_car_info *ci = &ps->tiles->cars[model];
    int i = free_slot(ps);
    gta_ped *p;

    if (i < 0)
        return 0;
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
    return 1;
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
 * sidestep away; within 3 px - hold this tick. */
static int separation(gta_peds *ps, gta_ped *p, int *hold)
{
    int i, crowded = 0;
    long fx = gta_sin(p->angle), fy = -gta_cos(p->angle);
    *hold = 0;
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        const gta_ped *o = &ps->p[i];
        long dx, dy, along, side;
        if (o == p || !o->alive || o->corpse || o->down || o->layer != p->layer)
            continue;
        dx = (o->x - p->x) >> 16; dy = (o->y - p->y) >> 16;
        if (dx > 6 || dx < -6 || dy > 6 || dy < -6)
            continue;
        along = (dx * fx + dy * fy) >> 14;
        side  = (dx * fy - dy * fx) >> 14;      /* + = to the right */
        if (along < 0 || along > 5)
            continue;
        crowded = 1;
        if (along <= 3 && side > -2 && side < 2)
            *hold = 1;
        p->angle = (p->angle + (side >= 0 ? -6 : 6)) & 255;
    }
    return crowded;
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
            if ((p->corpse && p->offscreen > GTA_CORPSE_TICKS)
                || (!p->corpse && p->pull < 0
                    && p->offscreen > GTA_PED_RETIRE_TICKS)
                || bx < cbx - 24 || bx > cbx + 24
                || by < cby - 24 || by > cby + 24) {
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
            int hold = 0;

            /* ON A ROAD: the flee mode, straight away from where he
             * stepped on, at a run. Ours: it ends when he is back on
             * pavement a block from that point. */
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

            /* OUR SEPARATION, before the step. */
            if (p->speed > 0 && separation(ps, p, &hold) && p->speed > 1
                && p->mode == GTA_PED_MODE_IDLE)
                p->speed = 1;

            /* THE STEP, and what refuses it. */
            if (p->speed > 0 && !hold) {
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
                else if (there == GTA_GROUND_FIELD && here != GTA_GROUND_FIELD)
                    blocked = 1;
                else if (there == GTA_GROUND_ROAD && here != GTA_GROUND_ROAD
                         && p->mode != GTA_PED_MODE_FLEE) {
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
        if (along <= phl + 3 && side <= phw + 3) {
            if (fast) {
                /* KILLED WHERE HE STANDS - the original's 0x2d: no
                 * displacement, the body stays. Blood and the crime report
                 * belong with objects and the wanted level. */
                if (!p->down) {
                    p->corpse = 1;
                    p->speed = 0;
                    p->tx = p->ty = 0;
                    ps->stat_killed++;
                    ps->stat_runover++;
                    hits++;
                } else {
                    p->corpse = 1;      /* run over while lying: dead too */
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
        gta_render_add_sprite_r(v, p->x, p->y, p->layer, p->layer,
                              ps->ped_base + f,
                              (p->angle + GTA_SPRITE_ART_SOUTH) & 255,
                              p->remap);
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
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        long dx, dy;
        if (!p->alive || p->corpse || p->pull >= 0 || p->shot || p->fall
            || p->down || p->layer != layer || p->speed >= 4)
            continue;
        dx = (p->x - x) >> 16;
        dy = (p->y - y) >> 16;
        if (dx > 32 || dx < -32 || dy > 32 || dy < -32)
            continue;
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
    }
}
