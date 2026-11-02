/* The ambulance and the fire engine. Read gta_service.h, then
 * the research notes
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <string.h>

#include "gta_service.h"
#include "gta_trig.h"

#define FP 16

/* the states, named after the original's where there is one */
enum { A_FREE = 0, A_DRIVE = 1, A_STOP = 0x50, A_WALK = 0x5c, A_REVIVE = 0x65,
       A_BACK = 0x66, A_NEXT = 2, A_HOME = 4 };
enum { E_FREE = 0, E_DRIVE = 1, E_BRAKE = 10, E_AIM = 100, E_SPRAY = 0x82,
       E_STOW = 0xa0, E_NEXT = 0x1e, E_HOME = 0x28 };

#define REVIVE_FRAMES   75
#define SPRAY_FRAMES    261
#define ENGINE_LIFE     2500
#define HOSE_REACH_PX   160         /* 320 original px */

static int record_of_model(const gta_tiles *t, int model)
{
    int i;
    for (i = 0; i < t->n_cars; i++)
        if (t->cars[i].model_id == model) return i;
    return -1;
}

void gta_service_init(gta_service *sv, const gta_map *m, const gta_nav *nav,
                      const gta_tiles *t)
{
    memset(sv, 0, sizeof *sv);
    sv->map = m;
    sv->nav = nav;
    sv->tiles = t;
    sv->rec_amb = record_of_model(t, 5);
    sv->rec_engine = record_of_model(t, 42);
    sv->spr_jet = gta_tiles_object_sprite(t, 0x30);
    sv->spr_jet_end = gta_tiles_object_sprite(t, 0x31);
}

/* The road block nearest (bx,by), ring by ring - on layer `pref` first (the
 * layer the body, the fire or the base is on: the highest road in a column is
 * often a bridge or the El, and a car put down there has no way to the
 * street below), on any layer only if that layer has none within 8. */
static int road_ring(const gta_service *sv, int bx, int by, int l0, int l1,
                     int *rx, int *ry, int *rl)
{
    int r, dx, dy, l;
    for (r = 0; r <= 8; r++)
        for (dy = -r; dy <= r; dy++)
            for (dx = -r; dx <= r; dx++) {
                if (dx > -r && dx < r && dy > -r && dy < r) continue;
                for (l = l1; l >= l0; l--) {
                    unsigned char n = gta_nav_at(sv->nav, bx + dx, by + dy, l);
                    if (gta_nav_ground(n) == 2 && gta_nav_dirs(n)) {
                        *rx = bx + dx; *ry = by + dy; *rl = l;
                        return 1;
                    }
                }
            }
    return 0;
}

static int road_near(const gta_service *sv, int bx, int by, int pref,
                     int *rx, int *ry, int *rl)
{
    if (pref >= 0 && pref < GTA_MAP_LAYERS &&
        road_ring(sv, bx, by, pref, pref, rx, ry, rl))
        return 1;
    return road_ring(sv, bx, by, 0, GTA_MAP_LAYERS - 1, rx, ry, rl);
}

/* a base's stored z counts from the top, like the mission file's */
#define LOC_LAYER(loc) (GTA_MAP_LAYERS - 1 - (int)(loc)->z)

static int cheb(int ax, int ay, int bx, int by)
{
    int dx = ax - bx, dy = ay - by;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx > dy ? dx : dy;
}

/* A car of record `rec` put down at the road by base `loc`, driving, sent to
 * (tx,ty). Serial, or 0. */
static unsigned long spawn_unit(gta_service *sv, gta_traffic *tr, int rec,
                                const gta_map_loc *loc, int tx, int ty)
{
    int rx, ry, rl;
    unsigned long s;
    if (rec < 0 || !road_near(sv, loc->x, loc->y, LOC_LAYER(loc), &rx, &ry, &rl))
        return 0;
    s = gta_traffic_abandon(tr, rec, ((long)rx * 32 + 16) << FP,
                            ((long)ry * 32 + 16) << FP, 0, rl, -1, 0);
    if (!s) return 0;
    gta_traffic_wake(tr, sv->map, s);
    gta_traffic_set_service(tr, s, rec == sv->rec_amb ? 1 : 2);
    /* kept by the fleet however far from the player it is - the retire
     * sweep takes an ordinary car off screen at once, and a base is
     * usually off screen (gta_traffic_remove_car clears it again) */
    gta_traffic_set_mission(tr, s, 1);
    gta_traffic_send_to(tr, sv->map, s, tx, ty);
    return s;
}

/* ---- the ambulance -------------------------------------------------- */

static int ped_index(const gta_peds *ps, unsigned long serial)
{
    int i;
    if (!serial) return -1;
    for (i = 0; i < GTA_MAX_PEDS; i++)
        if (ps->p[i].serial == serial && ps->p[i].alive) return i;
    return -1;
}

static void queue_bodies(gta_service *sv, gta_traffic *tr, gta_peds *ps)
{
    int i, k;
    for (i = 0; i < GTA_MAX_PEDS; i++) {
        gta_ped *p = &ps->p[i];
        int bx, by, best = -1, sx, sy, sl;
        if (!p->alive || !p->corpse || p->svc_q || p->cop || p->mission ||
            p->uniform)
            continue;
        p->svc_q = 1;
        if (!p->serial)
            p->serial = ++ps->next_ped_serial;
        bx = (int)(p->x >> 21); by = (int)(p->y >> 21);
        if (!road_near(sv, bx, by, p->layer, &sx, &sy, &sl))
            continue;
        /* join one working within 10 blocks of its current body */
        for (k = 0; k < GTA_AMB_MAX; k++) {
            gta_amb *a = &sv->a[k];
            if (a->state == A_FREE || a->state == A_HOME || a->n >= GTA_AMB_BODIES
                || a->cur >= a->n)
                continue;
            if (cheb(a->vx[a->cur], a->vy[a->cur], sx, sy) < 10) { best = k; break; }
        }
        if (best >= 0) {
            gta_amb *a = &sv->a[best];
            a->vser[a->n] = p->serial; a->vx[a->n] = (short)sx; a->vy[a->n] = (short)sy;
            a->n++;
            p->svc_q = 2;
            printf("gta: service - body %lu joins ambulance %d (%d of %d)\n",
                   p->serial, best, a->n, GTA_AMB_BODIES);
            continue;
        }
        for (k = 0; k < GTA_AMB_MAX; k++)
            if (sv->a[k].state == A_FREE) break;
        if (k == GTA_AMB_MAX || sv->map->n_hospital <= 0)
            continue;
        {
            gta_amb *a = &sv->a[k];
            int h, hb = 0, hd = 9999;
            for (h = 0; h < sv->map->n_hospital; h++) {
                int d;
                if (!sv->map->hospital[h].x) continue;
                d = cheb(sv->map->hospital[h].x, sv->map->hospital[h].y, sx, sy);
                if (d < hd) { hd = d; hb = h; }
            }
            memset(a, 0, sizeof *a);
            a->car = spawn_unit(sv, tr, sv->rec_amb, &sv->map->hospital[hb], sx, sy);
            if (!a->car) continue;
            a->vser[0] = p->serial; a->vx[0] = (short)sx; a->vy[0] = (short)sy;
            a->n = 1;
            p->svc_q = 2;
            a->state = A_DRIVE;
            sv->stat_amb++;
            printf("gta: service - ambulance %d from hospital %d (%d,%d) for"
                   " the body at (%d,%d)\n", k, hb, sv->map->hospital[hb].x,
                   sv->map->hospital[hb].y, sx, sy);
        }
    }
    fflush(stdout);
}

/* Bodies from `from` on are no longer waited for: the recycler may have
 * them (the original removes an abandoned body outright, 0x6c). */
static void amb_release(gta_amb *a, gta_peds *ps, int from)
{
    int j;
    for (j = from; j < a->n; j++) {
        int vi = ped_index(ps, a->vser[j]);
        if (vi >= 0 && ps->p[vi].svc_q == 2) ps->p[vi].svc_q = 1;
    }
}

static void amb_tick(gta_service *sv, int k, gta_traffic *tr, gta_peds *ps)
{
    gta_amb *a = &sv->a[k];
    long cx, cy;
    int cl, wrecked, vi;
    if (a->state == A_FREE)
        return;
    if (!gta_traffic_find_car(tr, a->car, &cx, &cy, &cl, &wrecked) || wrecked) {
        /* the car is gone or burnt: the job is over */
        if (a->medic) gta_peds_take_mission(ps, a->medic);
        amb_release(a, ps, a->cur);
        a->state = A_FREE;
        return;
    }
    vi = a->cur < a->n ? ped_index(ps, a->vser[a->cur]) : -1;
    switch (a->state) {
    case A_DRIVE:
        if (gta_traffic_arrived(tr, a->car) ||
            cheb((int)(cx >> 21), (int)(cy >> 21), a->vx[a->cur], a->vy[a->cur]) <= 1)
            a->state = A_STOP;
        break;
    case A_STOP:
        if (vi < 0 || !ps->p[vi].corpse) { a->state = A_NEXT; break; }
        a->medic = gta_peds_spawn_mission(ps, cx + (12L << FP), cy, cl, 0);
        if (!a->medic) break;                   /* retried every tick */
        gta_peds_mission_ai(ps, a->medic, GTA_PED_MODE_SENDTO,
                            ps->p[vi].x, ps->p[vi].y, 0);
        a->state = A_WALK;
        a->timer = 0;
        break;
    case A_WALK: {
        long mx, my;
        int ml, alive;
        if (vi < 0 || !ps->p[vi].corpse) { a->state = A_BACK; break; }
        if (!gta_peds_find(ps, a->medic, &mx, &my, &ml, &alive) || !alive) {
            a->medic = 0; a->state = A_HOME; break;   /* the medic was killed */
        }
        {
            long dx = (mx - ps->p[vi].x) >> FP, dy = (my - ps->p[vi].y) >> FP;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if ((dx <= 8 && dy <= 8) || ++a->timer > 400) {
                a->state = A_REVIVE; a->timer = 0;
            }
        }
        break;
    }
    case A_REVIVE:
        if (vi < 0 || !ps->p[vi].corpse) { a->state = A_BACK; break; }
        if (++a->timer >= REVIVE_FRAMES) {
            gta_peds_revive(ps, vi);
            sv->stat_revived++;
            printf("gta: service - ambulance %d: body %lu revived\n", k,
                   a->vser[a->cur]);
            fflush(stdout);
            a->state = A_BACK; a->timer = 0;
        }
        break;
    case A_BACK:
        if (a->timer == 0)
            gta_peds_mission_ai(ps, a->medic, GTA_PED_MODE_SENDTO, cx, cy, 0);
        {
            long mx, my;
            int ml, alive;
            int there = !gta_peds_find(ps, a->medic, &mx, &my, &ml, &alive);
            if (!there) {
                long dx = (mx - cx) >> FP, dy = (my - cy) >> FP;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                there = dx <= 14 && dy <= 14;
            }
            if (there || ++a->timer > 400) {
                gta_peds_take_mission(ps, a->medic);
                a->medic = 0;
                a->state = A_NEXT;
            }
        }
        break;
    case A_NEXT:
        if (a->cur < a->n) {
            int vj = ped_index(ps, a->vser[a->cur]);
            if (vj >= 0 && ps->p[vj].svc_q == 2) ps->p[vj].svc_q = 1;
        }
        if (++a->cur < a->n) {
            gta_traffic_send_to(tr, sv->map, a->car, a->vx[a->cur], a->vy[a->cur]);
            a->state = A_DRIVE;
        } else {
            /* home to hospital #1, whatever is nearest */
            int rx, ry, rl;
            if (road_near(sv, sv->map->hospital[0].x, sv->map->hospital[0].y,
                          LOC_LAYER(&sv->map->hospital[0]),
                          &rx, &ry, &rl))
                gta_traffic_send_to(tr, sv->map, a->car, rx, ry);
            a->state = A_HOME;
            a->timer = 0;
        }
        break;
    case A_HOME:
        if (gta_traffic_arrived(tr, a->car) || ++a->timer > 6000) {
            gta_traffic_remove_car(tr, a->car);     /* on screen or not */
            a->state = A_FREE;
        }
        break;
    }
}

/* ---- the fire engine ------------------------------------------------ */

static void engine_release(gta_service *sv, gta_engine *e)
{
    if (e->fire >= 0 && e->fire < GTA_MAX_FIRES)
        sv->fire_taken[e->fire] = 0;
    e->fire = -1;
}

static int engine_pick_fire(gta_service *sv, gta_weapons *w, gta_engine *e,
                            long cx, long cy)
{
    int i, best = -1;
    long bd = 0x7fffffffL;
    for (i = 0; i < GTA_MAX_FIRES; i++) {
        long dx, dy, d;
        if (w->f[i].ticks <= 0 || sv->fire_taken[i]) continue;
        dx = (w->f[i].x - cx) >> 21; dy = (w->f[i].y - cy) >> 21;
        d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return 0;
    sv->fire_taken[best] = 1;
    e->fire = best;
    e->fx = w->f[best].x; e->fy = w->f[best].y; e->layer = w->f[best].layer;
    return 1;
}

static void dispatch_fires(gta_service *sv, gta_traffic *tr, gta_weapons *w)
{
    int i, k, tracked = 0;
    for (i = 0; i < GTA_MAX_FIRES; i++)
        if (sv->fire_taken[i]) tracked++;
    for (i = 0; i < GTA_MAX_FIRES && tracked < GTA_FIREREC_MAX; i++) {
        int fx, fy, rx, ry, rl, s, sb = -1, sd = 9999, near_engine = 0;
        if (w->f[i].ticks <= 0 || sv->fire_taken[i]) continue;
        fx = (int)(w->f[i].x >> 21); fy = (int)(w->f[i].y >> 21);
        /* no second engine within 40 blocks of one already sent */
        for (k = 0; k < GTA_ENGINE_MAX; k++)
            if (sv->e[k].state != E_FREE && sv->e[k].fire >= 0 &&
                cheb((int)(sv->e[k].fx >> 21), (int)(sv->e[k].fy >> 21), fx, fy) < 40)
                near_engine = 1;
        if (near_engine) continue;
        for (k = 0; k < GTA_ENGINE_MAX; k++)
            if (sv->e[k].state == E_FREE) break;
        if (k == GTA_ENGINE_MAX) return;
        for (s = 0; s < sv->map->n_fire && s < 4; s++) {
            int d;
            if (!sv->map->fire[s].x) continue;
            d = cheb(sv->map->fire[s].x, sv->map->fire[s].y, fx, fy);
            if (d < sd) { sd = d; sb = s; }
        }
        if (sb < 0 || !road_near(sv, fx, fy, w->f[i].layer, &rx, &ry, &rl)) continue;
        {
            gta_engine *e = &sv->e[k];
            memset(e, 0, sizeof *e);
            e->fire = -1;
            e->car = spawn_unit(sv, tr, sv->rec_engine, &sv->map->fire[sb], rx, ry);
            if (!e->car) continue;
            sv->fire_taken[i] = 1;
            e->fire = i;
            e->fx = w->f[i].x; e->fy = w->f[i].y; e->layer = w->f[i].layer;
            e->state = E_DRIVE;
            e->life = ENGINE_LIFE;
            tracked++;
            sv->stat_engines++;
            printf("gta: service - fire engine %d from station %d (%d,%d)"
                   " for the fire at (%d,%d)\n", k, sb, sv->map->fire[sb].x,
                   sv->map->fire[sb].y, fx, fy);
            fflush(stdout);
        }
    }
}

static void engine_tick(gta_service *sv, int k, gta_traffic *tr, gta_weapons *w)
{
    gta_engine *e = &sv->e[k];
    long cx, cy;
    int cl, wrecked;
    if (e->state == E_FREE)
        return;
    if (!gta_traffic_find_car(tr, e->car, &cx, &cy, &cl, &wrecked) || wrecked) {
        engine_release(sv, e);
        e->state = E_FREE;
        return;
    }
    /* A TRACKED FIRE BURNS ON until its engine puts it out (see the .h) */
    if (e->fire >= 0 && e->state < E_SPRAY && w->f[e->fire].ticks > 0 &&
        w->f[e->fire].ticks < 20)
        w->f[e->fire].ticks = 20;
    if (--e->life <= 0 && e->state < E_NEXT) {
        engine_release(sv, e);
        e->state = E_HOME;
    }
    switch (e->state) {
    case E_DRIVE: {
        int d = cheb((int)(cx >> 21), (int)(cy >> 21),
                     (int)(e->fx >> 21), (int)(e->fy >> 21));
        /* the original's state 1: stop once the path is done, or within
         * five blocks (d < 5) - the jet reaches 320 original px */
        if (gta_traffic_arrived(tr, e->car) || d < 5) {
            gta_traffic_send_to(tr, sv->map, e->car, (int)(cx >> 21),
                                (int)(cy >> 21));       /* stop here */
            e->state = E_BRAKE;
        }
        break;
    }
    case E_BRAKE:
        e->state = E_AIM;
        e->cannon = 0;
        break;
    case E_AIM: {
        int want = (int)((gta_dir16((e->fx - cx) >> 8, (e->fy - cy) >> 8) >> 16) & 255);
        int d = (want - e->cannon) & 255;
        if (d < 3 || d > 253) {
            long dx = (e->fx - cx) >> FP, dy = (e->fy - cy) >> FP;
            long dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            e->cannon = want;
            if (dist > HOSE_REACH_PX + HOSE_REACH_PX / 2) {
                engine_release(sv, e);
                e->state = E_STOW;
                break;
            }
            e->hose_n = (int)(dist / 15);
            if (e->hose_n > 8) e->hose_n = 8;
            /* THE FIRE IS GONE THE MOMENT THE JET IS LAID (4.5) */
            if (e->fire >= 0) {
                w->f[e->fire].ticks = 0;
                sv->stat_put_out++;
                printf("gta: service - fire engine %d: fire out\n", k);
                fflush(stdout);
            }
            e->state = E_SPRAY;
            e->timer = 0;
        } else
            e->cannon = (e->cannon + (d < 128 ? 3 : -3)) & 255;
        break;
    }
    case E_SPRAY:
        if (++e->timer >= SPRAY_FRAMES) {
            engine_release(sv, e);
            e->hose_n = 0;
            e->state = E_STOW;
        }
        break;
    case E_STOW:
        e->cannon = 0;
        e->state = E_NEXT;
        break;
    case E_NEXT:
        if (e->life > 0 && engine_pick_fire(sv, w, e, cx, cy)) {
            int rx, ry, rl;
            if (road_near(sv, (int)(e->fx >> 21), (int)(e->fy >> 21), e->layer, &rx, &ry, &rl))
                gta_traffic_send_to(tr, sv->map, e->car, rx, ry);
            e->state = E_DRIVE;
        } else {
            int s, rx, ry, rl, sb = 0, sd = 9999;
            for (s = 0; s < sv->map->n_fire && s < 4; s++) {
                int d = cheb(sv->map->fire[s].x, sv->map->fire[s].y,
                             (int)(cx >> 21), (int)(cy >> 21));
                if (sv->map->fire[s].x && d < sd) { sd = d; sb = s; }
            }
            if (road_near(sv, sv->map->fire[sb].x, sv->map->fire[sb].y,
                          LOC_LAYER(&sv->map->fire[sb]), &rx, &ry, &rl))
                gta_traffic_send_to(tr, sv->map, e->car, rx, ry);
            gta_traffic_set_service(tr, e->car, 3);     /* lights off */
            e->state = E_HOME;
            e->timer = 0;
        }
        break;
    case E_HOME:
        /* deleted off screen (the original's routine), or once home */
        if (!gta_traffic_car_in_view(tr, e->car) || gta_traffic_arrived(tr, e->car)
            || ++e->timer > 6000) {
            if (!gta_traffic_car_in_view(tr, e->car)) {
                gta_traffic_remove_car(tr, e->car);
                e->state = E_FREE;
            }
        }
        break;
    }
}

void gta_service_tick(gta_service *sv, gta_traffic *tr, gta_peds *ps,
                      gta_weapons *w)
{
    int k;
    sv->half ^= 1;
    if (sv->half)
        return;                         /* the original's AI frame */
    queue_bodies(sv, tr, ps);
    for (k = 0; k < GTA_AMB_MAX; k++) {
        int was = sv->a[k].state;
        amb_tick(sv, k, tr, ps);
        if (sv->a[k].state != was) {
            long x = 0, y = 0;
            int l = 0, wr = 0;
            gta_traffic_find_car(tr, sv->a[k].car, &x, &y, &l, &wr);
            printf("gta: service - ambulance %d state %d -> %d at (%ld,%ld)\n",
                   k, was, sv->a[k].state, x >> 21, y >> 21);
        }
    }
    if (w) {
        dispatch_fires(sv, tr, w);
        for (k = 0; k < GTA_ENGINE_MAX; k++) {
            int was = sv->e[k].state;
            engine_tick(sv, k, tr, w);
            if (sv->e[k].state != was) {
                long x = 0, y = 0;
                int l = 0, wr = 0;
                gta_traffic_find_car(tr, sv->e[k].car, &x, &y, &l, &wr);
                printf("gta: service - engine %d state %d -> %d at (%ld,%ld),"
                       " fire (%ld,%ld)\n", k, was, sv->e[k].state,
                       x >> 21, y >> 21, sv->e[k].fx >> 21, sv->e[k].fy >> 21);
            }
        }
    }
    fflush(stdout);
}

void gta_service_draw(const gta_service *sv, const gta_traffic *tr,
                      gta_view *v)
{
    int k, j;
    if (sv->spr_jet < 0)
        return;
    for (k = 0; k < GTA_ENGINE_MAX; k++) {
        const gta_engine *e = &sv->e[k];
        long cx, cy, sx, sy;
        int cl, wr;
        if (e->state != E_SPRAY || e->hose_n <= 0)
            continue;
        if (!gta_traffic_find_car(tr, e->car, &cx, &cy, &cl, &wr))
            continue;
        sx = (long)gta_sin(e->cannon) * 4;      /* one px along the jet */
        sy = -(long)gta_cos(e->cannon) * 4;
        for (j = 0; j < e->hose_n; j++) {
            int last = j == e->hose_n - 1;
            int spr = last && sv->spr_jet_end >= 0 ? sv->spr_jet_end : sv->spr_jet;
            long d = 18 + j * 15;
            gta_render_add_sprite(v, cx + sx * d, cy + sy * d, cl, cl, spr,
                                  (e->cannon + 128) & 255);
        }
    }
}
