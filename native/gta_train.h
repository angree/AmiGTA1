/* THE TRAINS - Liberty City's El, Vice City's line, San Andreas' trams.
 * Read the research notes first: everything here is that note, in the port's
 * units (32 px a block, 16.16 world px, the 50 Hz tick of which the train
 * logic uses every other one - the original's frame).
 *
 * Nothing in mission.ini declares a train. At load the rail blocks are
 * walked into closed loops (the original's the original's routine), every station is
 * recorded, and a train of four carriages is put down at every station that
 * is also a spawn point (hint 7), at most six. A train on a road block is a
 * TRAM, and the original never moves trams - nor does this.
 *
 * DEVIATION, stated: the original moves each bogie along a nine-shape path
 * table inside each block (TRAINS.md 4.2). Here the track is the polyline
 * through the block centres; a carriage's body sits between its two bogies
 * and takes their angle, so a corner is cut on the chord instead of the
 * table's curve. Speeds, stops, signals, crush and wreck are the original's.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_TRAIN_H
#define GTA_TRAIN_H

#include "gta_map.h"
#include "gta_tiles.h"
#include "gta_render.h"
#include "gta_traffic.h"
#include "gta_peds.h"

#define GTA_TRAIN_MAX      6
#define GTA_TRAIN_CARS     4
#define GTA_TRACK_MAX      900      /* the original's visited list: 880 */
#define GTA_TRACK_LOOPS    4
#define GTA_STATION_MAX    16

typedef struct {
    unsigned char x, y, layer, hint;
} gta_track_node;

/* THE PATH THE BOGIES FOLLOW (TRAINS.md 4.2-4.3): straight runs along the
 * block centres, and at every corner a quarter circle tangent to both -
 * radius half a block for the ordinary turn (the table's shape 1/2, entry
 * edge middle to side edge middle) and a block and a half where the map
 * marks a wide curve with hints 4/5 either side (shapes 3..8, 150 original
 * px for the quarter). */
typedef struct {
    long x0, y0;            /* line: its start; arc: the corner block centre */
    long s0, len;           /* where it starts along the loop, and how long */
    signed char d1x, d1y;   /* line: its direction; arc: the way in */
    signed char d2x, d2y;   /* arc: the way out */
    short r;                /* arc radius, px */
    short node;             /* line: the corner it leaves; arc: the corner */
    short r_from;           /* line: the radius of the corner it leaves */
    unsigned char arc;
} gta_track_piece;
#define GTA_TRACK_PIECES (GTA_TRACK_MAX)

typedef struct {
    unsigned char type;         /* 1 train, 2 tram (never moves) */
    unsigned char loop;
    unsigned char state;        /* TRAINS.md 3.3: 2 station, 3 go, 4 brake,
                                 * 10 wait to depart, 11 stopped/driven, 12 */
    unsigned char after_brake;  /* the state braking ends in */
    unsigned char sub, sub_first;
    unsigned char control;      /* 1 automatic, 2 the player */
    unsigned char sense;        /* 6 as spawned, 7 reversed */
    unsigned char door;         /* 8 shut .. 1..7 */
    unsigned char wrecked;
    short speed, max;           /* the original's units: px/frame = speed/10 */
    short wait, drop;
    short cur_st, next_st;
    short last_st_node;         /* station node braked for, not again */
    long  s;                    /* lead bogie, 16.16 port px along the loop */
    int   lead_node;            /* the node the lead bogie is in */
} gta_train;

typedef struct {
    const gta_map *map;
    const gta_tiles *tiles;
    gta_track_node node[GTA_TRACK_MAX];
    int n_nodes;
    short loop_first[GTA_TRACK_LOOPS], loop_len[GTA_TRACK_LOOPS];
    int n_loops;
    gta_track_piece piece[GTA_TRACK_PIECES];
    int n_pieces;
    short loop_piece0[GTA_TRACK_LOOPS], loop_npieces[GTA_TRACK_LOOPS];
    long loop_total[GTA_TRACK_LOOPS];       /* 16.16 px */
    long node_s[GTA_TRACK_MAX];             /* each block centre's s */
    short st_node[GTA_STATION_MAX];       /* index into node[] */
    unsigned char st_loop[GTA_STATION_MAX], st_busy[GTA_STATION_MAX];
    int n_st;
    gta_train t[GTA_TRAIN_MAX];
    int n;
    int half;                   /* the train logic runs every other tick */
    int spr_base, spr_count;    /* the train sprite category */
    /* the player: -1 not on a train, else the train he rides */
    int pl_on;
    /* set when a train ran the player over this tick (on foot) */
    int pl_hit;
    unsigned long stat_crush, stat_runover, stat_stops;
} gta_trains;

/* Walk the rail, build the loops and stations, spawn the trains. Returns the
 * number of trains (0 is a valid answer: an open line means none, as in the
 * original). */
int  gta_trains_init(gta_trains *ts, const gta_map *m, const gta_tiles *t);
/* One 50 Hz tick. Crushes cars and runs over people on the way; plays the
 * air brake through `sound` (may be 0) at the given world point. */
void gta_trains_tick(gta_trains *ts, gta_traffic *tr, gta_peds *ps,
                     long pl_x, long pl_y, int pl_layer, int pl_on_foot,
                     void (*sound)(void *ctx, int id, long x, long y),
                     void *ctx);
void gta_trains_draw(const gta_trains *ts, gta_view *v);

/* The carriage body centre and angle (256ths) of train `t`'s carriage `j`. */
void gta_trains_body(const gta_trains *ts, int t, int j, long *x, long *y,
                     int *layer, int *angle);
/* A stopped train within `reach` px of (x,y) on the layer, or -1. */
int  gta_trains_board(gta_trains *ts, long x, long y, int layer, int reach);
/* May the rider get off now? Fills where he stands. 0 if moving or no
 * pavement within 60 original px (30 here) of the second carriage. */
int  gta_trains_get_off(const gta_trains *ts, int t, long *x, long *y,
                        int *layer);
/* FIRE on a train: take the controls (returns 1) or hand them back (0). */
int  gta_trains_hijack(gta_trains *ts, int t);
/* The rider's keys while he drives. */
void gta_trains_drive(gta_trains *ts, int t, int up, int down);
/* Wreck the whole train: every carriage becomes a damage-100 car. */
void gta_trains_wreck(gta_trains *ts, int t, gta_traffic *tr,
                      void (*sound)(void *ctx, int id, long x, long y),
                      void *ctx);

#endif /* GTA_TRAIN_H */
