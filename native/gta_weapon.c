/* Weapons - the projectiles, their flight and their hits. gta_weapon.h says
 * whose rules these are. Portable C89, no floats, no Amiga headers. */
#include <stdio.h>
#include <string.h>

#include "gta_weapon.h"
#include "gta_trig.h"
#include "gta_car.h"
#include "gta_style.h"      /* GTA_SPR_EX - the explosion sprite category */
#include "gta_player.h"     /* GTA_SPRITE_ART_SOUTH, the ground types */

void gta_weapons_init(gta_weapons *w, const gta_tiles *t)
{
    memset(w, 0, sizeof *w);
    /* EVERY SPRITE COMES OUT OF THE GAME'S OWN OBJECT TABLE, resolved once.
     * An object's run of frames starts AT its base sprite: the bullet stays
     * on frame 1 and draws the base, the rocket cycles three, the flame ten.
     * The explosion is not an object at all - it is a sprite category of its
     * own, 48 frames = four quadrants of twelve. */
    w->spr_bullet = gta_tiles_object_sprite(t, GTA_OBJ_BULLET);
    w->spr_rocket = gta_tiles_object_sprite(t, GTA_OBJ_ROCKET);
    w->spr_flame  = gta_tiles_object_sprite(t, GTA_OBJ_FLAME);
    w->spr_splat  = gta_tiles_object_sprite(t, GTA_OBJ_SPLAT);
    w->spr_expl   = gta_tiles_sprite_count(t, GTA_SPR_EX) >= 4 * GTA_EXPL_FRAMES
                  ? gta_tiles_sprite_base(t, GTA_SPR_EX) : -1;
}

int gta_weapons_alive(const gta_weapons *w)
{
    int i, n = 0;
    for (i = 0; i < GTA_MAX_BULLETS; i++)
        n += w->b[i].alive;
    return n;
}

int gta_weapons_cooldown(int weapon)
{
    switch (weapon) {
    case GTA_WEAPON_PISTOL: return GTA_COOL_PISTOL;
    case GTA_WEAPON_MG:     return GTA_COOL_MG;
    case GTA_WEAPON_ROCKET: return GTA_COOL_ROCKET;
    case GTA_WEAPON_FLAME:  return GTA_COOL_FLAME;
    default:                return 0;
    }
}

static void splat(gta_weapons *w, long x, long y, int layer)
{
    gta_splat *s = &w->s[w->splat_next];
    w->splat_next = (w->splat_next + 1) % GTA_MAX_SPLATS;
    s->x = x; s->y = y; s->layer = layer;
    s->ticks = GTA_SPLAT_TICKS;
}

/* THE EXPLOSION. Four quadrant sprites a block across, twelve frames each at
 * two ticks a frame; the damage is dealt once, here, and the sprites are only
 * what it looks like afterwards. */
void gta_weapons_explode(gta_weapons *w, long x, long y, int layer,
                         gta_peds *peds, gta_traffic *tr, gta_score *sc,
                         int by_player)
{
    int i;

    for (i = 0; i < GTA_MAX_EXPLOSIONS; i++)
        if (w->x[i].frame == 0) {
            w->x[i].x = x; w->x[i].y = y; w->x[i].layer = layer;
            w->x[i].frame = 1; w->x[i].tick = 0;
            break;
        }
    w->stat_expl++;

    /* THE PEOPLE: killed outright close in, set alight further out. */
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &peds->p[i];
        long dx, dy;
        if (!p->alive || p->corpse || p->layer != layer)
            continue;
        dx = (p->x - x) >> 16;
        dy = (p->y - y) >> 16;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > GTA_EXPL_BURN_PX || dy > GTA_EXPL_BURN_PX)
            continue;
        if (dx <= GTA_EXPL_KILL_PX && dy <= GTA_EXPL_KILL_PX) {
            gta_peds_kill(peds, i);
            if (sc && by_player)
                gta_score_event(sc, GTA_SCORE_TYPE_CIVILIAN,
                                GTA_SCORE_REASON_BLOWN);
        } else {
            gta_peds_burn(peds, i, x, y);
        }
    }

    /* THE CARS: anything this close is written off. */
    for (i = 0; i < tr->n; i++) {
        gta_car *c = &tr->cars[i];
        long dx, dy;
        if (c->done || c->layer != layer)
            continue;
        dx = (c->x - x) >> 16;
        dy = (c->y - y) >> 16;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx <= GTA_EXPL_CAR_PX && dy <= GTA_EXPL_CAR_PX
            && c->damage < GTA_CAR_WRECKED) {
            /* A blast writes a car off outright; its own fuse then runs and
             * it goes up a moment later, which is the chain reaction. */
            c->damage = GTA_CAR_WRECKED;
            c->dmg_bits |= GTA_DELTA_DMG_MASK;
        }
    }
}

void gta_weapons_wreck_car(gta_weapons *w, const gta_car_info *ci,
                           long cx, long cy, int face, int layer,
                           gta_peds *peds, gta_traffic *tr, gta_score *sc,
                           int by_player)
{
    long fx = gta_sin(face), fy = -gta_cos(face);
    long rx = gta_cos(face), ry = gta_sin(face);
    long hl = gta_car_world_len(ci) / 2, hw = gta_car_world_wid(ci) / 2;
    int q;

    /* The centre first - it is the one that does the damage that matters -
     * and then the four corners, which is what gives a burning car its
     * spread of fire rather than a single ball. */
    gta_weapons_explode(w, cx, cy, layer, peds, tr, sc, by_player);
    for (q = 0; q < 4; q++) {
        long a = (q & 1) ? hl : -hl;
        long b = (q & 2) ? hw : -hw;
        long ex = cx + fx * (a * 4) + rx * (b * 4);
        long ey = cy + fy * (a * 4) + ry * (b * 4);
        gta_weapons_explode(w, ex, ey, layer, peds, tr, sc, by_player);
    }
}

/* THE FUSES, once a tick. A car that has taken a hundred points burns for
 * GTA_CAR_FUSE ticks - long enough to get away from it, or not - and then
 * comes apart. Its driver is dragged out of nothing: the fleet's cars carry
 * no passengers yet, so there is nobody in it to kill. */
static void wreck_sweep(gta_weapons *w, gta_peds *peds, gta_traffic *tr,
                        const gta_tiles *t, gta_score *sc)
{
    int i;
    for (i = 0; i < tr->n; i++) {
        gta_car *c = &tr->cars[i];
        /* ALREADY BURNT IS NOT BURNING. A wreck keeps its hundred points of
         * damage for ever - that is what makes it a wreck - so without this
         * the sweep finds it again on the very next tick, lights the fuse
         * again, and the same car explodes over and over: the log filled with
         * "car 7 blew up" / "car 7 is a write-off" in an endless loop. */
        if (c->done || c->wrecked || c->damage < GTA_CAR_WRECKED)
            continue;
        if (c->fuse == 0) {
            c->fuse = GTA_CAR_FUSE;
            c->dmg_bits |= GTA_DELTA_DMG_MASK;
            printf("gta: car %d (model %d) is a write-off - burning\n",
                   i, c->model);
            continue;
        }
        if (--c->fuse > 0)
            continue;
        printf("gta: car %d (model %d) blew up at (%ld,%ld)\n", i, c->model,
               c->x >> 16, c->y >> 16);
        gta_weapons_wreck_car(w, &t->cars[c->model], c->x, c->y, c->face,
                              c->layer, peds, tr, sc, 0);
        if (sc)
            gta_score_add(sc, 100);
        /* AND THE WRECK STAYS. It used to be `c->done = 1` - deleted where it
         * stood - so blowing a car up REMOVED it from the street instead of
         * blocking the street with it. Now it sits there, burnt, solid and
         * driverless, until it is well off screen; see GTA_WRECK_KEEP_BLOCKS.
         *
         * `abandoned` is what makes the traffic tick leave it alone: every
         * rule that already skips a car nobody is driving skips this one, so
         * there is no second kind of parked car to keep working. */
        c->wrecked   = 1;
        c->abandoned = 1;
        c->speed     = 0;
        c->knock     = 0;
        c->fuse      = 0;
        c->hold      = 0;
    }
}

int gta_weapons_fire(gta_weapons *w, int weapon, long x, long y, int layer,
                     int angle, int running, int owner)
{
    int i;
    gta_bullet *b;
    /* Q14 forward and right-hand vectors. Q14 x 4 is one pixel in 16.16, so
     * one of the original's units - half a pixel - is Q14 x 2. */
    long fx = gta_sin(angle), fy = -gta_cos(angle);
    long rx = gta_cos(angle), ry = gta_sin(angle);

    if (weapon <= GTA_WEAPON_FIST || weapon >= GTA_WEAPON_COUNT)
        return 0;
    for (i = 0; i < GTA_MAX_BULLETS; i++)
        if (!w->b[i].alive)
            break;
    if (i == GTA_MAX_BULLETS)
        return 0;
    b = &w->b[i];
    b->layer = layer;
    b->heading = angle & 255;
    b->owner = owner;
    b->frame = 1;
    b->grace = 0;
    b->alive = 1;

    switch (weapon) {
    case GTA_WEAPON_PISTOL:
    case GTA_WEAPON_MG:
        b->kind = GTA_PROJ_BULLET;
        b->speed = GTA_BULLET_SPEED;
        b->life = GTA_BULLET_LIFE;
        if (!running) {
            b->x = x - fx * 4;          /* 2 units behind the centre */
            b->y = y - fy * 4;
        } else {
            b->x = x - fx * 6 + rx * 8; /* 3 back, 4 to the gun hand */
            b->y = y - fy * 6 + ry * 8;
            b->grace = 2;
        }
        break;
    case GTA_WEAPON_ROCKET:
        b->kind = GTA_PROJ_ROCKET;
        b->speed = GTA_ROCKET_SPEED;
        b->life = 0;                    /* no range limit at all */
        if (!running) {
            b->x = x + fx * 24 + rx * 4;    /* 12 ahead, 2 aside */
            b->y = y + fy * 24 + ry * 4;
        } else {
            b->x = x + fx * 4 + rx * 12;    /* 2 ahead, 6 aside */
            b->y = y + fy * 4 + ry * 12;
            b->grace = 1;
        }
        break;
    default:                            /* GTA_WEAPON_FLAME */
        b->kind = GTA_PROJ_FLAME;
        /* The shooter's own speed plus four: four units running, one
         * standing - the original reads the ped's speed field. */
        b->speed = (running ? 4 : 1) + 4;
        b->life = 0;                    /* the frame count kills it */
        if (!running) {
            b->x = x + fx * 24;             /* 12 ahead */
            b->y = y + fy * 24;
        } else {
            b->x = x + fx * 36 + rx * 12;   /* 18 ahead, 6 aside */
            b->y = y + fy * 36 + ry * 12;
        }
        break;
    }
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
                      gta_traffic *tr, const gta_tiles *t, gta_score *sc)
{
    int i;

    for (i = 0; i < GTA_MAX_SPLATS; i++)
        if (w->s[i].ticks > 0)
            w->s[i].ticks--;

    for (i = 0; i < GTA_MAX_EXPLOSIONS; i++)
        if (w->x[i].frame > 0 && ++w->x[i].tick >= GTA_EXPL_TICKS) {
            w->x[i].tick = 0;
            if (++w->x[i].frame > GTA_EXPL_FRAMES)
                w->x[i].frame = 0;
        }

    wreck_sweep(w, peds, tr, t, sc);

    for (i = 0; i < GTA_MAX_BULLETS; i++) {
        gta_bullet *b = &w->b[i];
        long ox, oy;
        int bx, by, g, pi, ci;

        if (!b->alive)
            continue;
        ox = b->x; oy = b->y;
        b->x += (long)gta_sin(b->heading) * GTA_SPEED_Q14(b->speed);
        b->y -= (long)gta_cos(b->heading) * GTA_SPEED_Q14(b->speed);
        /* THE ACCELERATION, after the step, as the original does it. */
        if (b->kind == GTA_PROJ_ROCKET && b->speed < GTA_ROCKET_SPEED_MAX)
            b->speed += 3;
        else if (b->kind == GTA_PROJ_FLAME && b->speed < 7)
            b->speed += 2;

        bx = (int)(b->x >> 21); by = (int)(b->y >> 21);
        if (bx < 1 || bx > 254 || by < 1 || by > 254) {
            b->alive = 0;
            w->stat_expired++;
            continue;
        }

        if (b->grace > 0) {
            b->grace--;
        } else if (b->kind == GTA_PROJ_FLAME) {
            /* A PUFF DOES NOT STOP ON PEOPLE. It sets alight everybody it
             * passes through and carries on; only a car ends it. */
            int guard = 0;
            while (guard++ < GTA_MAX_PEDS) {
                pi = gta_peds_hit_segment(peds, ox, oy, b->heading,
                                          b->speed / 2 + 2, 2, b->layer,
                                          b->owner);
                if (pi < 0 || peds->p[pi].burn > 0)
                    break;
                gta_peds_burn(peds, pi, ox, oy);
                if (sc && b->owner < 0)
                    gta_score_event(sc, GTA_SCORE_TYPE_CIVILIAN,
                                    GTA_SCORE_REASON_BURNED);
                printf("gta: flame caught ped %d\n", pi);
            }
            ci = car_at(tr, t, b->x, b->y, b->layer);
            if (ci >= 0) {
                tr->cars[ci].damage += 15;
                tr->cars[ci].dmg_bits |= 1UL << gta_car_panel_delta(
                    &t->cars[tr->cars[ci].model], tr->cars[ci].x,
                    tr->cars[ci].y, tr->cars[ci].face, b->x, b->y);
                if (sc && b->owner < 0)
                    gta_score_add(sc, 10);
                b->alive = 0;
                w->stat_car++;
                continue;
            }
        } else {
            /* THE PEDESTRIANS FIRST: the original's box reaches from where
             * the projectile was to a step beyond where it is. */
            pi = gta_peds_hit_segment(peds, ox, oy, b->heading,
                                      b->speed / 2 + 2, 1, b->layer,
                                      b->owner);
            if (pi >= 0) {
                if (b->kind == GTA_PROJ_ROCKET) {
                    gta_weapons_explode(w, b->x, b->y, b->layer, peds, tr, sc,
                            b->owner < 0);
                    printf("gta: rocket burst on ped %d at (%ld,%ld)\n", pi,
                           b->x >> 16, b->y >> 16);
                } else {
                    gta_peds_shoot(peds, pi, b->heading);
                    splat(w, peds->p[pi].x, peds->p[pi].y, b->layer);
                    if (sc && b->owner < 0)
                        gta_score_event(sc, GTA_SCORE_TYPE_CIVILIAN,
                                        GTA_SCORE_REASON_SHOT);
                    printf("gta: bullet %d hit ped %d at (%ld,%ld) from %s\n",
                           i, pi, peds->p[pi].x >> 16, peds->p[pi].y >> 16,
                           peds->p[pi].shot == 2 ? "behind" : "the front");
                }
                b->alive = 0;
                w->stat_ped++;
                continue;
            }
            /* THEN THE CARS. A bullet adds five and is spent; a rocket
             * writes the car off and bursts. Neither pushes it. */
            ci = car_at(tr, t, b->x, b->y, b->layer);
            if (ci >= 0) {
                if (b->kind == GTA_PROJ_ROCKET) {
                    tr->cars[ci].damage = GTA_CAR_WRECKED;
                    tr->cars[ci].dmg_bits |= GTA_DELTA_DMG_MASK;
                    gta_weapons_explode(w, b->x, b->y, b->layer, peds, tr, sc,
                            b->owner < 0);
                    if (sc && b->owner < 0)
                        gta_score_add(sc, 100);
                    printf("gta: rocket burst on car %d (model %d)\n", ci,
                           tr->cars[ci].model);
                } else {
                    tr->cars[ci].damage += 5;
                    tr->cars[ci].dmg_bits |= 1UL << gta_car_panel_delta(
                        &t->cars[tr->cars[ci].model], tr->cars[ci].x,
                        tr->cars[ci].y, tr->cars[ci].face, b->x, b->y);
                    splat(w, b->x, b->y, b->layer);
                    if (sc && b->owner < 0)
                        gta_score_add(sc, 10);
                    printf("gta: bullet %d hit car %d (model %d) at (%ld,%ld),"
                           " damage %d, panels %04lx\n", i, ci,
                           tr->cars[ci].model, b->x >> 16, b->y >> 16,
                           tr->cars[ci].damage, tr->cars[ci].dmg_bits);
                }
                b->alive = 0;
                w->stat_car++;
                continue;
            }
        }

        /* THEN THE BLOCK UNDER IT: a building wall or water stops anything.
         * The original also drops a shot silently at a kerb or slope edge;
         * the port has no edge table yet, so it flies over those. */
        g = gta_nav_ground(gta_nav_at_m(nav, bx, by, b->layer));
        if (g == GTA_GROUND_BUILDING || g == GTA_GROUND_WATER) {
            if (b->kind == GTA_PROJ_ROCKET) {
                gta_weapons_explode(w, b->x, b->y, b->layer, peds, tr, sc, b->owner < 0);
                printf("gta: rocket burst on %s at block (%d,%d,%d)\n",
                       g == GTA_GROUND_WATER ? "water" : "a wall",
                       bx, by, b->layer);
            } else if (b->kind == GTA_PROJ_BULLET) {
                splat(w, b->x, b->y, b->layer);
                printf("gta: bullet %d hit %s at (%ld,%ld) block (%d,%d,%d)\n",
                       i, g == GTA_GROUND_WATER ? "water" : "a wall",
                       b->x >> 16, b->y >> 16, bx, by, b->layer);
            }
            b->alive = 0;
            w->stat_wall++;
            continue;
        }

        /* AGEING. The flame counts frames and dies on the eleventh; a bullet
         * counts ticks; a rocket counts nothing and flies until it hits. */
        if (b->kind == GTA_PROJ_FLAME) {
            if (++b->frame > GTA_FLAME_FRAMES) {
                b->alive = 0;
                w->stat_expired++;
            }
        } else if (b->kind == GTA_PROJ_ROCKET) {
            if (++b->frame > 3)
                b->frame = 1;
        } else if (b->life > 0 && --b->life <= 0) {
            b->alive = 0;
            w->stat_expired++;
        }
    }
}

void gta_weapons_draw_ground(const gta_weapons *w, gta_view *v)
{
    int i;
    if (w->spr_splat < 0)
        return;
    for (i = 0; i < GTA_MAX_SPLATS; i++) {
        const gta_splat *s = &w->s[i];
        if (s->ticks > 0)
            gta_render_add_sprite(v, s->x, s->y, s->layer, s->layer,
                                  w->spr_splat, 0);
    }
}

void gta_weapons_draw_air(const gta_weapons *w, gta_view *v)
{
    int i;

    for (i = 0; i < GTA_MAX_BULLETS; i++) {
        const gta_bullet *b = &w->b[i];
        int spr, ang;
        if (!b->alive)
            continue;
        /* The rocket and the flame are drawn REVERSED - the art points back
         * along the flight - and the bullet along it. */
        switch (b->kind) {
        case GTA_PROJ_ROCKET:
            spr = w->spr_rocket < 0 ? -1 : w->spr_rocket + b->frame - 1;
            ang = (b->heading + 128) & 255;
            break;
        case GTA_PROJ_FLAME:
            spr = w->spr_flame < 0 ? -1 : w->spr_flame + b->frame - 1;
            ang = (b->heading + 128) & 255;
            break;
        default:
            spr = w->spr_bullet;
            ang = b->heading;
            break;
        }
        if (spr >= 0)
            gta_render_add_sprite(v, b->x, b->y, b->layer, b->layer, spr,
                                  (ang + GTA_SPRITE_ART_SOUTH) & 255);
    }

    /* THE EXPLOSION IS FOUR SPRITES, not one: a block-sized quadrant in each
     * corner, each with its own twelve frames further along the category. */
    if (w->spr_expl >= 0)
        for (i = 0; i < GTA_MAX_EXPLOSIONS; i++) {
            const gta_explosion *e = &w->x[i];
            int q;
            if (e->frame == 0)
                continue;
            for (q = 0; q < 4; q++) {
                long qx = e->x + ((q & 1) ? (16L << 16) : -(16L << 16));
                long qy = e->y + ((q & 2) ? (16L << 16) : -(16L << 16));
                /* ANGLE 0, NOT THE ART-SOUTH OFFSET. These four tile into
                 * one fireball, and half a turn puts each quarter in the
                 * opposite corner - which is exactly what the developer saw:
                 * "wybuch od bazooki jest na czterech rogach sprite zamiast
                 * w srodku". An explosion has no facing to convert. */
                gta_render_add_sprite(v, qx, qy, e->layer, e->layer,
                                      w->spr_expl + q * GTA_EXPL_FRAMES
                                      + e->frame - 1, 0);
            }
        }
}
