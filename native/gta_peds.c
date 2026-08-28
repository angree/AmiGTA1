/* Pedestrians - see gta_peds.h for what this is and is not yet.
 *
 * Licence: MIT (ours).
 */
#include "gta_peds.h"
#include "gta_style.h"
#include "gta_trig.h"
#include "gta_player.h"     /* GTA_SPRITE_ART_SOUTH - one convention */

/* WALKING PACE, AND IT USED TO BE A JOG.
 *
 * This was 72000, described in the comment as "a touch slower than the
 * player" - but the player's 1.30 px/tick is his RUN; his walk is 0.60. So
 * the entire population was moving at 85% of a sprint while playing the
 * eight-frame WALK cycle, and it read exactly as what it was: everyone
 * running. Reported from the screen as "the little people are running".
 *
 * These two now match gta_player.c's own WALK_SPEED and RUN_SPEED, so a
 * pedestrian and the player move at the same pace when doing the same thing.
 * One in PED_RUN_ONE_IN is in a hurry; the rest walk. */
#define PED_WALK_SPEED  39321L      /* 0.60 world px/tick = 0.94 blocks/s */
#define PED_RUN_SPEED   85197L      /* 1.30 world px/tick = 2.03 blocks/s */
#define PED_RUN_ONE_IN  8
#define PED_WALK_TICKS  7           /* the player's own cadence, both of them */
#define PED_RUN_TICKS   4
#define PED_DOWN_TICKS 250      /* five seconds flat on the road */
#define PED_SPAWN_TICKS 25      /* one spawn attempt twice a second */

/* The compass in the map's own bit order: N E S W. */
static const int step_dx[4] = { 0, 1, 0, -1 };
static const int step_dy[4] = { -1, 0, 1, 0 };

static unsigned long rng_next(gta_peds *ps)
{
    ps->rng = ps->rng * 1103515245UL + 12345UL;
    return (ps->rng >> 16) & 0x7FFF;
}

static int pavement_at(const gta_peds *ps, int bx, int by, int z)
{
    unsigned char b = gta_nav_at_m(ps->nav, bx, by, z);
    return gta_nav_ground(b) == 3;
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
    ps->spawn_wait = 0;
    ps->stat_spawned = ps->stat_runover = 0;
}

void gta_peds_set_nav(gta_peds *ps, const gta_nav *nav)
{
    ps->nav = nav;
}

/* A pavement block within the ring around the camera, or 0. The same
 * spawn-out-of-sight idea as the traffic's park_band, much smaller: pick a
 * random block 6..10 blocks out and take it if it is pavement. A few misses
 * a second are fine - the city is a quarter pavement. */
static int spawn_one(gta_peds *ps, const gta_map *m, int cbx, int cby)
{
    int t, i;

    (void)m;
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (!ps->p[i].alive)
            break;
    if (i == GTA_MAX_PEDS)
        return 0;

    for (t = 0; t < 8; t++) {
        int dx = (int)(rng_next(ps) % 21) - 10;
        int dy = (int)(rng_next(ps) % 21) - 10;
        int bx = cbx + dx, by = cby + dy, d;

        d = dx < 0 ? -dx : dx;
        if ((dy < 0 ? -dy : dy) > d) d = dy < 0 ? -dy : dy;
        if (d < 6)
            continue;                   /* never in plain sight */
        if (bx < 1 || bx > 254 || by < 1 || by > 254)
            continue;
        if (!pavement_at(ps, bx, by, 2))
            continue;
        ps->p[i].x = ((long)bx * 32 + 16) << 16;
        ps->p[i].y = ((long)by * 32 + 16) << 16;
        ps->p[i].layer = 2;
        ps->p[i].angle = (int)(rng_next(ps) & 3) * 64;
        ps->p[i].running = ((int)(rng_next(ps) % PED_RUN_ONE_IN) == 0);
        ps->p[i].remap = GTA_REMAP_PED_LO
                       + (int)(rng_next(ps) % (GTA_REMAP_PED_HI
                                               - GTA_REMAP_PED_LO + 1));
        ps->p[i].frame = (int)(rng_next(ps) & 7);
        ps->p[i].frame_tick = 0;
        ps->p[i].down = 0;
        ps->p[i].stunned = 0;
        ps->p[i].alive = 1;
        ps->stat_spawned++;
        return 1;
    }
    return 0;
}

int gta_peds_drop(gta_peds *ps, long x, long y, int layer, int angle,
                  int remap, int stun)
{
    int i;

    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (!ps->p[i].alive)
            break;
    if (i == GTA_MAX_PEDS)
        return 0;

    ps->p[i].x = x;
    ps->p[i].y = y;
    ps->p[i].layer = layer;
    ps->p[i].angle = angle & 255;
    ps->p[i].running = 0;
    ps->p[i].remap = (remap >= 0) ? remap
        : GTA_REMAP_PED_LO + (int)(rng_next(ps)
              % (GTA_REMAP_PED_HI - GTA_REMAP_PED_LO + 1));
    ps->p[i].frame = 0;
    ps->p[i].frame_tick = 0;
    ps->p[i].down = stun;
    ps->p[i].stunned = 1;
    ps->p[i].alive = 1;
    ps->stat_spawned++;
    return 1;
}

void gta_peds_tick(gta_peds *ps, const gta_map *m, long cam_x, long cam_y)
{
    int cbx = (int)(cam_x >> 21), cby = (int)(cam_y >> 21);
    int i;

    if (!ps->nav)
        return;

    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        int bx, by, d, dbx, dby;

        if (!p->alive)
            continue;

        bx = (int)(p->x >> 21); by = (int)(p->y >> 21);
        dbx = bx - cbx; if (dbx < 0) dbx = -dbx;
        dby = by - cby; if (dby < 0) dby = -dby;
        d = dbx > dby ? dbx : dby;
        if (d > 14) {                   /* the camera has moved on */
            p->alive = 0;
            continue;
        }

        if (p->down > 0) {
            /* A person who was RUN OVER stays down and the slot recycles. One
             * who was pulled out of a car was only stunned: they get up and
             * walk away, which is what makes a carjacking look like one. */
            if (--p->down == 0 && !p->stunned)
                p->alive = 0;           /* the slot respawns elsewhere */
            continue;
        }

        /* THE DECISION AT THE BLOCK CENTRE, the whole of the brain. Within
         * a pixel of the centre: if the block one step ahead is not
         * pavement, turn toward a side that is, else about-face. The order
         * left/right is coin-flipped so a corner crowd does not rotate in
         * formation. */
        {
            long off_x = (p->x >> 16) - ((long)bx * 32 + 16);
            long off_y = (p->y >> 16) - ((long)by * 32 + 16);
            if (off_x >= -1 && off_x <= 1 && off_y >= -1 && off_y <= 1) {
                int dir = (p->angle >> 6) & 3;
                if (!pavement_at(ps, bx + step_dx[dir],
                                 by + step_dy[dir], p->layer)) {
                    int first = (rng_next(ps) & 1) ? 1 : 3;
                    int lturn = (dir + first) & 3;
                    int rturn = (dir + (4 - first)) & 3;
                    if (pavement_at(ps, bx + step_dx[lturn],
                                    by + step_dy[lturn], p->layer))
                        dir = lturn;
                    else if (pavement_at(ps, bx + step_dx[rturn],
                                         by + step_dy[rturn], p->layer))
                        dir = rturn;
                    else
                        dir = (dir + 2) & 3;
                    p->angle = dir * 64;
                }
                /* and sometimes turn a corner anyway, or the whole city
                 * walks its block edges for ever */
                else if ((rng_next(ps) & 15) == 0) {
                    int side = (rng_next(ps) & 1) ? 1 : 3;
                    int nd = (dir + side) & 3;
                    if (pavement_at(ps, bx + step_dx[nd],
                                    by + step_dy[nd], p->layer))
                        p->angle = nd * 64;
                }
            }
        }

        {
            long sp = p->running ? PED_RUN_SPEED : PED_WALK_SPEED;
            p->x += ((long)gta_sin(p->angle) * (sp >> 8)) >> 6;
            p->y += ((long)-gta_cos(p->angle) * (sp >> 8)) >> 6;
        }

        /* The cadence follows the gait, or a walking person's legs blur and a
         * running one's do not keep up. Both cycles are eight frames. */
        if (++p->frame_tick >= (p->running ? PED_RUN_TICKS : PED_WALK_TICKS)) {
            p->frame_tick = 0;
            p->frame = (p->frame + 1) & 7;
        }
    }

    if (ps->spawn_wait > 0) {
        ps->spawn_wait--;
    } else {
        ps->spawn_wait = PED_SPAWN_TICKS;
        spawn_one(ps, m, cbx, cby);
    }
}

int gta_peds_ram(gta_peds *ps, long px, long py, int pface, int phl, int phw,
                 int layer)
{
    int i, hits = 0;
    long fx = gta_sin(pface), fy = -gta_cos(pface);
    long rx = gta_cos(pface), ry = gta_sin(pface);

    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        long dx, dy, along, side;

        if (!p->alive || p->down || p->layer != layer)
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
            p->down = PED_DOWN_TICKS;
            ps->stat_runover++;
            hits++;
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
        /* WHICH FRAME. Walk is 0..7 and run is 8..15 on the same sheet, and
         * a body on the road is frame 42 - `lies_on_floor`. This used to draw
         * the run-over with frame 98, which is `standing_still`: a person
         * standing up, turned sideways, lying in the road. */
        f = p->down ? GTA_PED_LIES_ON_FLOOR
          : (p->running ? GTA_PED_RUN_FIRST : GTA_PED_WALK_FIRST) + p->frame;
        if (f >= ps->ped_count)
            f = 0;
        gta_render_add_sprite_r(v, p->x, p->y, p->layer, p->layer,
                              ps->ped_base + f,
                              (p->angle + GTA_SPRITE_ART_SOUTH) & 255,
                              p->remap);
    }
}
