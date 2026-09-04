/* The route finder: a path of blocks from here to there, along the map's own
 * one-way arrows.
 *
 * WHY THIS EXISTS AT ALL - and it is the single biggest thing this port had
 * wrong about traffic. GTA 1's cars do not pick a direction at each junction;
 * they are given a DESTINATION out of a pool and a route to it, and then they
 * follow it. One routine asks and another searches, and the answer is a list
 * of (x, y, z) blocks
 * kept per driver, 255 bytes of it - the game even has an error for that array
 * overflowing. The randomness in the original's traffic is in WHERE a car is
 * going, not in what it does at each corner, and that is why its traffic reads
 * as purposeful where a random walk reads as a shuffle.
 *
 * A BREADTH-FIRST SEARCH, not A*. The arrows make the graph directed and
 * sparse, every step costs the same, and the window below is small - so BFS
 * gives the same answer as A* would, in a form with no priority queue, no
 * scoring, and a bounded worst case. On a 68020 the bounded worst case is the
 * point.
 *
 * WHAT IT COSTS, AND WHY THAT IS SAFE. The search is confined to a window of
 * GTA_ROUTE_WINDOW blocks round the start, so it can visit at most that window
 * and the scratch is a fixed 4 KB rather than a 384 KB visited map. One search
 * is run per tick for the whole fleet (the original does the same - it has a
 * single global "a route is being searched for ped N" and refuses a second),
 * so the cost per frame is one search, not twenty.
 *
 * SINGLE LAYER. Routes are found within one map layer, because traffic in this
 * port cannot use ramps yet (LEFTOFF item 1). The z is carried through so that
 * when it can, this is where the change goes.
 *
 * Portable C89, no floats, no Amiga headers.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_ROUTE_H
#define GTA_ROUTE_H

#include "gta_nav.h"

/* How far from the start the search may look. 48 blocks is three screens at
 * the default zoom and further than a car gets between two route requests. */
#define GTA_ROUTE_WINDOW 48

/* How many blocks of route a car carries. The original keeps 85 (255 bytes of
 * 3); ours are 32-pixel blocks against its 64, so a shorter list covers a
 * comparable distance, and a car asks again when it runs out. */
#define GTA_ROUTE_MAX 40

/* How many blocks one search may look at before it gives up.
 *
 * Measured on the 68020: an unbounded search over the 48-block window costs
 * about eleven milliseconds, which is half a frame for one car's route. Most
 * searches are far cheaper - the streets are a thin graph inside a city of
 * buildings - so a budget costs nothing on the ordinary case and puts a
 * ceiling on the bad one. A refused route is not a failure: the car follows
 * the map's arrows and is asked again later. */
#define GTA_ROUTE_BUDGET 700

typedef struct {
    unsigned char x, y;
} gta_route_node;

/* WHY THE LAST SEARCH CAME BACK EMPTY - a diagnostic, and it exists because
 * "62% of searches fail" was blamed on the budget for a week and the budget
 * turned out to have nothing to do with it (tripling it to 2200 changed the
 * failure count by four out of nineteen thousand).
 *
 * A failure that never expanded a cell is a different fault from one that
 * explored everything it could reach and did not get there, and they need
 * opposite fixes: the first is `gta_route_pick_target` choosing somewhere
 * silly, the second is the map genuinely not connecting the two within the
 * window. Set on every call; read it right after one. */
#define GTA_ROUTE_OK        0   /* it found something */
#define GTA_ROUTE_NO_START  1   /* the car is not on a block traffic can leave */
#define GTA_ROUTE_NO_TARGET 2   /* the target is not a drivable block */
#define GTA_ROUTE_FAR       3   /* the target is outside the search window */
#define GTA_ROUTE_SAME      4   /* the target is the block we are on */
#define GTA_ROUTE_BUDGET_HIT 5  /* ran out of budget - the one it was blamed on */
#define GTA_ROUTE_EXHAUSTED 6   /* explored all it could reach; no way there */
#define GTA_ROUTE_FAIL_KINDS 7

int gta_route_last_fail(void);

/* Find somewhere this car can actually GET to, and the route there, in one
 * search - see the long note in gta_route.c. `min_d`/`max_d` are the Manhattan
 * distance band the destination should fall in, the same ring
 * gta_route_pick_target used to throw darts at. Returns route length, or 0 if
 * nowhere in the band is reachable at all. */
int gta_route_wander(const gta_nav *nav, int sx, int sy, int z, int ban,
                     int min_d, int max_d, unsigned long *seed,
                     gta_route_node *out, int max);

/* Find a route from (sx,sy) to (tx,ty) on layer z.
 *
 * Writes the blocks AFTER the start, up to and including the target, and
 * returns how many were written; 0 means there is no route inside the window
 * (or the start or target is not drivable). A return of `max` means the path
 * was truncated - the car drives what it has and asks again, which is what the
 * original does when its own array fills. */
/* `ban` IS THE WAY THE CAR CAME, and it is not optional. A breadth-first
 * search over the arrows knows nothing about which way the car is pointing, so
 * on a two-way street the shortest route to a destination behind the car
 * begins with the block it has just left - and the driver, being asked to go
 * that way, turned round in the road. That is exactly what was reported from
 * the emulator: "a car comes out of a side street, turns left onto the main
 * road, and immediately turns left again back into it". Cars in GTA do not do
 * that, and the fault is here rather than in the driving: the route asked for
 * it.
 *
 * So the reverse of the car's heading is refused AT THE START CELL only -
 * further down the route the search may double back on a different street,
 * which is an ordinary block circuit and looks like driving. Pass -1 for a car
 * that has no heading yet. */
int gta_route_find(const gta_nav *nav, int sx, int sy, int z,
                   int tx, int ty, int ban, gta_route_node *out, int max);

/* The same search for a PURSUER: the two-step no-doubling-back rule is
 * lifted, so a cop on one carriageway can route to the other through a
 * single crossover - the very loop the rule exists to refuse traffic. The
 * start-cell ban still holds. */
int gta_route_find_chase(const gta_nav *nav, int sx, int sy, int z,
                         int tx, int ty, int ban, gta_route_node *out, int max);

/* Pick somewhere to drive to: a road block `min`..`max` blocks away from
 * (bx,by) on layer z, chosen with the caller's own random number generator so
 * the host and the Amiga agree. Returns 0 if nothing suitable was found.
 *
 * `seed` is stepped in place; it is the traffic's LCG rather than rand(). */
int gta_route_pick_target(const gta_nav *nav, int bx, int by, int z,
                          int min, int max, unsigned long *seed,
                          int *tx, int *ty);

#endif /* GTA_ROUTE_H */
