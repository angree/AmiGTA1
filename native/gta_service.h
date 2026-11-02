/* THE AMBULANCE AND THE FIRE ENGINE - the research notes
 *
 * Ambulance (car model 5): sent for dead ordinary civilians. A body joins
 * an ambulance already working within 10 blocks (up to 10 bodies), else a
 * new one (at most GTA_AMB_MAX) comes from the nearest hospital, stops by
 * the body, a medic walks to it and kneels 75 frames, and the body gets up
 * with full health. With its list done it drives to hospital #1 and is
 * deleted on arrival. No siren.
 *
 * Fire engine (model 42): sent for the fires a blast leaves on the ground
 * (the original's object type 0x12; the port's gta_fire). At most 4 fires
 * tracked and 3 engines, each with a 2500-frame life, from the nearest of
 * the first four fire stations, lights on, silent. It stops by the fire,
 * swings the water cannon round, lays the jet (objects 0x30 / 0x31, reach
 * 320 original px) and the fire is gone the moment the jet is laid; it
 * sprays 261 frames, then the next fire or home, deleted off screen.
 *
 * PORT DEVIATIONS, stated: a tracked fire is kept burning until its engine
 * puts it out (the original's fire lifetime is not established, the port's
 * own is five seconds - no engine could ever arrive); the medic and the
 * engine use the port's route follower, not the original's A* mutex.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_SERVICE_H
#define GTA_SERVICE_H

#include "gta_map.h"
#include "gta_nav.h"
#include "gta_tiles.h"
#include "gta_traffic.h"
#include "gta_peds.h"
#include "gta_weapon.h"
#include "gta_render.h"

#define GTA_AMB_MAX     3      /* the original allows 10; 3 is plenty for a
                                * city with twelve people in it */
#define GTA_AMB_BODIES  10
#define GTA_ENGINE_MAX  3
#define GTA_FIREREC_MAX 4

typedef struct {
    unsigned long car, medic;
    unsigned long vser[GTA_AMB_BODIES];
    short vx[GTA_AMB_BODIES], vy[GTA_AMB_BODIES];   /* scene blocks */
    int  n, cur;
    int  state, timer;
} gta_amb;

typedef struct {
    unsigned long car;
    int  fire;              /* index into the weapons' fires, -1 none */
    long fx, fy;            /* where the fire was */
    int  layer;
    int  state, timer, life;
    int  cannon;            /* the cannon's angle, 256ths */
    int  hose_n;            /* jet pieces drawn while spraying */
} gta_engine;

typedef struct {
    const gta_map *map;
    const gta_nav *nav;
    const gta_tiles *tiles;
    gta_amb a[GTA_AMB_MAX];
    gta_engine e[GTA_ENGINE_MAX];
    unsigned char fire_taken[GTA_MAX_FIRES];
    int rec_amb, rec_engine;        /* car records of models 5 and 42 */
    int spr_jet, spr_jet_end;
    unsigned long stat_revived, stat_amb, stat_engines, stat_put_out;
    int half;
} gta_service;

void gta_service_init(gta_service *sv, const gta_map *m, const gta_nav *nav,
                      const gta_tiles *t);
/* One 50 Hz tick (the service logic runs on every other one). */
void gta_service_tick(gta_service *sv, gta_traffic *tr, gta_peds *ps,
                      gta_weapons *w);
/* The fire engines' jets. */
void gta_service_draw(const gta_service *sv, const gta_traffic *tr,
                      gta_view *v);

#endif /* GTA_SERVICE_H */
