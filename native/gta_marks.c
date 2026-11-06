/* Tyre marks and wheel smoke: see gta_marks.h. */
#include <string.h>

#include "gta_marks.h"
#include "gta_trig.h"

#define OBJ_SMOKE         0x0a
#define OBJ_MARK          0x0b
#define MARK_FRAMES       4
#define MARK_FRAME_TICKS  128
#define MARK_LIFE         (MARK_FRAMES * MARK_FRAME_TICKS)
#define SMOKE_FRAMES      11
#define SMOKE_FRAME_TICKS 2
#define SMOKE_LIFE        (SMOKE_FRAMES * SMOKE_FRAME_TICKS)
/* "over the limit": the original's routine refuses at more than 16 and more than 2 */
#define MARK_CELL_MAX     16
#define SMOKE_CELL_MAX    2
#define OBJ_POOL          0x3f
#define OBJ_SPLAT         0x40
#define OBJ_TRAIL         0x4d
#define OBJ_BLOODY        0x0c
#define BLOOD_REACH       (4L << 16)
#define SPLAT_FREE_LIFE   1000
#define OBJ_CLOUD         0x33
#define OBJ_SPLASH        0x36
#define CLOUD_FRAMES      12
#define CLOUD_FRAME_TICKS 2
#define CLOUD_LIFE        (CLOUD_FRAMES * CLOUD_FRAME_TICKS)
#define CLOUD_AT_FRAME    10
#define POOL_FRAMES       12
#define POOL_FRAME_TICKS  24
#define POOL_LIFE         (POOL_FRAMES * POOL_FRAME_TICKS)
#define POOL_SPLAT_AT     (POOL_LIFE - 4 * POOL_FRAME_TICKS)  /* frame 5 */
#define STAINS_PER_FRAME  24
/* one original unit, 64 to a block, in the port's 16.16 px at 32 to a block */
#define ORIG_UNIT         0x8000L
/* The sprite queue is shared with everything else in the frame, and the marks
 * go in first: they must not be able to push a car out of it. */
#define MARKS_PER_FRAME   48
#define DRAW_REACH        (10L << 21)   /* ten blocks from the camera */

#define CELL_OF(c)        ((int)(((c) >> 22) & (GTA_MARK_CELLS - 1)))

void gta_marks_init(gta_marks *g, const gta_tiles *t)
{
    memset(g, 0, sizeof(*g));
    g->spr_mark = gta_tiles_object_sprite(t, OBJ_MARK);
    g->spr_smoke = gta_tiles_object_sprite(t, OBJ_SMOKE);
    g->spr_pool = gta_tiles_object_sprite(t, OBJ_POOL);
    g->spr_splat = gta_tiles_object_sprite(t, OBJ_SPLAT);
    g->spr_trail = gta_tiles_object_sprite(t, OBJ_TRAIL);
    g->spr_cloud = gta_tiles_object_sprite(t, OBJ_CLOUD);
    g->spr_splash = gta_tiles_object_sprite(t, OBJ_SPLASH);
    g->spr_bloody = gta_tiles_object_sprite(t, OBJ_BLOODY);
    g->spr_rubble[0] = gta_tiles_object_sprite(t, 0x2c);
    g->spr_rubble[1] = gta_tiles_object_sprite(t, 0x2d);
    g->spr_rubble[2] = gta_tiles_object_sprite(t, 0x2b);
    g->spr_lump = gta_tiles_object_sprite(t, 0x05);
    g->spr_spark = gta_tiles_object_sprite(t, 0x0d);
}

#define SPARK_LIFE 5            /* frames, one original tick each */

void gta_marks_sparks(gta_marks *g, long x, long y, int layer, int len, int wid,
                      int heading)
{
    static unsigned rnd = 12345u;
    /* across the car (cos, sin) and along it (sin, -cos), Q14 -> 16.16 is 4 */
    long ax = (long)gta_cos(heading) * (wid / 2) * 4, ay = (long)gta_sin(heading) * (wid / 2) * 4;
    long lx = (long)gta_sin(heading) * (len / 2) * 4, ly = -(long)gta_cos(heading) * (len / 2) * 4;
    int k;
    if (g->spr_spark < 0)
        return;
    for (k = 0; k < 4; k++) {
        gta_mark *s = &g->spark[k];
        long sa = (k & 1) ? 1 : -1, sl = (k & 2) ? 1 : -1;
        rnd = rnd * 1103515245u + 12345u;
        s->x = x + sa * ax + sl * lx;
        s->y = y + sa * ay + sl * ly;
        s->layer = (unsigned char)layer;
        s->angle = (unsigned char)(rnd >> 16);   /* % 0x400 of the original */
        s->left = SPARK_LIFE;
        s->sprite = (short)g->spr_spark;
    }
    g->stat_sparks += 4;
}

static void lay_debris(gta_marks *g, long x, long y, int layer, int angle, int sprite)
{
    gta_mark *m;
    if (sprite < 0)
        return;
    m = &g->debris[g->debris_next];
    g->debris_next = (g->debris_next + 1) % GTA_MAX_DEBRIS;
    m->x = x;
    m->y = y;
    m->layer = (unsigned char)layer;
    m->angle = (unsigned char)angle;
    m->left = 1;                /* never counts down: rubble stays */
    m->sprite = (short)sprite;
}

void gta_marks_debris(gta_marks *g, int bx, int by, int layer, int face)
{
    static const int fdx[4] = { -1, 1, 0, 0 }, fdy[4] = { 0, 0, -1, 1 };
    int dx = fdx[face & 3], dy = fdy[face & 3], k;
    /* the face's middle. A block is 0x200000 here, 0x400000 in the
     * original, so every offset of the original's routine is halved. */
    long ex = ((long)bx << 21) + 0x100000L + dx * 0x100000L;
    long ey = ((long)by << 21) + 0x100000L + dy * 0x100000L;
    for (k = 0; k < 3; k++)
        lay_debris(g, ex + dx * 0x20000L, ey + dy * 0x20000L, layer, 0,
                   g->spr_rubble[k]);
    /* 0x200 of the original's 0x400 is half a turn */
    lay_debris(g, ex + dx * 0x100000L, ey + dy * 0x100000L, layer, 128, g->spr_lump);
    lay_debris(g, ex + dx * 0x100000L, ey + dy * 0x100000L, layer, 128, g->spr_lump);
}

/* A free slot, or else the next one round: blood is scenery and the oldest
 * goes first. */
static void lay_stain(gta_marks *g, int kind, long x, long y, int layer, int ped)
{
    int i;
    gta_stain *s;
    for (i = 0; i < GTA_MAX_STAINS; i++) {
        s = &g->st[g->st_next];
        g->st_next = (g->st_next + 1) % GTA_MAX_STAINS;
        if (s->kind == 0)
            break;
    }
    s = i < GTA_MAX_STAINS ? s
                           : &g->st[(g->st_next + GTA_MAX_STAINS - 1) % GTA_MAX_STAINS];
    s->x = x;
    s->y = y;
    s->layer = (unsigned char)layer;
    s->kind = (unsigned char)kind;
    s->ped = (short)ped;
    s->left = kind == OBJ_POOL ? POOL_LIFE : (ped < 0 ? SPLAT_FREE_LIFE : 0);
}

void gta_marks_pool(gta_marks *g, long x, long y, int layer)
{
    lay_stain(g, OBJ_POOL, x, y, layer, -1);
    g->stat_pools++;
}

/* the original's routine: is there blood at this wheel? */
static int blood_at(const gta_marks *g, long x, long y)
{
    int i;
    for (i = 0; i < GTA_MAX_STAINS; i++) {
        const gta_stain *s = &g->st[i];
        long dx, dy;
        if (s->kind != OBJ_POOL && s->kind != OBJ_SPLAT)
            continue;
        dx = s->x - x; if (dx < 0) dx = -dx;
        dy = s->y - y; if (dy < 0) dy = -dy;
        if (dx < BLOOD_REACH && dy < BLOOD_REACH)
            return 1;
    }
    return 0;
}

/* Polled rather than called from the half-dozen places that kill a ped. */
static void blood_tick(gta_marks *g, gta_peds *peds)
{
    int i;
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &peds->p[i];
        if (!p->alive)
            continue;
        if (p->shot && !p->corpse && p->shot_step < GTA_SHOT_STATES - 1)
            lay_stain(g, OBJ_TRAIL, p->x, p->y, p->layer, i);
        if (p->corpse && !p->bled) {
            p->bled = 1;
            lay_stain(g, OBJ_POOL, p->x, p->y, p->layer, i);
            g->stat_pools++;
        }
    }
    for (i = 0; i < GTA_MAX_STAINS; i++) {
        gta_stain *s = &g->st[i];
        if (s->kind == 0)
            continue;
        if (s->kind == OBJ_POOL) {
            if (--s->left == POOL_SPLAT_AT)
                lay_stain(g, OBJ_SPLAT, s->x, s->y, s->layer, s->ped);
            if (s->left == 0)
                s->kind = 0;
        } else {
            if (s->ped < 0) {
                if (--s->left == 0)
                    s->kind = 0;
            } else {
                const gta_ped *p = &peds->p[s->ped];
                if (!p->alive || !(p->corpse || p->shot))
                    s->kind = 0;
            }
        }
    }
}

static void lay_smoke(gta_marks *g, int ground, long x, long y, int layer)
{
    int i, pick = -1, near_ = 0;
    if (g->spr_smoke < 0 || ground != 2)
        return;
    for (i = 0; i < GTA_MAX_SMOKES; i++) {
        const gta_mark *s = &g->s[i];
        if (s->left == 0) {
            if (pick < 0) pick = i;
        } else if (CELL_OF(s->x) == CELL_OF(x) && CELL_OF(s->y) == CELL_OF(y)) {
            near_++;
        }
    }
    if (pick < 0 || near_ > SMOKE_CELL_MAX)
        return;
    g->s[pick].x = x;
    g->s[pick].y = y;
    g->s[pick].layer = (unsigned char)layer;
    g->s[pick].left = SMOKE_LIFE;
    g->stat_smoke++;
}

static void lay_mark(gta_marks *g, int nav_byte, long x, long y, int layer,
                     int angle, int sprite)
{
    int ground = gta_nav_ground(nav_byte), i;
    unsigned char *c;
    if (g->spr_mark < 0 || (nav_byte & 0x80))
        return;
    if (ground < 2 || ground > 4)
        return;
    if (g->count >= GTA_MAX_MARKS)
        return;
    c = &g->cell[CELL_OF(y) * GTA_MARK_CELLS + CELL_OF(x)];
    if (*c > MARK_CELL_MAX)
        return;
    for (i = 0; i < GTA_MAX_MARKS; i++) {
        gta_mark *m = &g->m[g->next];
        g->next = (g->next + 1) % GTA_MAX_MARKS;
        if (m->left == 0) {
            m->x = x;
            m->y = y;
            m->layer = (unsigned char)layer;
            m->angle = (unsigned char)angle;
            m->left = MARK_LIFE;
            m->sprite = (short)sprite;
            (*c)++;
            g->count++;
            return;
        }
    }
}

static void lay_at(gta_marks *g, const gta_nav *nav, long x, long y, int layer,
                   int angle)
{
    int b = gta_nav_at_m(nav, (int)(x >> 21), (int)(y >> 21), layer);
    int sprite = g->spr_mark;
    lay_smoke(g, gta_nav_ground(b), x, y, layer);
    if (g->bloody > 0) {
        g->bloody--;
        if (g->spr_bloody >= 0)
            sprite = g->spr_bloody;
    }
    /* The original's random 12..21: the position's low bits are as random as
     * anything a mark needs. */
    if (blood_at(g, x, y))
        g->bloody = 12 + (int)((unsigned long)(x ^ y) >> 12) % 10;
    lay_mark(g, b, x, y, layer, angle, sprite);
}

void gta_marks_slide(gta_marks *g, const gta_nav *nav, long x, long y, int layer,
                     int wid, int heading, long vx, long vy, int single)
{
    int angle;
    long off;
    if (!g->half)
        return;                 /* the original's 25 Hz */
    /* The mark points the way the car is GOING, which in a slide is not the
     * way it faces. */
    angle = (vx == 0 && vy == 0) ? heading
                                 : (int)(gta_dir16(vx, vy) >> 16);
    if (single) {
        lay_at(g, nav, x, y, layer, angle);
        return;
    }
    /* a quarter of the width, along the axis across the car: (cos, sin) of
     * the heading in this port's convention. Q14 -> 16.16 is a factor of 4. */
    off = (long)(wid / 4) * 4;
    lay_at(g, nav, x + gta_cos(heading) * off, y + gta_sin(heading) * off,
           layer, angle);
    lay_at(g, nav, x - gta_cos(heading) * off, y - gta_sin(heading) * off,
           layer, angle);
}

/* An explosion's frame turns over on a tick of its own, and `tick == 0` is true
 * for exactly one of the port's ticks of each frame. */
static void lay_cloud(gta_marks *g, long x, long y, int layer, int sprite,
                      int drift)
{
    int k;
    for (k = 0; k < GTA_MAX_CLOUDS; k++)
        if (g->cloud[k].left == 0) {
            g->cloud[k].x = x;
            g->cloud[k].y = y;
            g->cloud[k].layer = (unsigned char)layer;
            g->cloud[k].left = CLOUD_LIFE;
            g->cloud[k].sprite = (short)sprite;
            g->cloud[k].angle = (unsigned char)drift;
            break;
        }
}

void gta_marks_splash(gta_marks *g, long x, long y, int layer)
{
    if (g->spr_splash >= 0)
        lay_cloud(g, x, y, layer, g->spr_splash, 0);
}

static void cloud_poll(gta_marks *g, const gta_weapons *w)
{
    int i;
    for (i = 0; i < GTA_MAX_EXPLOSIONS; i++) {
        const gta_explosion *e = &w->x[i];
        if (e->frame == CLOUD_AT_FRAME && e->tick == 0 && g->spr_cloud >= 0)
            lay_cloud(g, e->x, e->y, e->layer, g->spr_cloud, 1);
    }
}

void gta_marks_tick(gta_marks *g, const gta_nav *nav, gta_peds *peds,
                    const gta_weapons *w)
{
    int i;
    cloud_poll(g, w);
    g->half ^= 1;
    if (!g->half)
        return;
    blood_tick(g, peds);
    for (i = 0; i < GTA_MAX_FIRES; i++) {
        const gta_fire *f = &w->f[i];
        if (f->ticks > 0 && f->frame == 0)
            lay_smoke(g, gta_nav_ground(gta_nav_at_m(nav, (int)(f->x >> 21),
                      (int)(f->y >> 21), f->layer)), f->x, f->y, f->layer);
    }
    for (i = 0; i < 8; i++)
        if (g->spark[i].left > 0)
            g->spark[i].left--;
    for (i = 0; i < GTA_MAX_CLOUDS; i++) {
        gta_mark *c = &g->cloud[i];
        if (c->left > 0) {
            c->left--;
            if (c->angle) {
                c->x += ORIG_UNIT;
                c->y -= ORIG_UNIT;
            }
        }
    }
    for (i = 0; i < GTA_MAX_MARKS; i++) {
        gta_mark *m = &g->m[i];
        if (m->left > 0 && --m->left == 0) {
            unsigned char *c = &g->cell[CELL_OF(m->y) * GTA_MARK_CELLS
                                        + CELL_OF(m->x)];
            if (*c > 0) (*c)--;
            g->count--;
        }
    }
    for (i = 0; i < GTA_MAX_SMOKES; i++) {
        gta_mark *s = &g->s[i];
        if (s->left > 0) {
            s->left--;
            s->x += ORIG_UNIT;
            s->y -= ORIG_UNIT;
        }
    }
}

void gta_marks_draw_air(const gta_marks *g, gta_view *v)
{
    int i;
    for (i = 0; i < GTA_MAX_CLOUDS; i++) {
        const gta_mark *c = &g->cloud[i];
        if (c->left > 0)
            gta_render_add_sprite(v, c->x, c->y, c->layer, c->layer,
                c->sprite + (CLOUD_LIFE - c->left) / CLOUD_FRAME_TICKS, 0);
    }
}

static int near_camera(const gta_view *v, long x, long y)
{
    long dx = x - v->cam_x, dy = y - v->cam_y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= DRAW_REACH && dy <= DRAW_REACH;
}

void gta_marks_draw(const gta_marks *g, gta_view *v)
{
    int i, n, shown = 0;
    for (i = 0; i < 8; i++) {
        const gta_mark *s = &g->spark[i];
        if (s->left > 0 && near_camera(v, s->x, s->y))
            gta_render_add_sprite(v, s->x, s->y, s->layer, s->layer,
                                  s->sprite + (SPARK_LIFE - s->left), s->angle);
    }
    for (i = 0; i < GTA_MAX_DEBRIS; i++) {
        const gta_mark *d = &g->debris[i];
        if (d->left > 0 && near_camera(v, d->x, d->y))
            gta_render_add_sprite(v, d->x, d->y, d->layer, d->layer,
                                  d->sprite, d->angle);
    }
    /* The big splats first: the pool that spawned one lies on top of it. */
    for (n = 0; n < 2; n++)
        for (i = 0; i < GTA_MAX_STAINS; i++) {
            const gta_stain *s = &g->st[i];
            int spr;
            if (s->kind == 0 || (s->kind == OBJ_POOL) != n
                || !near_camera(v, s->x, s->y) || shown >= STAINS_PER_FRAME)
                continue;
            spr = s->kind == OBJ_POOL ? g->spr_pool
                  + (POOL_LIFE - s->left) / POOL_FRAME_TICKS
                : s->kind == OBJ_SPLAT ? g->spr_splat : g->spr_trail;
            if (g->spr_pool >= 0 && g->spr_splat >= 0 && g->spr_trail >= 0) {
                shown++;
                gta_render_add_sprite(v, s->x, s->y, s->layer, s->layer, spr, 0);
            }
        }
    shown = 0;
    /* Newest first, so a full budget drops the faintest. */
    if (g->spr_mark >= 0 && g->count > 0) {
        i = g->next;
        for (n = 0; n < GTA_MAX_MARKS; n++) {
            const gta_mark *m;
            i = (i + GTA_MAX_MARKS - 1) % GTA_MAX_MARKS;
            m = &g->m[i];
            if (m->left > 0 && near_camera(v, m->x, m->y)) {
                if (shown >= MARKS_PER_FRAME)
                    break;
                shown++;
                gta_render_add_sprite(v, m->x, m->y, m->layer, m->layer,
                    m->sprite + (MARK_LIFE - m->left) / MARK_FRAME_TICKS,
                    m->angle);
            }
        }
    }
    if (g->spr_smoke >= 0) {
        for (i = 0; i < GTA_MAX_SMOKES; i++) {
            const gta_mark *s = &g->s[i];
            if (s->left > 0 && near_camera(v, s->x, s->y))
                gta_render_add_sprite(v, s->x, s->y, s->layer, s->layer,
                    g->spr_smoke + (SMOKE_LIFE - s->left) / SMOKE_FRAME_TICKS,
                    0);
        }
    }
}
