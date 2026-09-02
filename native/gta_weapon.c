/* Weapons - the pistol's bullet, its flight and its hits. gta_weapon.h says
 * whose rules these are. Portable C89, no floats, no Amiga headers. */
#include <stdio.h>
#include <string.h>

#include "gta_weapon.h"
#include "gta_trig.h"
#include "gta_car.h"
#include "gta_player.h"     /* GTA_SPRITE_ART_SOUTH, the ground types */

void gta_weapons_init(gta_weapons *w)
{
    memset(w, 0, sizeof *w);
}

int gta_weapons_alive(const gta_weapons *w)
{
    int i, n = 0;
    for (i = 0; i < GTA_MAX_BULLETS; i++)
        n += w->b[i].alive;
    return n;
}

static void splat(gta_weapons *w, long x, long y, int layer)
{
    gta_splat *s = &w->s[w->splat_next];
    w->splat_next = (w->splat_next + 1) % GTA_MAX_SPLATS;
    s->x = x; s->y = y; s->layer = layer;
    s->ticks = GTA_SPLAT_TICKS;
}

int gta_weapons_fire_pistol(gta_weapons *w, long x, long y, int layer,
                            int angle, int running, int owner)
{
    int i;
    gta_bullet *b;
    /* Q14 forward and right-hand vectors; Q14 x 4 is one pixel in 16.16, so
     * a unit (half a pixel) is Q14 x 2. */
    long fx = gta_sin(angle), fy = -gta_cos(angle);
    long rx = gta_cos(angle), ry = gta_sin(angle);

    for (i = 0; i < GTA_MAX_BULLETS; i++)
        if (!w->b[i].alive)
            break;
    if (i == GTA_MAX_BULLETS)
        return 0;
    b = &w->b[i];
    if (!running) {
        /* 2 units BEHIND the centre, as the original does */
        b->x = x - fx * 4;
        b->y = y - fy * 4;
        b->grace = 0;
    } else {
        /* 3 back, 4 to the right, and two ticks out of the grid */
        b->x = x - fx * 6 + rx * 8;
        b->y = y - fy * 6 + ry * 8;
        b->grace = 2;
    }
    b->layer = layer;
    b->heading = angle & 255;
    b->life = GTA_BULLET_LIFE;
    b->owner = owner;
    b->alive = 1;
    w->stat_fired++;
    return 1;
}

/* The fleet car whose body (x,y) is inside, on `layer`; -1 for none. The
 * same point-in-box as gta_main.c's car_body_hit, with a pixel of margin. */
static int car_at(const gta_traffic *tr, const gta_tiles *t, long x, long y,
                  int layer)
{
    int i;
    for (i = 0; i < tr->n; i++) {
        const gta_car *c = &tr->cars[i];
        const gta_car_info *ci;
        long fx, fy, rx, ry, dx, dy, along, across;
        if (c->done || c->layer != layer)
            continue;
        dx = (x - c->x) >> 16;
        dy = (y - c->y) >> 16;
        if (dx > 80 || dx < -80 || dy > 80 || dy < -80)
            continue;
        ci = &t->cars[c->model];
        fx = gta_sin(c->face); fy = -gta_cos(c->face);
        rx = gta_cos(c->face); ry = gta_sin(c->face);
        along  = (dx * fx + dy * fy) >> 14;
        across = (dx * rx + dy * ry) >> 14;
        if (along < 0)  along  = -along;
        if (across < 0) across = -across;
        if (along <= gta_car_world_len(ci) / 2 + 1
            && across <= gta_car_world_wid(ci) / 2 + 1)
            return i;
    }
    return -1;
}

void gta_weapons_tick(gta_weapons *w, const gta_nav *nav, gta_peds *peds,
                      gta_traffic *tr, const gta_tiles *t)
{
    int i;

    for (i = 0; i < GTA_MAX_SPLATS; i++)
        if (w->s[i].ticks > 0)
            w->s[i].ticks--;

    for (i = 0; i < GTA_MAX_BULLETS; i++) {
        gta_bullet *b = &w->b[i];
        long ox, oy;
        int bx, by, g, pi, ci;

        if (!b->alive)
            continue;
        ox = b->x; oy = b->y;
        b->x += (long)gta_sin(b->heading) * GTA_BULLET_STEP_Q14;
        b->y -= (long)gta_cos(b->heading) * GTA_BULLET_STEP_Q14;
        bx = (int)(b->x >> 21); by = (int)(b->y >> 21);
        if (bx < 1 || bx > 254 || by < 1 || by > 254) {
            b->alive = 0;
            w->stat_expired++;
            continue;
        }
        if (b->grace > 0) {
            b->grace--;
        } else {
            /* THE PEDESTRIANS FIRST: the original's box is +/- a speed's
             * length around the new position, so it reaches from where the
             * bullet was to a step beyond where it is; two units wide. */
            pi = gta_peds_hit_segment(peds, ox, oy, b->heading,
                                      2 * (GTA_BULLET_STEP_Q14 / 4), 1,
                                      b->layer, b->owner);
            if (pi >= 0) {
                gta_peds_shoot(peds, pi, b->heading);
                splat(w, peds->p[pi].x, peds->p[pi].y, b->layer);
                b->alive = 0;
                w->stat_ped++;
                printf("gta: bullet %d hit ped %d at (%ld,%ld) from %s\n", i, pi,
                       peds->p[pi].x >> 16, peds->p[pi].y >> 16,
                       peds->p[pi].shot == 2 ? "behind" : "the front");
                continue;
            }
            /* THEN THE CARS: +5 damage (the original's +2 for the light
             * classes and the 75..99 wreck are not modelled yet), a splat,
             * and the bullet is spent. It never pushes the car. */
            ci = car_at(tr, t, b->x, b->y, b->layer);
            if (ci >= 0) {
                tr->cars[ci].damage += 5;
                splat(w, b->x, b->y, b->layer);
                b->alive = 0;
                w->stat_car++;
                printf("gta: bullet %d hit car %d (model %d) at (%ld,%ld), damage %d\n",
                       i, ci, tr->cars[ci].model, b->x >> 16, b->y >> 16,
                       tr->cars[ci].damage);
                continue;
            }
        }
        /* THEN THE BLOCK UNDER IT: a building wall or water stops it with a
         * splat. The original also drops it silently at a kerb or slope
         * edge; the port has no edge table yet, so it flies over those. */
        g = gta_nav_ground(gta_nav_at_m(nav, bx, by, b->layer));
        if (g == GTA_GROUND_BUILDING || g == GTA_GROUND_WATER) {
            splat(w, b->x, b->y, b->layer);
            b->alive = 0;
            w->stat_wall++;
            printf("gta: bullet %d hit %s at (%ld,%ld) block (%d,%d,%d)\n", i,
                   g == GTA_GROUND_WATER ? "water" : "a wall",
                   b->x >> 16, b->y >> 16, bx, by, b->layer);
            continue;
        }
        if (--b->life <= 0) {
            b->alive = 0;
            w->stat_expired++;
        }
    }
}

void gta_weapons_draw(const gta_weapons *w, gta_view *v, int bullet_sprite,
                      int splat_sprite)
{
    int i;
    if (splat_sprite >= 0)
        for (i = 0; i < GTA_MAX_SPLATS; i++) {
            const gta_splat *s = &w->s[i];
            if (s->ticks > 0)
                gta_render_add_sprite(v, s->x, s->y, s->layer, s->layer,
                                      splat_sprite, 0);
        }
    if (bullet_sprite >= 0)
        for (i = 0; i < GTA_MAX_BULLETS; i++) {
            const gta_bullet *b = &w->b[i];
            if (b->alive)
                gta_render_add_sprite(v, b->x, b->y, b->layer, b->layer,
                                      bullet_sprite,
                                      (b->heading + GTA_SPRITE_ART_SOUTH) & 255);
        }
}
