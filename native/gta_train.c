/* The trains. Read gta_train.h, then the research notes
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <string.h>

#include "gta_train.h"
#include "gta_trig.h"
#include "gta_style.h"

#define FP 16
#define BLK 32                              /* port px a block */
#define CAR_PITCH 63                        /* 126 original px */
#define BOGIE_IN  10                        /* 20 original px in from each end */
#define BOGIE_GAP (CAR_PITCH - 2 * BOGIE_IN)

/* ---- the walker (TRAINS.md 2.2) -------------------------------------- */

/* The twelve step codes, in port terms: dl is the change of PORT layer,
 * +1 = up (the original's z-1). */
static const signed char st_dx[13] = { 0, 1, 1, 1, -1, -1, -1, 0, 0, 0, 0, 0, 0 };
static const signed char st_dy[13] = { 0, 0, 0, 0, 0, 0, 0, -1, -1, -1, 1, 1, 1 };
static const signed char st_dl[13] = { 0, 1, -1, 0, 1, -1, 0, 1, -1, 0, 1, -1, 0 };
/* Candidate order per heading group; the LAST rail one wins. */
static const unsigned char cand[4][9] = {
    { 11, 8, 10, 7, 9, 12, 1, 2, 3 },       /* +x */
    { 8, 11, 7, 10, 12, 9, 4, 5, 6 },       /* -x */
    { 5, 2, 4, 1, 3, 6, 7, 8, 9 },          /* -y */
    { 5, 2, 4, 1, 3, 6, 10, 11, 12 }        /* +y */
};

static int rail_at(const gta_map *m, int x, int y, int l, int *hint)
{
    gta_block b;
    if (x < 0 || x > 255 || y < 0 || y > 255 || l < 0 || l >= GTA_MAP_LAYERS)
        return 0;
    if (!gta_map_block(m, x, y, l, &b) || !gta_block_is_railway(&b))
        return 0;
    if (hint) *hint = gta_block_traffic_hint(&b);
    return 1;
}

static int seen(const gta_trains *ts, int x, int y, int l)
{
    int i;
    for (i = 0; i < ts->n_nodes; i++)
        if (ts->node[i].x == x && ts->node[i].y == y && ts->node[i].layer == l)
            return 1;
    return 0;
}

/* the original's routine: 0 ok (back at the start), 1 dead end / overflow. */
static int walk(gta_trains *ts, int sx, int sy, int sl)
{
    int x = sx, y = sy, l = sl, head = 3, hint = 0, guard = 0;
    rail_at(ts->map, x, y, l, &hint);
    for (;;) {
        int g, k, best = 0;
        if (ts->n_nodes >= GTA_TRACK_MAX)
            return 1;
        ts->node[ts->n_nodes].x = (unsigned char)x;
        ts->node[ts->n_nodes].y = (unsigned char)y;
        ts->node[ts->n_nodes].layer = (unsigned char)l;
        ts->node[ts->n_nodes].hint = (unsigned char)hint;
        ts->n_nodes++;
        g = head <= 3 ? 0 : head <= 6 ? 1 : head <= 9 ? 2 : 3;
        for (k = 0; k < 9; k++) {
            int c = cand[g][k];
            if (rail_at(ts->map, x + st_dx[c], y + st_dy[c], l + st_dl[c], 0))
                best = c;
        }
        if (!best)
            return 1;
        x += st_dx[best]; y += st_dy[best]; l += st_dl[best];
        head = best;
        rail_at(ts->map, x, y, l, &hint);
        if (x == sx && y == sy && l == sl)
            return 0;
        if (++guard > GTA_TRACK_MAX)
            return 1;
    }
}

static unsigned long ts_rand(unsigned long *seed)
{
    *seed = *seed * 1664525UL + 1013904223UL;
    return *seed >> 16;
}
static unsigned long g_seed = 0x5eedUL;

/* ---- geometry ---------------------------------------------------------- */

static long loop_px(const gta_trains *ts, int loop)
{
    return ts->loop_total[loop];
}

static int node_dir(const gta_trains *ts, int loop, int k, int *dx, int *dy)
{
    int n = ts->loop_len[loop], f = ts->loop_first[loop];
    const gta_track_node *a = &ts->node[f + ((k % n) + n) % n];
    const gta_track_node *b = &ts->node[f + (((k + 1) % n) + n) % n];
    *dx = b->x - a->x; *dy = b->y - a->y;
    if (*dx > 1) *dx = 1;
    if (*dx < -1) *dx = -1;
    if (*dy > 1) *dy = 1;
    if (*dy < -1) *dy = -1;
    return 0;
}

/* Build the loop's pieces (see gta_track_piece). 0 if it has no corner. */
static int build_path(gta_trains *ts, int loop)
{
    int n = ts->loop_len[loop], f = ts->loop_first[loop];
    static short cn[GTA_TRACK_MAX], cr[GTA_TRACK_MAX];
    int nc = 0, k, i;
    long s0 = 0;
    for (i = 0; i < n && nc < GTA_TRACK_MAX; i++) {
        int ax, ay, bx, by;
        node_dir(ts, loop, i - 1, &ax, &ay);
        node_dir(ts, loop, i, &bx, &by);
        if (ax != bx || ay != by) {
            int hp = ts->node[f + (i + n - 1) % n].hint;
            int hn = ts->node[f + (i + 1) % n].hint;
            cn[nc] = (short)i;
            cr[nc] = (short)((hp == 4 || hp == 5 || hn == 4 || hn == 5) ? 48 : 16);
            nc++;
        }
    }
    if (nc == 0)
        return 0;
    /* two corners closer than their radii allow share the room between */
    for (k = 0; k < nc; k++) {
        int k2 = (k + 1) % nc;
        int d = ((cn[k2] - cn[k] + n) % n) * BLK;
        if (nc == 1) d = n * BLK;
        if (cr[k] + cr[k2] > d) {
            if (cr[k] > d / 2) cr[k] = (short)(d / 2);
            if (cr[k2] > d / 2) cr[k2] = (short)(d / 2);
        }
    }
    ts->loop_piece0[loop] = (short)ts->n_pieces;
    for (k = 0; k < nc && ts->n_pieces + 2 <= GTA_TRACK_PIECES; k++) {
        int k2 = (k + 1) % nc, i2 = cn[k2];
        int dx, dy, d1x, d1y;
        long cx = ((long)ts->node[f + cn[k]].x * BLK + 16) << FP;
        long cy = ((long)ts->node[f + cn[k]].y * BLK + 16) << FP;
        int dist = ((cn[k2] - cn[k] + n) % n) * BLK;
        gta_track_piece *pl = &ts->piece[ts->n_pieces++];
        gta_track_piece *pa;
        if (nc == 1) dist = n * BLK;
        node_dir(ts, loop, cn[k], &dx, &dy);       /* out of corner k */
        /* the straight from corner k's way out to corner k2's way in */
        pl->arc = 0;
        pl->x0 = cx + ((long)dx * cr[k] << FP);
        pl->y0 = cy + ((long)dy * cr[k] << FP);
        pl->d1x = (signed char)dx; pl->d1y = (signed char)dy;
        pl->len = (long)(dist - cr[k] - cr[k2]) << FP;
        pl->s0 = s0;
        pl->node = (short)cn[k];
        pl->r_from = cr[k];
        for (i = 1; i < (cn[k2] - cn[k] + n) % n || (nc == 1 && i < n); i++) {
            int nd = (cn[k] + i) % n;
            ts->node_s[f + nd] = s0 + ((long)(i * BLK - cr[k]) << FP);
        }
        s0 += pl->len;
        /* corner k2's quarter circle */
        node_dir(ts, loop, i2 - 1, &d1x, &d1y);
        node_dir(ts, loop, i2, &dx, &dy);
        pa = &ts->piece[ts->n_pieces++];
        pa->arc = 1;
        pa->x0 = ((long)ts->node[f + i2].x * BLK + 16) << FP;
        pa->y0 = ((long)ts->node[f + i2].y * BLK + 16) << FP;
        pa->d1x = (signed char)d1x; pa->d1y = (signed char)d1y;
        pa->d2x = (signed char)dx; pa->d2y = (signed char)dy;
        pa->r = cr[k2];
        pa->len = ((long)cr[k2] * 25736L >> 14) << FP;   /* r * pi/2 */
        if (pa->len <= 0) pa->len = 1L << FP;
        pa->s0 = s0;
        pa->node = (short)i2;
        ts->node_s[f + i2] = s0 + pa->len / 2;
        s0 += pa->len;
    }
    ts->loop_npieces[loop] = (short)(ts->n_pieces - ts->loop_piece0[loop]);
    ts->loop_total[loop] = s0;
    return 1;
}

/* The point at `s` along the loop, its layer and the block it is in. */
static void point_at(const gta_trains *ts, int loop, long s, long *x, long *y,
                     int *layer, int *node)
{
    long len = ts->loop_total[loop], u;
    int lo = ts->loop_piece0[loop], hi = lo + ts->loop_npieces[loop] - 1;
    int f = ts->loop_first[loop], n = ts->loop_len[loop], nd;
    const gta_track_piece *p;
    while (s < 0) s += len;
    while (s >= len) s -= len;
    while (lo < hi) {                       /* the last piece starting <= s */
        int mid = (lo + hi + 1) >> 1;
        if (ts->piece[mid].s0 <= s) lo = mid; else hi = mid - 1;
    }
    p = &ts->piece[lo];
    u = s - p->s0;
    if (!p->arc) {
        *x = p->x0 + (long)p->d1x * u;
        *y = p->y0 + (long)p->d1y * u;
        nd = (p->node + (int)(((u >> FP) + p->r_from + 16) / BLK)) % n;
    } else {
        /* phi 0..64 (a quarter of the 256 circle) along the arc */
        long ph = ((u >> 8) * 64L) / ((p->len >> 8) ? (p->len >> 8) : 1);
        int phi = (int)(ph > 64 ? 64 : ph);
        long c = gta_cos(phi), sn = gta_sin(phi);
        long R = p->r;
        long ox = p->x0 + ((long)(p->d2x - p->d1x) * R << FP);
        long oy = p->y0 + ((long)(p->d2y - p->d1y) * R << FP);
        *x = ox + ((-(long)p->d2x * R * c + (long)p->d1x * R * sn) << 2);
        *y = oy + ((-(long)p->d2y * R * c + (long)p->d1y * R * sn) << 2);
        nd = p->node;
    }
    if (layer) *layer = ts->node[f + nd].layer;
    if (node) *node = f + nd;
}

static void bogies(const gta_trains *ts, const gta_train *t, int j,
                   long *fx, long *fy, long *bx, long *by, int *layer)
{
    long s = t->s - ((long)j * CAR_PITCH << FP);
    point_at(ts, t->loop, s, fx, fy, layer, 0);
    point_at(ts, t->loop, s - ((long)BOGIE_GAP << FP), bx, by, 0, 0);
}

void gta_trains_body(const gta_trains *ts, int ti, int j, long *x, long *y,
                     int *layer, int *angle)
{
    long fx, fy, bx, by;
    const gta_train *t = &ts->t[ti];
    bogies(ts, t, j, &fx, &fy, &bx, &by, layer);
    *x = fx + ((bx - fx) >> 1);
    *y = fy + ((by - fy) >> 1);
    if (angle) {
        if (fx == bx && fy == by) *angle = 0;
        else *angle = (int)((gta_dir16((fx - bx) >> 8, (fy - by) >> 8) >> 16) & 255);
    }
}

/* ---- load ------------------------------------------------------------ */

static int station_index(const gta_trains *ts, int node)
{
    int k;
    for (k = 0; k < ts->n_st; k++)
        if (ts->st_node[k] == node) return k;
    return -1;
}

int gta_trains_init(gta_trains *ts, const gta_map *m, const gta_tiles *t)
{
    int x, y, z, i;
    memset(ts, 0, sizeof *ts);
    ts->map = m;
    ts->tiles = t;
    ts->pl_on = -1;
    ts->spr_base = gta_tiles_sprite_base(t, GTA_SPR_TRAIN);
    ts->spr_count = gta_tiles_sprite_count(t, GTA_SPR_TRAIN);

    /* the original's routine: top-left first, the original's z (from the top) inner */
    for (y = 0; y < 256; y++)
        for (x = 0; x < 256; x++)
            for (z = 0; z < GTA_MAP_LAYERS; z++) {
                int l = GTA_MAP_LAYERS - 1 - z, first = ts->n_nodes;
                if (!rail_at(m, x, y, l, 0) || seen(ts, x, y, l))
                    continue;
                if (ts->n_loops >= GTA_TRACK_LOOPS || walk(ts, x, y, l)) {
                    printf("gta: trains - the line at (%d,%d) L%d does not close:"
                           " no trains on this map\n", x, y, l);
                    ts->n_loops = 0;
                    ts->n = 0;
                    return 0;
                }
                ts->loop_first[ts->n_loops] = (short)first;
                ts->loop_len[ts->n_loops] = (short)(ts->n_nodes - first);
                ts->n_loops++;
            }

    /* the bogies' path round each loop */
    for (i = 0; i < ts->n_loops; i++)
        if (!build_path(ts, i)) {
            printf("gta: trains - loop %d has no corner: no trains\n", i);
            ts->n_loops = 0;
            ts->n = 0;
            return 0;
        }

    /* the stations, in walk order, and a train at every hint 7 */
    for (i = 0; i < ts->n_loops; i++) {
        int k;
        for (k = 0; k < ts->loop_len[i]; k++) {
            int nd = ts->loop_first[i] + k;
            if ((ts->node[nd].hint == 6 || ts->node[nd].hint == 7) &&
                ts->n_st < GTA_STATION_MAX) {
                ts->st_node[ts->n_st] = (short)nd;
                ts->st_loop[ts->n_st] = (unsigned char)i;
                ts->n_st++;
            }
        }
    }
    for (i = 0; i < ts->n_st && ts->n < GTA_TRAIN_MAX; i++) {
        int nd = ts->st_node[i], l = ts->st_loop[i];
        gta_train *tr = &ts->t[ts->n];
        int g;
        if (ts->node[nd].hint != 7)
            continue;
        memset(tr, 0, sizeof *tr);
        g = gta_map_ground_type(m, ts->node[nd].x, ts->node[nd].y,
                                ts->node[nd].layer);
        tr->type = (g == 2) ? 2 : 1;        /* on a road: a tram */
        tr->loop = (unsigned char)l;
        /* centred on the station block's entry edge (3.1) */
        tr->s = ts->node_s[nd] + ((long)(2 * CAR_PITCH - BOGIE_IN - 16) << FP);
        if (tr->s >= loop_px(ts, l)) tr->s -= loop_px(ts, l);
        tr->state = 2;
        tr->sub = 0;
        tr->control = 1;
        tr->sense = 6;
        tr->door = 8;
        tr->max = tr->type == 2 ? 30 : 60;
        tr->cur_st = (short)i;
        tr->next_st = (short)i;
        tr->last_st_node = (short)nd;
        ts->st_busy[i] = 1;
        {
            long px_, py_;
            point_at(ts, l, tr->s, &px_, &py_, 0, &tr->lead_node);
        }
        ts->n++;
    }
    printf("gta: trains - %d loop%s, %d nodes, %d stations, %d trains\n",
           ts->n_loops, ts->n_loops == 1 ? "" : "s", ts->n_nodes, ts->n_st,
           ts->n);
    for (i = 0; i < ts->n; i++) {
        long bx, by;
        int bl, ba;
        gta_trains_body(ts, i, 0, &bx, &by, &bl, &ba);
        printf("gta: trains - %s %d on loop %d at (%ld,%ld) L%d\n",
               ts->t[i].type == 2 ? "tram" : "train", i, ts->t[i].loop,
               bx >> (FP + 5), by >> (FP + 5), bl);
    }
    fflush(stdout);
    return ts->n;
}

/* ---- the tick ------------------------------------------------------- */

static int next_station(const gta_trains *ts, int st)
{
    int k = st + 1;
    if (k < ts->n_st && ts->st_loop[k] == ts->st_loop[st])
        return k;
    for (k = 0; k < ts->n_st; k++)
        if (ts->st_loop[k] == ts->st_loop[st])
            return k;
    return st;
}

static void brake_to(gta_train *t, int st)
{
    t->after_brake = (unsigned char)st;
    t->state = 4;
}

static int record_of_model(const gta_tiles *t, int model)
{
    int i;
    for (i = 0; i < t->n_cars; i++)
        if (t->cars[i].model_id == model) return i;
    return -1;
}

void gta_trains_wreck(gta_trains *ts, int ti, gta_traffic *tr,
                      void (*sound)(void *ctx, int id, long x, long y),
                      void *ctx)
{
    gta_train *t = &ts->t[ti];
    int j, rec = record_of_model(ts->tiles, t->type == 2 ? 12 : 11);
    int cars = t->type == 2 ? 1 : GTA_TRAIN_CARS;
    if (t->wrecked)
        return;
    t->wrecked = 1;
    for (j = 0; j < cars; j++) {
        long x, y;
        int l, a;
        gta_trains_body(ts, ti, j, &x, &y, &l, &a);
        if (rec >= 0 && tr)
            gta_traffic_abandon(tr, rec, x, y, a, l, -1, 100);
        if (sound) sound(ctx, 10, x, y);
    }
    t->speed = 0;
    t->state = 11;
    t->control = 1;
    printf("gta: trains - train %d wrecked\n", ti);
    fflush(stdout);
}

static void crush(gta_trains *ts, int ti, gta_traffic *tr, gta_peds *ps,
                  long pl_x, long pl_y, int pl_layer, int pl_on_foot,
                  void (*sound)(void *ctx, int id, long x, long y), void *ctx)
{
    gta_train *t = &ts->t[ti];
    int j, b, k, cars = t->type == 2 ? 1 : GTA_TRAIN_CARS;
    for (j = 0; j < cars; j++) {
        long px[2], py[2];
        int l;
        bogies(ts, t, j, &px[0], &py[0], &px[1], &py[1], &l);
        for (b = 0; b < 2; b++) {
            /* another train's bogie: both wrecked (7.1) */
            for (k = 0; k < ts->n; k++) {
                int jj;
                if (k == ti || ts->t[k].wrecked) continue;
                for (jj = 0; jj < GTA_TRAIN_CARS; jj++) {
                    long ox, oy, qx, qy, dx, dy;
                    int ol;
                    bogies(ts, &ts->t[k], jj, &ox, &oy, &qx, &qy, &ol);
                    if (ol != l) continue;
                    dx = (ox - px[b]) >> FP; dy = (oy - py[b]) >> FP;
                    if (dx < 0) dx = -dx;
                    if (dy < 0) dy = -dy;
                    if (dx < 8 && dy < 8) {
                        gta_trains_wreck(ts, ti, tr, sound, ctx);
                        gta_trains_wreck(ts, k, tr, sound, ctx);
                        return;
                    }
                }
            }
            /* a car: +20 damage, and the train halves its speed */
            if (tr) for (k = 0; k < tr->n; k++) {
                gta_car *c = &tr->cars[k];
                long dx, dy;
                if (c->done || c->layer != l) continue;
                dx = (c->x - px[b]) >> FP; dy = (c->y - py[b]) >> FP;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx > 12 || dy > 12) continue;
                if (!c->invuln && c->damage < 100) {
                    c->damage += 20;
                    if (c->damage > 100) c->damage = 100;
                }
                t->speed /= 2;
                ts->stat_crush++;
            }
            /* people: run over above 1 px a frame */
            if (t->speed / 10 > 1) {
                if (ps) for (k = 0; k < GTA_MAX_PEDS; k++) {
                    gta_ped *p = &ps->p[k];
                    long dx, dy;
                    if (!p->alive || p->corpse || p->layer != l) continue;
                    dx = (p->x - px[b]) >> FP; dy = (p->y - py[b]) >> FP;
                    if (dx < 0) dx = -dx;
                    if (dy < 0) dy = -dy;
                    if (dx < 10 && dy < 10) {
                        gta_peds_kill(ps, k);
                        ts->stat_runover++;
                    }
                }
                if (pl_on_foot && pl_layer == l) {
                    long dx = (pl_x - px[b]) >> FP, dy = (pl_y - py[b]) >> FP;
                    if (dx < 0) dx = -dx;
                    if (dy < 0) dy = -dy;
                    if (dx < 10 && dy < 10) ts->pl_hit = 1;
                }
            }
        }
    }
}

static void one_train(gta_trains *ts, int ti, gta_traffic *tr, gta_peds *ps,
                      long pl_x, long pl_y, int pl_layer, int pl_on_foot,
                      void (*sound)(void *ctx, int id, long x, long y),
                      void *ctx, int logic)
{
    gta_train *t = &ts->t[ti];
    long step;
    int node;

    if (t->type == 2 || t->wrecked)     /* trams never move (TRAINS.md 9) */
        return;
    if (!logic)
        goto move;

    switch (t->state) {
    case 2:                             /* the station stop */
        switch (t->sub) {
        case 5: t->door = 1; t->sub = 1; break;
        case 1:
            if (t->door < 7) t->door++;
            else { t->sub = 3; t->sub_first = 1; }
            if (sound) {
                long x, y;
                gta_trains_body(ts, ti, 0, &x, &y, 0, 0);
                if ((t->door & 1) == 0) sound(ctx, 0x29, x, y);
            }
            break;
        case 3:
            if (t->sub_first) {
                t->drop = (short)(ts_rand(&g_seed) % 16);
                t->wait = 100;
                t->sub_first = 0;
            } else if (t->drop == 0 || --t->wait <= 0) {
                t->sub = 2; t->sub_first = 1;
            } else if ((t->wait % 5) == 0) {
                t->drop--;      /* a passenger off - the port drops no ped */
            }
            break;
        case 2:
            if (t->sub_first) { t->wait = 100; t->sub_first = 0; }
            else if (--t->wait <= 0) t->sub = 4;
            break;
        case 4:
            if (t->door >= 2) t->door--;
            else { t->door = 8; t->sub = 0; }
            break;
        default:                        /* 0 */
            t->state = 10;
            t->next_st = (short)next_station(ts, t->cur_st);
            t->sub = 5;
            ts->stat_stops++;
            break;
        }
        break;
    case 3:
        if (t->speed < t->max) t->speed++;
        if (t->speed > t->max) t->speed = t->max;
        break;
    case 4:
        if (--t->speed <= 0) { t->speed = 0; t->state = t->after_brake; }
        break;
    case 10:                            /* the block signal */
        if (t->next_st == t->cur_st || !ts->st_busy[t->next_st]) {
            ts->st_busy[t->cur_st] = 0;
            ts->st_busy[t->next_st] = 1;
            t->cur_st = t->next_st;
            t->state = 3;
        }
        break;
    case 12:
        t->state = 2; t->sub = 5;
        break;
    default:                            /* 11: stopped, driven */
        break;
    }
    if (t->state != 2 && t->door < 8) {
        if (--t->door < 2) t->door = 8;
    }

move:
    /* MOVE: speed/10 original px a frame = half that here (4.1), spread
     * over the two 50 Hz ticks of that frame - moved only on the frame's
     * tick it went in 3 px jumps ("skokowo dziala ten pociag") */
    step = ((long)(t->speed / 10)) << (FP - 2);
    if (step) {
        long len = loop_px(ts, t->loop);
        t->s += t->sense == 7 ? -step : step;
        while (t->s >= len) t->s -= len;
        while (t->s < 0) t->s += len;
        {
            long x, y;
            point_at(ts, t->loop, t->s, &x, &y, 0, &node);
        }
        if (node != t->lead_node) {
            t->lead_node = node;
            /* the lead bogie on a new block: a station brakes it (3.4) */
            if (t->control == 1 && t->state == 3 &&
                station_index(ts, node) >= 0 && node != t->last_st_node) {
                t->last_st_node = (short)node;
                brake_to(t, 2);
                t->sub = 5;
            }
        }
        if (logic)
            crush(ts, ti, tr, ps, pl_x, pl_y, pl_layer, pl_on_foot, sound, ctx);
    }
}

void gta_trains_tick(gta_trains *ts, gta_traffic *tr, gta_peds *ps,
                     long pl_x, long pl_y, int pl_layer, int pl_on_foot,
                     void (*sound)(void *ctx, int id, long x, long y),
                     void *ctx)
{
    int i;
    ts->half ^= 1;                      /* the original's frame: 25 Hz */
    for (i = 0; i < ts->n; i++)
        one_train(ts, i, tr, ps, pl_x, pl_y, pl_layer, pl_on_foot, sound, ctx,
                  !ts->half);
}

/* ---- drawing -------------------------------------------------------- */

void gta_trains_draw(const gta_trains *ts, gta_view *v)
{
    int i, j;
    if (ts->spr_count < 5)
        return;
    for (i = 0; i < ts->n; i++) {
        const gta_train *t = &ts->t[i];
        int cars = t->type == 2 ? 1 : GTA_TRAIN_CARS;
        if (t->wrecked)
            continue;
        for (j = 0; j < cars; j++) {
            long x, y, fx, fy, bx, by, dx, dy;
            int l, a, da;
            gta_trains_body(ts, i, j, &x, &y, &l, &a);
            dx = x - v->cam_x; dy = y - v->cam_y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > (14L << 21) || dy > (14L << 21))
                continue;
            da = (a + GTA_CAR_ART_SOUTH) & 255;
            bogies(ts, t, j, &fx, &fy, &bx, &by, 0);
            /* ONE HEIGHT FOR THE CARRIAGE, read once at its middle, and
             * given to all three sprites: left to the sprite path each
             * piece read the block under its own point and a 125-px body
             * was lifted a level whenever either end was over a slope, so
             * the carriages grew and shrank as they ran ("sie powiekszaja
             * i pomniejszaja wagony"). */
            {
                int sub8 = gta_render_sub_at(v, x, y, l);
                /* the end pieces: the engine's leading one train+4, the
                 * last carriage's trailing one train+3, the rest train+2 */
                gta_render_add_sprite_air(v, fx, fy, l, l, sub8,
                                          ts->spr_base + (j == 0 ? 4 : 2), da,
                                          0, -1, 0UL);
                gta_render_add_sprite_air(v, bx, by, l, l, sub8,
                                          ts->spr_base + (j == cars - 1 ? 3 : 2),
                                          da, 0, -1, 0UL);
                gta_render_add_sprite_air(v, x, y, l, l, sub8,
                                          ts->spr_base + (j == 0 ? 1 : 0), da,
                                          0, -1, 0UL);
            }
        }
    }
}

/* ---- the rider ------------------------------------------------------ */

int gta_trains_board(gta_trains *ts, long x, long y, int layer, int reach)
{
    int i, j;
    for (i = 0; i < ts->n; i++) {
        gta_train *t = &ts->t[i];
        int cars = t->type == 2 ? 1 : GTA_TRAIN_CARS;
        if (t->wrecked || t->speed != 0)
            continue;
        for (j = 0; j < cars; j++) {
            long bx, by, dx, dy;
            int bl;
            gta_trains_body(ts, i, j, &bx, &by, &bl, 0);
            if (bl != layer) continue;
            dx = (bx - x) >> FP; dy = (by - y) >> FP;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx <= reach && dy <= reach) return i;
        }
    }
    return -1;
}

int gta_trains_get_off(const gta_trains *ts, int ti, long *x, long *y,
                       int *layer)
{
    const gta_train *t = &ts->t[ti];
    long bx, by;
    int bl, a, side;
    if (t->speed != 0)
        return 0;
    gta_trains_body(ts, ti, t->type == 2 ? 0 : 1, &bx, &by, &bl, &a);
    for (side = 0; side < 2; side++) {
        int sa = (a + (side ? 192 : 64)) & 255;
        long tx = bx + (long)gta_sin(sa) * 30 * 4;
        long ty = by - (long)gta_cos(sa) * 30 * 4;
        if (gta_map_ground_type(ts->map, (int)(tx >> 21), (int)(ty >> 21), bl) == 3) {
            *x = bx + (long)gta_sin(sa) * 14 * 4;
            *y = by - (long)gta_cos(sa) * 14 * 4;
            *layer = bl;
            return 1;
        }
    }
    return 0;
}

int gta_trains_hijack(gta_trains *ts, int ti)
{
    gta_train *t = &ts->t[ti];
    if (t->wrecked || t->type == 2)
        return 0;
    if (t->control == 1) {
        t->control = 2;
        t->max = 80;
        if (t->state == 2 || t->state == 10) {
            if (t->state == 2 && t->door < 8) t->door = 8;
            t->state = 11;
            t->speed = 0;
        } else
            brake_to(t, 11);
        return 1;
    }
    t->control = 1;
    t->max = 60;
    if (t->speed == 0) t->state = 12;
    else brake_to(t, 12);
    return 0;
}

void gta_trains_drive(gta_trains *ts, int ti, int up, int down)
{
    gta_train *t = &ts->t[ti];
    if (t->control != 2 || t->wrecked)
        return;
    if (up) {
        if (t->sense == 6) t->state = 3;
        else if (t->speed == 0) { t->sense = 6; t->state = 3; }
        else if (t->state != 4) brake_to(t, 11);
    } else if (down) {
        if (t->sense == 7) t->state = 3;
        else if (t->speed == 0) { t->sense = 7; t->state = 3; }
        else if (t->state != 4) brake_to(t, 11);
    }
}
