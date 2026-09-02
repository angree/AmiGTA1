/* Parked cars in Liberty City. Read gta_traffic.h first - it carries the
 * reasoning, including why a car's facing constant is the player's and how
 * that was established without being able to read car art.
 *
 * Licence: MIT (ours).
 */
#include <string.h>

#include "gta_traffic.h"
#include "gta_trig.h"

#define FP 16

/* Ground types, as in gta_player.h. Repeated rather than included: a car has
 * no business pulling in the player. */
#define GROUND_ROAD      2
#define GROUND_PAVEMENT  3

/* THE NAVIGATION GRID, remembered at file scope as well as in the fleet.
 *
 * Every geometric test in this file goes through ground_at(), and on the map
 * that is a column walk: the traffic tick was 1.4 ms for twenty cars on the
 * 68020 and most of it was here - `fits()` alone asks for up to five blocks,
 * twice a car, every tick. The grid answers the same question with one indexed
 * byte. It is a file-scope pointer rather than a parameter because ground_at()
 * has a dozen callers and threading it through all of them would say nothing
 * the name does not already. */
static const gta_nav *g_nav;

/* COSINE AND SINE AT 16.16 ANGULAR RESOLUTION.
 *
 * gta_cos() is a 256-entry table, and 256 steps round a circle is 1.4 degrees -
 * on a 29-pixel arc that is a 0.7-pixel jump between one table entry and the
 * next, which a car crawling through a corner at half a pixel a tick would
 * take as a stutter. Interpolating between the two neighbours costs one
 * multiply and keeps the arc smooth at any speed.
 *
 * Q14 in, Q14 out. (c1 - c0) is at most 402, so the product is under 2.7e7 and
 * fits a 32-bit long with room to spare. */
static long cos_fp(long a_fp)
{
    int i = (int)((a_fp >> FP) & 255);
    long f = a_fp & 0xFFFFL;
    long c0 = gta_cos(i), c1 = gta_cos(i + 1);
    return c0 + (((c1 - c0) * f) >> FP);
}

static long sin_fp(long a_fp)
{
    return cos_fp(a_fp - (64L << FP));
}

static int ground_at(const gta_map *m, int bx, int by, int z)
{
    gta_block b;

    if (g_nav && g_nav->b)
        return gta_nav_ground(gta_nav_at_m(g_nav, bx, by, z));

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM)
        return 0;
    if (z < 0 || z >= GTA_MAP_LAYERS)
        return 0;
    if (!gta_map_block(m, bx, by, z, &b))
        return 0;
    return gta_block_ground_type(&b);
}

/* Is this block part of the elevated railway?
 *
 * It matters because the railway's own blocks are marked ground type ROAD -
 * which is correct, a train runs on them - so "is it road?" says yes and a car
 * gets parked on the tracks. Worse, the viaduct is ONE BLOCK WIDE, so a car up
 * there also overhangs into thin air on both sides. Both were found by the
 * placement test in `gtadump traffic`, which flagged four cars that every
 * other rule considered perfectly placed. */
static int is_railway(const gta_map *m, int bx, int by, int z)
{
    gta_block b;
    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM)
        return 0;
    if (z < 0 || z >= GTA_MAP_LAYERS)
        return 0;
    if (!gta_map_block(m, bx, by, z, &b))
        return 0;
    return gta_block_is_railway(&b);
}

/* The lowest layer of this column whose ground type is ROAD, or -1.
 *
 * ROAD and not "walkable": a car parked on grass or on a pavement is a bug
 * that looks like a feature, and the map already distinguishes the two. The
 * LOWEST such layer for the same reason the player starts on the lowest
 * walkable one - a road on a bridge above the street is not somewhere a car
 * could have been parked from here. */
static int road_layer(const gta_map *m, int bx, int by)
{
    int z;
    for (z = 0; z < GTA_MAP_LAYERS; z++)
        if (ground_at(m, bx, by, z) == GROUND_ROAD &&
            !is_railway(m, bx, by, z))
            return z;
    return -1;
}

/* An LCG, ours, so the host and the Amiga produce the same street. The
 * constants are Numerical Recipes'; the low bits of an LCG are poor, so every
 * caller takes from the top. */
static unsigned long next_rand(gta_traffic *tr)
{
    tr->seed = tr->seed * 1664525UL + 1013904223UL;
    return (tr->seed >> 16) & 0x7FFFUL;
}

/* THE ROAD'S OWN DIRECTION BITS, as a heading, or -1 if the block carries none.
 *
 * type_map bits 0..3 say which ways traffic may leave a block: one bit for a
 * one-way street, the opposing pair for a two-way road, three or four at a
 * junction. This is the real data and it replaces the shape heuristic below,
 * which was always described here as a placeholder for it.
 *
 * It is coherent in the map and can be read straight off:
 *
 *   (62,60) (62,62) (63,60)  dirs=N...   a one-way avenue running north
 *   (62,58)                  dirs=N.W.   the junction where it meets a street
 *   (58,60)                  dirs=....   a building - no direction at all
 *
 * Where several are legal, one is chosen at random, which for a parked car is
 * the whole of the decision. A DRIVING car will want the same table for a
 * different question - "which way may I go from here" - and that is the point
 * at which junctions start to matter. */
static int road_heading(const gta_map *m, int bx, int by, int z, gta_traffic *tr)
{
    gta_block b;
    int angles[4], n = 0;

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM)
        return -1;
    if (z < 0 || z >= GTA_MAP_LAYERS)
        return -1;
    if (!gta_map_block(m, bx, by, z, &b))
        return -1;

    if (gta_block_dir_north(&b)) angles[n++] = 0;
    if (gta_block_dir_east(&b))  angles[n++] = 64;
    if (gta_block_dir_south(&b)) angles[n++] = 128;
    if (gta_block_dir_west(&b))  angles[n++] = 192;
    if (n == 0)
        return -1;
    return angles[next_rand(tr) % (unsigned long)n];
}

/* Which way the road under (bx, by) runs, as an angle: 0 for a north-south
 * street, 64 for an east-west one.
 *
 * THE FALLBACK, now that road_heading() reads the map's own direction bits.
 * Still needed: not every drivable block carries them - a car park or a
 * forecourt has a road ground type and no direction at all - and a car there
 * still has to be laid along something rather than across it.
 *
 * It measures the shape of the road itself: count how far the run of road
 * blocks reaches along each axis and take the longer. On a straight street one
 * axis wins clearly; in the middle of a junction they tie, and the tie goes to
 * north-south, which is arbitrary and harmless because a junction is exactly
 * where a parked car should not be put anyway (see gta_traffic_park). */
static int road_axis(const gta_map *m, int bx, int by, int z)
{
    int nx = 0, ny = 0, i;

    for (i = 1; i <= 3; i++) {
        if (ground_at(m, bx - i, by, z) == GROUND_ROAD) nx++; else break;
    }
    for (i = 1; i <= 3; i++) {
        if (ground_at(m, bx + i, by, z) == GROUND_ROAD) nx++; else break;
    }
    for (i = 1; i <= 3; i++) {
        if (ground_at(m, bx, by - i, z) == GROUND_ROAD) ny++; else break;
    }
    for (i = 1; i <= 3; i++) {
        if (ground_at(m, bx, by + i, z) == GROUND_ROAD) ny++; else break;
    }
    return (nx > ny) ? 64 : 0;
}

/* Speed, in 16.16 world pixels per tick, per unit of the car table's
 * `max_speed`. The table runs 5..50 and a saloon is 30; 3495 turns that into
 * 1.6 px a tick, which at the 50 Hz simulation rate is 80 world pixels or 2.5
 * blocks a second - a little faster than the player runs (2.03) and slow
 * enough to be followed on a 320x200 screen. A bus at 16 does 1.3 blocks.
 *
 * A NUMBER, not a physics model. The car table also holds mass, thrust and
 * tyre adhesion, all in 16.16 and all ready to use, and traffic that
 * accelerates properly wants them. It does not want them yet: a car that
 * cannot reach its own top speed before the next junction only makes the grid
 * following harder to judge. */
#define SPEED_UNIT 3495L

/* One step of a compass heading, in blocks. */
static void heading_step(int angle, int *dx, int *dy)
{
    switch (angle & 255) {
    case 0:   *dx =  0; *dy = -1; break;
    case 64:  *dx =  1; *dy =  0; break;
    case 128: *dx =  0; *dy =  1; break;
    default:  *dx = -1; *dy =  0; break;
    }
}

/* Does a vehicle this long, lying along `axis`, actually FIT here?
 *
 * The first version checked only the block a car was centred on, and on the
 * Amiga that produced the picture that made this function necessary: a fire
 * truck is 128 source pixels long - four blocks - so two of them ended up lying
 * across the pavement with their ladders inside a building. The centre block
 * was road, and every other block the vehicle covered was not.
 *
 * So the extent is walked. `length` is in world pixels and a block is 32 of
 * them, so a vehicle reaches (length / 2 + 31) / 32 blocks each way - rounded
 * UP, because half a block of bus still sticks out of the road.
 *
 * Only the LENGTH is checked, not the width: the widest thing in the table is
 * 50 pixels against a 32-pixel block, so half of it is 25 and a vehicle
 * centred in a lane never reaches into the next block sideways by enough to
 * matter. Length is where the problem was and length is what is tested. */
/* AND IT IS THE BODY THAT HAS TO FIT, NOT A WHOLE NUMBER OF BLOCKS.
 *
 * Rounding the reach UP and demanding road in every block touched was too
 * strict, and it did not show as a car in the wrong place - it showed as
 * traffic STOPPING. An ordinary 60-pixel car reaches 30 pixels from its
 * centre, less than a block, and the old rule still insisted the whole
 * neighbouring block be road. At the end of a street where the road turns, the
 * block beyond is pavement, so the move was refused, the car sat there for
 * good, and the whole lane queued behind it: three cars in the drive test were
 * held by "DEAD END" and two of them were buses.
 *
 * So the blocks the body FULLY covers must be road, and the one block at each
 * end that it only reaches into may be road or PAVEMENT - a nose over a kerb
 * at a corner is what cars in this game do anyway. A building is neither, so
 * the fire truck through the wall that this function was written for is still
 * refused. */
static int fits(const gta_map *m, int bx, int by, int z, int axis, int length)
{
    /* The body reaches `half` pixels each way from a car centred in its block,
     * and the first 16 of those are still inside that block. THE CENTRE BLOCK
     * MUST BE ROAD; everything the rest of the body touches may be road or
     * KERB, which is the same standard the off-road test in gtadump applies
     * (it accepts ground types 2 and 3). A building is neither, so the fire
     * truck with its ladder through a wall that this function was written for
     * is still refused.
     *
     * IT USED TO DEMAND ROAD EVERYWHERE, AND THAT DID NOT SHOW AS A CAR IN THE
     * WRONG PLACE - IT SHOWED AS TRAFFIC STOPPING. A bus cannot turn out of a
     * street whose corner block is pavement, so it stood at the end of that
     * street for ever and every car behind it queued: the drive test called it
     * "DEAD END" and every single one was a long vehicle. Being strict here is
     * not the safe direction; it is a different bug, further away from where
     * it is felt. */
    int half  = length / 2;
    int over  = half - 16;
    int reach = (over > 0) ? (over + 31) / 32 : 0;
    int i;

    if (ground_at(m, bx, by, z) != GROUND_ROAD)
        return 0;
    for (i = -reach; i <= reach; i++) {
        int cx = (axis == 0) ? bx : bx + i;
        int cy = (axis == 0) ? by + i : by;
        int g  = ground_at(m, cx, cy, z);
        if (g != GROUND_ROAD && g != GROUND_PAVEMENT)
            return 0;
    }
    return 1;
}

/* Defined with the driving code, used by the placement above it: can a vehicle
 * `length` long that arrived here heading `in` get out again? */
static int has_exit(const gta_map *m, int bx, int by, int z, int in, int length);

/* Also defined with the driving code and used by the placement: is this block
 * part of the road network, or of a pocket a car could never drive out of? */
static int can_get_away(const gta_map *m, int bx, int by, int z);

/* Does this block let traffic through on both axes? Defined with the driving
 * code; the lane geometry above needs it to tell a street from a crossing. */
static int is_junction(const gta_map *m, int bx, int by, int z);
static void box_build(const gta_nav *nav);

/* Host-side diagnostics only: gtadump sets this from GTA_TRACE_CAR and the
 * want ladder prints its inputs for that one car. Zero (the default)
 * costs one compare per car per tick and nothing else. */
unsigned long gta_traffic_trace_serial = 0;

/* Why the last claim_route() said no - read by the gate for the per-car
 * probe field. 0 granted, 15 somebody's claim, 16 a body on the path,
 * 17 straight landing not legal road, 18 no room to land, 19 forced
 * straight with no landing. */
static int g_claim_why = 0, g_claim_side = -9, g_claim_fell = 0;
/* join-candidacy outcomes, host diagnostics: [0] joined, [1] chained shape,
 * [2] tail gone/not crossing, [3] shape mismatch, [4] fairness valve,
 * [5] body in the way */
long gta_join_why[6];

void gta_traffic_init(gta_traffic *tr, const gta_tiles *t, unsigned long seed)
{
    memset(tr, 0, sizeof *tr);
    tr->tiles = t;
    /* THE THREE JUNCTION SWITCHES, AND WHY THEY ARE SET THE WAY THEY ARE.
     * All three were measured on 2026-08-24 over three sites and three seeds;
     * the numbers are in the notes. They go AFTER the memset, which is where
     * the first version of them did not - and a wiped switch is not a bug you
     * see, it is a bug you measure and believe. */
    tr->opt_cross_lock = 1;     /* the one bit: a turn is decided on ordinary
                                 * road, never inside the box */
    tr->opt_sweep      = 0;     /* OFF: the rectangle after the turn. It is
                                 * the narrowest version of the idea and the
                                 * closest to what was asked for, and it still
                                 * costs 14 points of flow at (64,64) while
                                 * DOUBLING the overlaps there - refused turns
                                 * queue on the approach and the queue is what
                                 * collides. Numbers in the notes (31). */
    tr->opt_creep      = GTA_ARC_CREEP;
    tr->opt_nooverlap  = 0;     /* OFF: see car_free_at(). 21324 car-ticks of
                                 * cars held exactly where they stood at one
                                 * site, 97% of the fleet moving down to 63%,
                                 * and the overlap count barely moved. The
                                 * rules above it have to be right; a veto at
                                 * the last moment only converts an overlap
                                 * into a stall. */
    tr->opt_arrows     = 1;     /* a turn obeys the block's own arrows */
    tr->opt_keepclear  = 1;     /* no turn into an exit lane with no room */
    tr->opt_lights     = 0;     /* invisible lights read as phantom stops */
    tr->opt_boxroot    = 0;     /* the entry test sees the whole crossing */
    tr->opt_holdbox    = 1;     /* one car crosses a junction at a time */
    tr->opt_occ_hold   = 1;     /* the corner's cells are TAKEN - one vehicle
                                 * per square, which is the whole point */
    tr->opt_occ_look   = 0;     /* the early refusal is the expensive half */
    tr->opt_occ        = 0;     /* OFF, and this is now a DECISION rather than
                                 * an open question - the notes (40) and (43).
                                 * Both of the changes that were prescribed for
                                 * it have been made and measured: re-booking
                                 * from where the car has got to, so the ground
                                 * behind frees itself, and the outer cell only
                                 * beside the middle of the arc. They take it
                                 * from 26/31/28% of the fleet moving to
                                 * 44/37/28% against 89/65/72% without it. Half
                                 * the city for a penetration count that moves
                                 * both ways. The remaining cost is inherent:
                                 * even three cells held for the length of a
                                 * corner is a quarter of a 2x2 crossing. The corner is
                                 * looked at early and booked at the commit,
                                 * exactly as specified, and it OVER-RESERVES:
                                 * five samples round the arc plus the outer
                                 * cells for a long vehicle is eight to twelve
                                 * blocks, which on a 2x2 crossing is most of
                                 * it, held from the commit until the car is
                                 * out the far side. Flow 77/67/68% -> 26/31/28%
                                 * and the penetration count went both ways.
                                 * The idea is right; the footprint is too big
                                 * and the hold too long. */
    tr->opt_occ_unused = 0;     /* OFF, and the numbers are in the notes
                                 * (39). The matrix is exactly the design that
                                 * was asked for and it does not pay in either
                                 * scope. Junction cells only: penetration 2126
                                 * -> 1328 at one site but 516 -> 1093 and 941
                                 * -> 1510 at the other two, flow -7/-10/-15.
                                 * Every block: 516 -> 126 at one site, but
                                 * 2126 -> 2621 at another and the flow falls
                                 * to 39/28/47%. A 32-pixel cell cannot express
                                 * where a 32x12 car is: one car per block
                                 * loses a whole block to a small vehicle, and
                                 * the penetration that is left is LATERAL,
                                 * between lanes, which no block grid can
                                 * see. */
    tr->opt_body       = 0;     /* OFF. Asking block occupancy by the whole
                                 * BODY instead of the centre is correct and it
                                 * does NOT fix the fault it was written for:
                                 * drive-through car-ticks 2126 -> 1981, 516 ->
                                 * 897, 941 -> 840 - noise - while the flow at
                                 * two sites fell 67% -> 49% and 68% -> 29%.
                                 * A stopped vehicle blocking a whole junction
                                 * arm with its overhang costs more than the
                                 * penetration it prevents. */
    tr->opt_arcclaim   = 0;     /* OFF. A turn booking the crossing it drives
                                 * into is the leak the box instrument found,
                                 * and closing it does not pay: overlaps at
                                 * (64,64) 7 -> 11 and the flow at (204,108)
                                 * 34% -> 29%, against overlaps 17 -> 13 there.
                                 * The claim is one per junction ROOT and a
                                 * root is up to four blocks across, so making
                                 * a turner hold the whole crossing shuts out
                                 * the arms it is nowhere near. The finer
                                 * version - claim the BLOCKS the arc sweeps -
                                 * is the next thing to try; see LEFTOFF. */
    tr->opt_boxgap     = 0;     /* OFF. The rectangle as a CONTINUOUS brake
                                 * cannot be made to work: symmetric, both
                                 * cars stop for each other and neither ever
                                 * starts (15% of the fleet moving, 43% of all
                                 * car-ticks held); asymmetric, the one with
                                 * right of way drives into the one that gave
                                 * it (overlaps 7 -> 13). It survives as a
                                 * ONE-SHOT test at the moment a turn is
                                 * committed, which is arc_clear(). */
    tr->opt_horizon    = GTA_TRAFFIC_HORIZON;
    tr->opt_unwedge    = 0;     /* OFF, and this one was MINE and it was the
                                 * pile-up in the developer's screenshot: a car
                                 * wedged for six seconds pushed through
                                 * whatever was in front of it. Overlaps at the
                                 * worst site 67 -> 17 with it off, and the
                                 * flow did not move (35% -> 34%). */
    /* Zero is a fixed point of the multiply-only part of an LCG and would make
     * every run identical in the wrong way, so it is replaced rather than
     * trusted. */
    tr->seed = seed ? seed : 1UL;
    /* The default zoom, until somebody says otherwise. See the note on
     * gta_traffic_despawn_blocks(): at 32 pixels a block a 320-wide screen is
     * five blocks either side of the camera. */
    tr->view_blocks = 5;
    tr->fleet_cap = GTA_MAX_CARS;
}

void gta_traffic_set_nav(gta_traffic *tr, const gta_nav *nav)
{
    tr->nav = nav;
    g_nav = nav;
    box_build(nav);                     /* the junction-box bitmap */
}

void gta_traffic_set_view_blocks(gta_traffic *tr, int blocks)
{
    /* Clamped low so a silly zoom cannot collapse the ring onto the camera and
     * spawn cars on top of the player, and high so a wide one cannot make the
     * band scan walk the whole map every half second. */
    if (blocks < 3)  blocks = 3;
    if (blocks > 24) blocks = 24;
    tr->view_blocks = blocks;
}

/* Is there room here for a vehicle `length` long, facing `angle`?
 *
 * PLACEMENT HAS TO KNOW ABOUT LENGTH, and the first version did not: it only
 * refused a block that already held a car. Blocks are 32 pixels and a bus is
 * 120, so cars were routinely put down one block apart needing four, and the
 * overlap test found them immediately - "0 px apart, need 90", "32 px apart,
 * need 62". The distances are all multiples of 32, which is what named it as a
 * placement fault rather than a driving one.
 *
 * It matters more than it looks: a car has no reverse gear here, so two
 * vehicles that START inside each other never separate. The follow logic can
 * keep a gap; it cannot open one. */
static int drivable(const gta_map *m, int bx, int by, int z);
static int in_view(const gta_traffic *tr, int bx, int by);

/* IS THERE ANYWHERE TO BACK INTO? The original's stuck handler, state 1.
 *
 * The original checks five blocks behind; this checks two, because our blocks
 * are half the size in world pixels and a car is about one block long either
 * way. Each must be drivable and hold nobody. */
static int back_out_clear(const gta_traffic *tr, const gta_map *m, int idx,
                          int bx, int by)
{
    const gta_car *c = &tr->cars[idx];
    int dx, dy, k, i;

    heading_step((c->angle + 128) & 255, &dx, &dy);
    for (k = 1; k <= 2; k++) {
        int nx = bx + dx * k, ny = by + dy * k;
        if (!drivable(m, nx, ny, c->layer))
            return 0;
        for (i = 0; i < tr->n; i++) {
            const gta_car *o = &tr->cars[i];
            if (i == idx || o->done || o->layer != c->layer)
                continue;
            if ((int)(o->x >> (FP + 5)) == nx && (int)(o->y >> (FP + 5)) == ny)
                return 0;
        }
    }
    return 1;
}

/* Any car's CENTRE on this block, moving or not? The stopping squares of a
 * booking - the landing, the neutral strip of a chain - are places a car
 * may legally STOP (a refused gate stops it exactly there), so the
 * moving-cars-will-vacate exemption of block_full() does not hold for
 * them: a chain granted "through" a moving car whose own gate then
 * refused it made a deadlock ring at (80,18) - the owner could not drive
 * through the body, the body could not leave the owner's boxes. */
static int centre_on(const gta_traffic *tr, int skip, int bx, int by, int z)
{
    int i;

    for (i = 0; i < tr->n; i++) {
        const gta_car *o = &tr->cars[i];

        if (i == skip || o->done || o->layer != z)
            continue;
        if ((int)(o->x >> (FP + 5)) == bx &&
            (int)(o->y >> (FP + 5)) == by)
            return 1;
    }
    return 0;
}

/* A body on this block - optionally only stopped ones, optionally
 * exempting the members of one convoy (the joiner drives up behind its
 * own leader; the leader's body is not an obstacle, it is the queue). */
static int body_on_sq(const gta_traffic *tr, int skip, int bx, int by,
                      int z, int stopped_only, unsigned long exempt)
{
    int i;

    for (i = 0; i < tr->n; i++) {
        const gta_car *o = &tr->cars[i];

        if (i == skip || o->done || o->layer != z)
            continue;
        if (stopped_only && o->speed > 0)
            continue;
        if (exempt != 0 && (o->serial == exempt || o->convoy == exempt))
            continue;
        if ((int)(o->x >> (FP + 5)) == bx &&
            (int)(o->y >> (FP + 5)) == by)
            return 1;
    }
    if (tr->pl_active && tr->pl_layer == z &&
        !(stopped_only && tr->pl_speed > 0) &&
        (int)(tr->pl_x >> (FP + 5)) == bx &&
        (int)(tr->pl_y >> (FP + 5)) == by)
        return 1;
    return 0;
}

/* Is anyone still RIDING this route id - a committed car other than the
 * tail itself? While one is, the route's claims are load-bearing: the
 * tail aborting (stuck-backing, a stray) or finishing early must not
 * strand the members inside the box without their protection. */
static int convoy_riding(const gta_traffic *tr, unsigned long id)
{
    int i;

    for (i = 0; i < tr->n; i++)
        if (!tr->cars[i].done &&
            tr->cars[i].crossing &&
            tr->cars[i].serial != id &&
            tr->cars[i].convoy == id)
            return 1;
    return 0;
}

/* Any live booking on this block? The spawner asks - a car placed onto a
 * claimed square deadlocks the owner and itself in one move. */
static int claim_any(const gta_traffic *tr, int bx, int by, int z)
{
    int i;

    for (i = 0; i < tr->claim_top; i++)
        if (tr->claim_ttl[i] > 0 &&
            (int)tr->claim_x[i] == bx &&
            (int)tr->claim_y[i] == by &&
            (int)tr->claim_z[i] == z)
            return 1;
    return 0;
}

static int room_for(const gta_traffic *tr, int skip, long x, long y, int layer,
                    int angle, int length, long slack)
{
    int i, ns = ((angle & 127) == 0);

    for (i = 0; i < tr->n; i++) {
        const gta_car *o = &tr->cars[i];
        long along, side, need;

        /* `skip` is the car ASKING. Without it a moving car blocks itself: it
         * sits a block behind the target and is of course within its own
         * length of it, so every car in the city stopped dead - the driving
         * test reported "0 world px moved" for every seed at once, which is
         * what a self-check looks like as opposed to a traffic jam. */
        if (i == skip || o->done || o->layer != layer)
            continue;

        along = ns ? (o->y - y) : (o->x - x);
        side  = ns ? (o->x - x) : (o->y - y);
        if (along < 0) along = -along;
        if (side  < 0) side  = -side;

        /* Anything in a different lane is not in the way, whichever way it is
         * pointing - except that a car sitting in the SAME block is, always. */
        if (side > (16L << FP) && along > (16L << FP))
            continue;

        need = (((long)length / 2 + (long)gta_car_world_len(&tr->tiles->cars[o->model]) / 2)
                << FP) + slack;
        if (side <= (16L << FP) && along < need)
            return 0;
    }
    return 1;
}

/* IS ANYTHING COMING THE OTHER WAY? The original's give-way rule.
 *
 * The original checks a corner with TWO different questions and they have two
 * different consequences, which is the distinction this port collapsed:
 *
 *   the lane scan       3 blocks INTO the lane being entered, any car at all.
 *                       Chooses the speed the corner is taken at (2 or 5) and,
 *                       on failure, re-plans one block further along. It does
 *                       NOT stop the car.
 *   the give-way scan   4 blocks back down the OPPOSITE lane, and a car there
 *                       counts only if it is MOVING.
 *                       THIS is the one that brakes.
 *
 * So the give-way is about oncoming traffic, not about the exit being full. A
 * queue in the exit lane is somebody else's problem and the bumper gap already
 * deals with it; an occupied-exit rule that STOPS the car deadlocks, because
 * the cars filling that exit are themselves waiting to turn. Measured: making
 * an occupied exit brake took (204,108) from 69% of the fleet moving to 33%
 * with 22 cars abandoned, while fixing the fault it was aimed at.
 *
 * A PARKED CAR DOES NOT STOP YOU CROSSING IT, which is the whole point of the
 * `moving` test - Liberty City is lined with parked cars and a give-way that
 * counted them would never release. */
/* DO TWO CARS OVERLAP, as oriented boxes rather than as circles?
 *
 * A separating-axis test on the four box normals. Cars in this game are up to
 * 60 world pixels long and 25 wide, so a circle test either misses a car lying
 * alongside or refuses one that is nowhere near - and the case that matters
 * here is exactly the awkward one, a car half way round a corner lying
 * diagonally across a vehicle waiting at the line.
 *
 * Directions are Q14 from the cosine table; extents are whole world pixels.
 * The largest product is 200 * 16384, so this is safe in 32 bits. */
static int box_hit(long ax, long ay, int aang, int ahl, int ahw,
                   long bx_, long by_, int bang, int bhl, int bhw)
{
    long dx = (bx_ - ax) >> FP, dy = (by_ - ay) >> FP;
    long axis[4][2];
    long lim;
    int k;

    /* THE FAST REJECT, and it is exact, not approximate. A box's projection
     * on any world axis is hl*|cos| + hw*|sin| <= hl + hw, so centres further
     * apart than the two sums on either world axis cannot touch on any axis -
     * geometric disjointness, which is precisely when the SAT below answers 0.
     * Same result, ~5 instructions instead of ~190. It matters because
     * occ_rebuild asks this question for the 3x3 blocks around every car on
     * every tick, and most of those blocks are nowhere near the body:
     * measured by callgrind, box_hit was 15.7% of the whole program and
     * 100% of it arrived through car_on_block. */
    lim = (long)(ahl + ahw + bhl + bhw);
    if (dx > lim || dx < -lim || dy > lim || dy < -lim)
        return 0;

    axis[0][0] =  gta_sin(aang); axis[0][1] = -gta_cos(aang);
    axis[1][0] =  gta_cos(aang); axis[1][1] =  gta_sin(aang);
    axis[2][0] =  gta_sin(bang); axis[2][1] = -gta_cos(bang);
    axis[3][0] =  gta_cos(bang); axis[3][1] =  gta_sin(bang);

    for (k = 0; k < 4; k++) {
        long nx = axis[k][0], ny = axis[k][1];
        long r  = dx * nx + dy * ny;              /* Q14 pixels */
        long pa, pb, t;

        if (r < 0) r = -r;

        t = (axis[0][0] * nx + axis[0][1] * ny) >> 14;
        pa = (t < 0 ? -t : t) * ahl;
        t = (axis[1][0] * nx + axis[1][1] * ny) >> 14;
        pa += (t < 0 ? -t : t) * ahw;

        t = (axis[2][0] * nx + axis[2][1] * ny) >> 14;
        pb = (t < 0 ? -t : t) * bhl;
        t = (axis[3][0] * nx + axis[3][1] * ny) >> 14;
        pb += (t < 0 ? -t : t) * bhw;

        if (r > pa + pb)
            return 0;                             /* an axis separates them */
    }
    return 1;
}

/* THE 32-BIT HEADROOM, MEASURED RATHER THAN ARGUED ABOUT.
 *
 * `long` is 64 bits on the host and 32 on the Amiga, so an overflow that would
 * wreck the game on the target passes every host test in silence. Building the
 * host tools with -DGTA_RANGE_CHECK makes every intermediate product of the
 * collision arithmetic report its magnitude, and `gtadump ramsweep` then prints
 * the largest one the whole 2888-pair sweep reached. Off by default and gone
 * entirely from the Amiga build - RCHK is the identity there. */
#ifdef GTA_RANGE_CHECK
long gta_rchk_max = 0;
const char *gta_rchk_where = "none";
static long rchk_(long v, const char *where)
{
    long a = v < 0 ? -v : v;
    if (a > gta_rchk_max) { gta_rchk_max = a; gta_rchk_where = where; }
    return v;
}
#define RCHK(v, w)  rchk_((v), (w))
#else
#define RCHK(v, w)  (v)
#endif

/* A 16.16 magnitude against a Q14 unit axis, back in 16.16. Shifting the
 * magnitude by 7 first keeps the product inside 32 bits for anything up to a
 * few hundred pixels a tick while losing under a thousandth of a pixel. */
/* A Q14 sine/cosine against a 16.16 length, back in 16.16, sign-correct. */
#define UMUL_S(t, v)   (((long)(t) * ((v) >> 8)) >> 6)
#define NRAM_MUL(v, n)   ((RCHK(((v) >> 7) * (n), "NRAM_MUL")) >> 7)
/* No single push may move a car more than this in one tick - 16 world pixels,
 * half a block. */
#define NRAM_MAXPUSH     (16L << 16)
/* How fast two bodies have to be closing along an axis before that axis counts
 * as the one they hit each other on - an eighth of a pixel a tick, enough to
 * ignore the numerical wobble of a pair that is merely resting together. */
#define NRAM_CLOSING     (65536L / 8)

/* THE SAME TEST, BUT IT ANSWERS WHERE AND HOW DEEP.
 *
 * box_hit() above is the hot one - it runs for the 3x3 blocks around every car
 * on every tick and it must stay a yes/no. This is the cold one: a handful of
 * calls a tick, only from the player's collision, and it returns the MINIMUM
 * TRANSLATION VECTOR - the axis of least overlap and how far along it the two
 * boxes are inside each other.
 *
 * THAT IS THE DIFFERENCE BETWEEN PUSHING A CAR AND DRIVING THROUGH IT. The ram
 * code used to take the collision normal from the line between the two centres,
 * snapped to whichever world axis was longer. For two cars of the same size
 * that points roughly the right way. For a 60-pixel bus against a 30-pixel
 * saloon it does not: the bus's nose can be buried in the saloon's flank while
 * the centre-to-centre line still runs almost along the bus's own length, so
 * the "push" went forward, into the car, instead of sideways, out of it.
 *
 * The MTV has no such failure mode. The axis of least overlap between two boxes
 * IS the direction they have to move to stop touching, by construction, and the
 * depth is exactly how far. Directions are Q14 from the cosine table, extents
 * whole world pixels, and the returned depth is Q14 pixels.
 *
 * The returned axis always points FROM the a-box TOWARDS the b-box, so the
 * caller can push b along it and a against it without another sign test.
 *
 * ONE CORRECTION THE TEXTBOOK VERSION NEEDS HERE, and the ram test found it in
 * one run. "Least overlap" is only the contact direction while the overlap is
 * SHALLOW. A bus at eighteen pixels a tick buries a third of its nose in a
 * parked saloon in a single step, and past that depth the sideways overlap
 * (half-widths, 17 px) is smaller than the head-on one (half-lengths, 46 px) -
 * so the least-overlap axis flips ninety degrees and the collision starts
 * shoving the saloon SIDEWAYS out of the bus's path instead of forward. It
 * slides clear and the bus sails through the space it used to be in. Measured:
 * 3 ticks of contact, 759 px past the target, "DROVE THROUGH IT".
 *
 * The approach direction settles it. An axis the two bodies are not closing
 * along is not the axis they collided on, whatever its overlap - so only axes
 * with a positive closing speed are candidates, and the smallest overlap is
 * chosen among those. `rvx`/`rvy` is a's velocity less b's, 16.16. If nothing
 * is closing (a resting overlap, a pair being pushed apart by a third car) the
 * plain least-overlap answer stands. */
static int box_mtv(long ax, long ay, int aang, int ahl, int ahw,
                   long bx_, long by_, int bang, int bhl, int bhw,
                   long rvx, long rvy,
                   long *outx, long *outy, long *outdepth)
{
    long dx = (bx_ - ax) >> FP, dy = (by_ - ay) >> FP;
    long axis[4][2];
    long best = 0x7FFFFFFFL, bx2 = 0, by2 = 0;
    long cbest = 0x7FFFFFFFL, cx2 = 0, cy2 = 0;   /* best among CLOSING axes */
    long lim;
    int k;

    lim = (long)(ahl + ahw + bhl + bhw);
    if (dx > lim || dx < -lim || dy > lim || dy < -lim)
        return 0;

    axis[0][0] =  gta_sin(aang); axis[0][1] = -gta_cos(aang);
    axis[1][0] =  gta_cos(aang); axis[1][1] =  gta_sin(aang);
    axis[2][0] =  gta_sin(bang); axis[2][1] = -gta_cos(bang);
    axis[3][0] =  gta_cos(bang); axis[3][1] =  gta_sin(bang);

    for (k = 0; k < 4; k++) {
        long nx = axis[k][0], ny = axis[k][1];
        long r  = RCHK(dx * nx + dy * ny, "mtv r");   /* Q14 pixels */
        long sgn = r < 0 ? -1 : 1;
        long pa, pb, t, over;

        if (r < 0) r = -r;

        t = RCHK(axis[0][0] * nx + axis[0][1] * ny, "mtv dot") >> 14;
        pa = (t < 0 ? -t : t) * ahl;
        t = RCHK(axis[1][0] * nx + axis[1][1] * ny, "mtv dot") >> 14;
        pa += (t < 0 ? -t : t) * ahw;

        t = RCHK(axis[2][0] * nx + axis[2][1] * ny, "mtv dot") >> 14;
        pb = (t < 0 ? -t : t) * bhl;
        t = RCHK(axis[3][0] * nx + axis[3][1] * ny, "mtv dot") >> 14;
        pb += (t < 0 ? -t : t) * bhw;

        over = RCHK(pa + pb - r, "mtv over");
        if (over <= 0)
            return 0;                             /* an axis separates them */
        nx *= sgn;                                /* a -> b, always */
        ny *= sgn;
        if (over < best) {
            best = over;
            bx2 = nx;
            by2 = ny;
        }
        /* Are the two actually coming together along this one? */
        if (RCHK(((rvx >> 8) * nx) + ((rvy >> 8) * ny), "mtv close") >> 6
                > NRAM_CLOSING
            && over < cbest) {
            cbest = over;
            cx2 = nx;
            cy2 = ny;
        }
    }
    if (cbest != 0x7FFFFFFFL) {
        *outx = cx2;
        *outy = cy2;
        *outdepth = cbest;
    } else {
        *outx = bx2;
        *outy = by2;
        *outdepth = best;
    }
    return 1;
}

/* WHERE THE TURN PUTS THE CAR, PLUS THE ROAD IT NEEDS IN FRONT OF IT.
 *
 * "zeby dokonac skretu auto+prostokat po skrecie nie moze kolidowac z innym
 * autem"
 *
 * Exactly that, and nothing more. The box the car will occupy when the arc
 * ends, lengthened forward by GTA_TURN_CLEAR pixels so it is not committing
 * into the back of a queue, tested against every other vehicle where it is
 * now. Half way round the arc is checked too, because a long vehicle sweeps
 * ground the two ends do not cover.
 *
 * IT IS A ONE-SHOT TEST AND THAT IS WHY IT WORKS. Refusing a turn costs a tick
 * on the approach and the car asks again; refusing to MOVE, which is what the
 * same geometry does when it runs every tick, deadlocks two cars against each
 * other for ever - the numbers are on opt_boxgap. Patience
 * (GTA_SWEEP_PATIENCE) stops even the one-shot version waiting for something
 * that is never going to move. */
static int arc_clear(const gta_traffic *tr, int idx, long cx, long cy,
                     int r_px, int from_ang, int dir)
{
    const gta_car *c = &tr->cars[idx];
    const gta_car_info *info = &tr->tiles->cars[c->model];
    int ahl = gta_car_world_len(info) / 2;
    int ahw = gta_car_world_wid(info) / 2;
    int s;

    for (s = 2; s <= 4; s += 2) {           /* half way round, and the landing */
        int phi = s * (GTA_TURN_QUARTER / 4);
        int ang = (from_ang + dir * phi) & 255;
        int rd  = (ang - dir * GTA_TURN_QUARTER) & 255;
        long px = cx + (((long)r_px * gta_sin(rd)) << 2);
        long py = cy - (((long)r_px * gta_cos(rd)) << 2);
        /* The rectangle in front: the box is grown forwards by half the
         * clearance and its centre moved forward by the other half, which is
         * the same rectangle written as one box. */
        int reach = (s == 4) ? GTA_TURN_CLEAR : 0;
        int hl = ahl + reach / 2;
        int i;

        px += ((long)gta_sin(ang) * (reach / 2)) * 4;
        py -= ((long)gta_cos(ang) * (reach / 2)) * 4;

        for (i = 0; i < tr->n; i++) {
            const gta_car *o = &tr->cars[i];
            const gta_car_info *oi;
            long ddx, ddy;

            if (i == idx || o->done || o->layer != c->layer)
                continue;
            ddx = (px - o->x) >> FP; if (ddx < 0) ddx = -ddx;
            ddy = (py - o->y) >> FP; if (ddy < 0) ddy = -ddy;
            if (ddx > 120 || ddy > 120)
                continue;
            oi = &tr->tiles->cars[o->model];
            if (box_hit(px, py, ang, hl, ahw,
                        o->x, o->y, o->face,
                        gta_car_world_len(oi) / 2, gta_car_world_wid(oi) / 2))
                return 0;
        }
    }
    return 1;
}

/* WOULD THIS CAR'S BODY, PUT HERE, BE INSIDE ANOTHER CAR'S BODY?
 *
 * THE BACKSTOP. Every rule above this one - the follow gap, the junction
 * claim, the give-way, the arc's own clearance - is an attempt to arrange that
 * cars never want to be in the same place. They are all approximations and
 * they all have holes, and the hole shows on screen as two sprites drawn on
 * top of each other, which is one of the four faults being fixed and the one
 * that no amount of rule-tuning has closed.
 *
 * So the final position is simply tested. If the car cannot go where it wanted
 * to go, it stays where it is, and the rules above get another tick to sort
 * themselves out. That makes overlapping sprites IMPOSSIBLE rather than rare,
 * which is the difference between a fault that is fixed and one that is less
 * frequent.
 *
 * It is affordable because of the cheap reject: cars more than 80 pixels away
 * on either axis cost four subtractions. In a normal street that is everybody
 * but one or two. */
static int car_free_at(const gta_traffic *tr, int idx, long px, long py, int ang)
{
    const gta_car *c = &tr->cars[idx];
    const gta_car_info *info = &tr->tiles->cars[c->model];
    int ahl = gta_car_world_len(info) / 2;
    int ahw = gta_car_world_wid(info) / 2;
    int i;

    for (i = 0; i < tr->n; i++) {
        const gta_car *o = &tr->cars[i];
        const gta_car_info *oi;
        long ddx, ddy;

        if (i == idx || o->done || o->layer != c->layer)
            continue;
        ddx = (px - o->x) >> FP; if (ddx < 0) ddx = -ddx;
        ddy = (py - o->y) >> FP; if (ddy < 0) ddy = -ddy;
        if (ddx > 80 || ddy > 80)
            continue;
        oi = &tr->tiles->cars[o->model];
        if (box_hit(px, py, ang, ahl, ahw, o->x, o->y, o->face,
                    gta_car_world_len(oi) / 2, gta_car_world_wid(oi) / 2))
            return 0;
    }
    return 1;
}

/* IS THE NEXT FEW PIXELS OF THIS CAR'S ARC PHYSICALLY FREE?
 *
 * One sample, four pixels further round, tested as boxes. This is what lets a
 * car wedged half way through a crossing creep out of it without driving
 * THROUGH the vehicle in front of it: the wedge that locks a junction is
 * almost always cars waiting for each other with room to spare, and the few
 * that really are touching stay where they are until they are not.
 *
 * Without it the creep bought 20 points of flow at the cost of overlaps going
 * from 3-7 to 11-23 at the worst site, which is a bad trade - overlapping
 * sprites is one of the four faults being fixed, not a currency. */
/* Both defined further down, beside the junction rules they belong to. */
static int  car_on_block(const gta_traffic *tr, const gta_car *o,
                         int bx, int by);
static int  is_junction(const gta_map *m, int bx, int by, int z);
static void junction_root(const gta_map *m, int bx, int by, int z,
                          int *rx, int *ry);

static int arc_step_free(const gta_traffic *tr, int idx)
{
    const gta_car *c = &tr->cars[idx];
    const gta_car_info *info = &tr->tiles->cars[c->model];
    int ahl = gta_car_world_len(info) / 2;
    int ahw = gta_car_world_wid(info) / 2;
    long s = c->arc_s + (4L << FP);
    long phi, px, py;
    int ang, rd, i;

    if (c->turn == 0 || c->turn_radius <= 0)
        return 1;
    if (s > c->arc_len) s = c->arc_len;

    phi = ((s / c->turn_radius) * GTA_ARC_K) >> GTA_ARC_KSHIFT;
    if (phi > ((long)GTA_TURN_QUARTER << FP))
        phi = (long)GTA_TURN_QUARTER << FP;
    ang = (int)(((((long)c->turn_from) << FP) + (long)c->turn * phi) >> FP) & 255;
    rd  = (ang - c->turn * GTA_TURN_QUARTER) & 255;
    px  = c->arc_cx + (((long)c->turn_radius * gta_sin(rd)) << 2);
    py  = c->arc_cy - (((long)c->turn_radius * gta_cos(rd)) << 2);

    for (i = 0; i < tr->n; i++) {
        const gta_car *o = &tr->cars[i];
        const gta_car_info *oi;
        long ddx, ddy;

        if (i == idx || o->done || o->layer != c->layer)
            continue;
        ddx = (px - o->x) >> FP; if (ddx < 0) ddx = -ddx;
        ddy = (py - o->y) >> FP; if (ddy < 0) ddy = -ddy;
        if (ddx > 80 || ddy > 80)
            continue;
        oi = &tr->tiles->cars[o->model];
        if (box_hit(px, py, ang, ahl, ahw, o->x, o->y, o->face,
                    gta_car_world_len(oi) / 2, gta_car_world_wid(oi) / 2))
            return 0;
    }
    return 1;
}


/* THE CITY'S OWN TRAFFIC LIST, and it is not a filter.
 *
 * The port used to decide what may drive around by TESTING the car table -
 * class 4 is a car, so let it through - and that let the helicopter in (filed
 * as a bus), then the remote-control car (filed as a car, 29x30 px). Each one
 * needed its own exception. The original does nothing of the kind.
 *
 * The original carries a hard-coded table - three rows of one hundred model
 * ids, one row per city - and the ambient fleet is exactly that multiset.
 * The game shuffles its city's row once at level load,
 * builds a hundred dormant cars by walking the shuffled row, and from then on
 * the "spawner" only ever teleports a dormant car back into view: A GTA 1
 * TRAFFIC CAR NEVER CHANGES MODEL.
 *
 * So the list is a WEIGHTED one, and the weights are what make Liberty City
 * look like Liberty City: nineteen of its hundred cars are taxis, eleven are
 * Mundanos, and exactly one is a Beast GTS. Uniform choice over "everything
 * that looks like a car" gets the population wrong even once the strays are
 * excluded.
 *
 * Taken from the original's own table, and independently confirmed against a
 * full pass over its spawn code. Each row sums to 100.
 *
 * Everything absent is absent on purpose: police,
 * ambulance and fire truck have their own dispatchers, trains and trams are
 * placed from the map's object list, and the boat, tank, helicopter and RC car
 * simply never appear as traffic. */
static const short traffic_mix[3][65] = {
    /* style001, Liberty City - 30 models */
    {  0,1,  1,5,  2,1,  3,6,  6,1,  9,4, 14,2, 17,2, 18,11, 19,5,
      21,1, 22,19, 25,2, 26,2, 27,5, 28,1, 29,1, 31,3, 34,4, 35,1,
      41,2, 43,2, 44,2, 45,2, 46,2, 58,4, 70,4, 72,2, 76,2, 80,1, -1 },
    /* style002, San Andreas - 30 models */
    {  1,8,  3,4,  6,1,  7,2,  9,3, 19,7, 22,16, 31,5, 35,1, 41,2,
      43,2, 44,1, 45,1, 46,1, 50,2, 53,1, 54,10, 55,2, 58,3, 61,2,
      62,2, 63,1, 64,5, 65,4, 66,1, 70,5, 74,1, 76,1, 79,5, 86,1, -1 },
    /* style003, Vice City - 32 models */
    {  1,8,  3,8,  6,2,  7,2,  9,3, 19,6, 22,13, 26,2, 29,1, 35,1,
      41,3, 43,3, 44,2, 45,1, 46,2, 54,10, 62,2, 65,3, 70,6, 72,2,
      73,1, 74,1, 75,4, 76,2, 77,1, 78,2, 79,2, 80,2, 81,1, 82,1,
      83,1, 87,2, -1 }
};

/* The table names MODEL IDS; the rest of the port indexes RECORDS. */
static int record_of_model(const gta_tiles *t, int model_id)
{
    int i;
    for (i = 0; i < t->n_cars; i++)
        if (t->cars[i].model_id == model_id)
            return i;
    return -1;
}

/* Is this model something that belongs parked in a street? Kept as a sanity
 * check behind the table above - a record has to exist and be drawable before
 * anything else is worth asking. */
static int parkable(const gta_car_info *c)
{
    if (c->sprite_index < 0) return 0;
    /* THE HELICOPTER IS FILED AS A BUS. Model 88 in Carnage3D's table is
     * `eVehicle_Helicopter`, and the car table gives it vtype 0 - the same
     * class as a coach - so "is it a bus?" said yes and it joined the traffic,
     * driving down the road at 1.3 blocks a second. It is the only entry in
     * the file that is not a road vehicle yet passes the class test. */
    if (c->model_id == GTA_MODEL_HELICOPTER) return 0;

    /* AND THE REMOTE-CONTROL CAR IS FILED AS A CAR. The developer found it
     * driving the streets like a taxi: model 47, 29x30 source pixels - half
     * the length of a saloon and the only vehicle in the file under 58 - with
     * class 4, so the class test waves it through exactly as it did the
     * helicopter.
     *
     * THE FILE ITSELF SAYS WHICH VEHICLES A DRIVER CAN BE IN: `n_doors`. The
     * RC car has NONE, and so does the boat; every ordinary car, bus and
     * motorcycle in style001 has one or two. A vehicle with no door has no
     * way for a person to get into it, so it cannot be something the city's
     * traffic drives around in - which is the test, rather than a list of
     * model ids that would have to be maintained per city. */
    if (c->n_doors < 1) return 0;

    switch (c->vtype) {
    case GTA_VEH_CAR:
    case GTA_VEH_BUS:
    case GTA_VEH_BIKE:
        return 1;
    default:
        return 0;
    }
}

/* The body of both the initial population and the top-up. `ring_lo` is what
 * separates them: filling the city at start-up begins next to the camera, but
 * a car that APPEARS while the player is watching must not do so in view, so
 * the top-up starts outside the screen. */
static int park_band(gta_traffic *tr, const gta_map *m,
                     int bx, int by, int ring_lo, int ring_hi, int want)
{
    int ring, placed = 0;
    int n_models = 0, models[100];
    int i;

    if (!tr->tiles || tr->tiles->n_cars <= 0)
        return 0;

    /* THE CITY'S BAG OF A HUNDRED CARS, built once per call. Each model
     * appears as many times as the original's table gives it, so drawing
     * uniformly from the bag reproduces the original's weights exactly -
     * nineteen taxis in a hundred, one Beast GTS - without a rejection loop,
     * which is not something to put on a 68020. */
    {
        int style = m->style_number;
        const short *mix;
        int p;

        if (style < 1 || style > 3) style = 1;
        mix = traffic_mix[style - 1];
        for (p = 0; mix[p] >= 0 && n_models < 100; p += 2) {
            int rec = record_of_model(tr->tiles, mix[p]);
            int w = mix[p + 1];
            if (rec < 0 || !parkable(&tr->tiles->cars[rec]))
                continue;
            while (w-- > 0 && n_models < 100)
                models[n_models++] = rec;
        }
    }
    if (n_models == 0)
        return 0;

    /* Outward in rings from the camera block, so the cars that do get placed
     * are the ones nearest the player - the same traversal order the renderer
     * uses, and for the same reason: what is close matters most. */
    for (ring = ring_lo; ring <= ring_hi && placed < want; ring++) {
        int side, step;
        for (side = 0; side < 4 && placed < want; side++) {
            for (step = -ring; step <= ring && placed < want; step++) {
                int cx, cy, z, axis, ang, mi;
                gta_car *car;
                const gta_car_info *info;
                long ox, oy;
                int lane_tgt = GTA_LANE_TARGET;

                switch (side) {
                case 0: cx = bx + step; cy = by - ring; break;
                case 1: cx = bx + step; cy = by + ring; break;
                case 2: cx = bx - ring; cy = by + step; break;
                default:cx = bx + ring; cy = by + step; break;
                }

                if (tr->n >= GTA_MAX_CARS)
                    return placed;

                z = road_layer(m, cx, cy);
                if (z < 0)
                    continue;

                /* Thin the street out, or every road block gets a car and the
                 * city looks like a car park. Three quarters are skipped. */
                if ((next_rand(tr) & 3) != 0)
                    continue;

                /* Not on a junction: a car parked across a crossroads is the
                 * one placement a person notices immediately.
                 *
                 * AND "A JUNCTION IS A BLOCK WITH ROAD ON ALL FOUR SIDES" IS
                 * NOT WHAT A JUNCTION IS. That was the test here, and it is
                 * also true of every INNER LANE of a road wider than two
                 * blocks: its neighbours up and down the street are road, and
                 * so are the lanes either side of it. So on a four-lane avenue
                 * the two middle lanes never received a car - in the game
                 * almost every vehicle the player sees was placed by this
                 * function rather than driven here, which is why the report
                 * was "nic nie wjezdza na skrzyzowanie z 2. alejki z zadnego
                 * kierunku" while the host drive test, which parks once and
                 * then drives for twelve thousand ticks, showed both lanes in
                 * use and found nothing.
                 *
                 * is_junction() is the engine's own test - the block permits
                 * both a north/south and an east/west direction - and it is
                 * what every other rule in this file means by a junction. */
                if (is_junction(m, cx, cy, z))
                    continue;

                /* The map's own direction bits first; the shape heuristic only
                 * where the block carries none. A car now faces a direction
                 * traffic is actually allowed to travel in, rather than one of
                 * the two the street's shape permits. */
                ang = road_heading(m, cx, cy, z, tr);
                if (ang < 0) {
                    axis = road_axis(m, cx, cy, z);
                    /* Half of them point the other way down the street. */
                    ang = axis + (int)((next_rand(tr) & 1) << 7);
                } else {
                    axis = ((ang & 127) == 64) ? 64 : 0;
                }

                /* Pick a model that FITS. A few tries rather than a search:
                 * most of the table is saloons, most streets take a saloon, and
                 * a fixed small number of attempts keeps this bounded on a
                 * 68020. If none of them fits, the block gets no car - which is
                 * correct, because a place where a bus does not fit and four
                 * random cars did not come up is a place worth leaving empty. */
                {
                    int tries;
                    int reach = -1;     /* can_get_away(), computed lazily */
                    long px = ((long)cx * 32 + 16) << FP;
                    long py = ((long)cy * 32 + 16) << FP;
                    mi = -1;
                    for (tries = 0; tries < 4; tries++) {
                        int cand = models[next_rand(tr)
                                          % (unsigned long)n_models];
                        int len = gta_car_world_len(&tr->tiles->cars[cand]);
                        if (!fits(m, cx, cy, z, axis, len))
                            continue;
                        /* AND IT MUST BE ABLE TO DRIVE AWAY FROM HERE.
                         *
                         * Fitting is not enough: a bus dropped at the end of a
                         * street it cannot turn out of never moves again, and
                         * everything that comes down that lane queues behind
                         * it. Every remaining stall in the drive test was one
                         * of those, and every one of them was a bus. The
                         * placement is what has to know - the driving code
                         * cannot rescue a vehicle from a street it should
                         * never have been put in. */
                        if (!has_exit(m, cx, cy, z, ang, len))
                            continue;
                        /* And there must be ROOM - not just an empty block.
                         * See room_for(): a bus is four blocks long and this
                         * used to put one down a block behind another. */
                        if (!room_for(tr, -1, px, py, z, ang, len, GTA_TRAFFIC_MIN_GAP))
                            continue;
                        /* AND NOBODY'S BOOKING MAY COVER IT. The chain
                         * through a close junction pair claims the NEUTRAL
                         * square between the boxes - ordinary road, which
                         * the junction test above does not skip - and a car
                         * spawned onto such a claim is a deadlock ring made
                         * to order: the chain's owner cannot drive through
                         * the body, the body cannot leave (the boxes either
                         * side are the owner's), and nothing times out.
                         * Measured at (80,18): 75 s standing, flow 56%. */
                        if (claim_any(tr, cx, cy, z))
                            continue;
                        /* AND THE CAR MUST BE ABLE TO GET AWAY FROM HERE AT
                         * ALL - not just off this block, which `has_exit`
                         * already asked, but out into the city. See
                         * can_get_away(): the map has pockets that pass every
                         * other test and that a car can never leave.
                         *
                         * Computed once for the block rather than once per
                         * model, because it does not depend on the vehicle,
                         * and only after everything cheaper has passed. A
                         * pocket is a property of the place, so if it fails
                         * there is no point trying another model here. */
                        if (reach < 0)
                            reach = can_get_away(m, cx, cy, z);
                        if (!reach)
                            break;
                        mi = cand;
                        break;
                    }
                    if (mi < 0)
                        continue;
                }
                info = &tr->tiles->cars[mi];

                /* ON ITS LANE, which is where the lane keeper will hold it -
                 * so it is put there rather than made to twitch onto it over
                 * its first few ticks. The rule is the one in drive_one and
                 * has to stay the same rule: block centre, moved away from the
                 * kerb where there is a kerb on exactly one side. See
                 * gta_car.lane_target. */
                {
                    int lo, hi, tgt;
                    if (axis == 64) {           /* east-west: kerbs are y */
                        lo = ground_at(m, cx, cy - 1, z) == GROUND_ROAD;
                        hi = ground_at(m, cx, cy + 1, z) == GROUND_ROAD;
                    } else {                    /* north-south: kerbs are x */
                        lo = ground_at(m, cx - 1, cy, z) == GROUND_ROAD;
                        hi = ground_at(m, cx + 1, cy, z) == GROUND_ROAD;
                    }
                    tgt = GTA_LANE_TARGET;
                    if (!lo && hi)      tgt += GTA_LANE_KERB;
                    else if (lo && !hi) tgt -= GTA_LANE_KERB;
                    lane_tgt = tgt;
                    ox = oy = 0;
                    if (axis == 64) oy = ((long)(tgt - GTA_LANE_TARGET)) << FP;
                    else            ox = ((long)(tgt - GTA_LANE_TARGET)) << FP;
                }

                car = &tr->cars[tr->n++];
                car->serial = ++tr->next_serial;
                car->convoy = car->serial;
                car->book_lx = -1;
                car->book_ly = -1;
                car->x     = (((long)cx * 32 + 16) << FP) + ox;
                car->y     = (((long)cy * 32 + 16) << FP) + oy;
                car->layer = z;
                car->angle = ang & 255;
                car->model = mi;
                car->done  = 0;
                /* The car's own top speed, scaled to world pixels per tick.
                 * The table's numbers run 5..50 and a saloon is 30, which
                 * SPEED_UNIT turns into about 2.5 blocks a second - faster
                 * than the player runs (2.03) and slow enough to follow. It
                 * starts AT that speed rather than from rest: a car appearing
                 * seven blocks away has notionally been driving all along. */
                car->top   = (long)info->max_speed * SPEED_UNIT;
                /* AND CAPPED AT THE ORIGINAL'S CRUISE. Its traffic drives at 6
                 * of its speed units whatever the model; keeping the table's
                 * number below that cap is what makes a bus slower than a
                 * saloon without letting a sports car tear through the city at
                 * fifty. */
                if (car->top > (long)GTA_SPEED_CRUISE * GTA_SPEED_UNIT)
                    car->top = (long)GTA_SPEED_CRUISE * GTA_SPEED_UNIT;
                car->speed = car->top;
                /* Its OWN acceleration and braking, out of the original's car
                 * table. See GTA_TRAFFIC_ACCEL_UNIT: a saloon comes out at
                 * exactly the single rate this used to have for everything,
                 * and a bus at 40% of it. */
                car->accel = info->accel   ? (long)info->accel * GTA_TRAFFIC_ACCEL_UNIT
                                           : GTA_TRAFFIC_ACCEL;
                car->brake = info->braking ? (long)info->braking * GTA_TRAFFIC_BRAKE_UNIT
                                           : GTA_TRAFFIC_BRAKE;
                car->face  = ang & 255;
                /* Aim at the block it is standing in, so the first tick asks
                 * for a heading straight away instead of driving off the grid.
                 */
                car->cell_x = cx;
                car->cell_y = cy;
                car->tx = car->x;
                car->ty = car->y;
                /* Steering state: it starts pointing the way the lane goes and
                 * with no turn under way, and no route - the first tick asks
                 * for one. */
                car->turn = 0;
                car->turn_accum = 0;
                car->turn_frac = 0;
                car->turn_radius = GTA_TURN_RADIUS;
                car->since_turn = GTA_AFTER_TURN;   /* not just out of a corner */
                car->turn_from = ang & 255;
                /* AND EVERY OTHER PIECE OF PER-CAR STATE, because this slot is
                 * not blank. The fleet array is COMPACTED when cars are
                 * retired, so `tr->cars[tr->n]` holds whatever the last
                 * vehicle to occupy that slot left behind - a half-finished
                 * lane correction, a wait counter three seconds deep, a turn
                 * lock from a junction on the other side of the city. Each of
                 * those makes a brand-new car behave as though it had been
                 * driving for a minute, which is the kind of fault that shows
                 * up as "sometimes a car does something odd right after it
                 * appears" and cannot be reproduced on purpose. */
                car->turn_lock = 0;
                car->crossing = 0;
                /* AND THE PLAYER'S OWN MARKS, which the list above never
                 * covered. When he gets out, gta_traffic_abandon() parks his
                 * car in the LAST slot; the next compaction copies that slot
                 * down and leaves the stale struct where tr->n now points -
                 * and this spawner then reused it with `abandoned` still set.
                 * The new car was born parked: never driven (drive_car()
                 * returns at once for an abandoned car), never recycled (the
                 * slot recycler refuses "the player's" cars), a dead vehicle
                 * in the road for the rest of the session, and one more every
                 * time it happened. The autodrive exit film caught it:
                 * "fleet[19] abandoned model 1 at (1940,1264)" four ticks
                 * after the compaction, for a car nobody had got out of
                 * (PROGRESS.md 113). Damage and the knock state are the same
                 * kind of leftover. */
                car->abandoned = 0;
                car->damage = 0;
                car->knock = 0;
                car->recover = 0;
                car->allow_turn = 1;
                car->lane_bx = cx;
                car->lane_by = cy;
                car->appr_bx = cx;
                car->appr_by = cy;
                car->odo = 0;
                car->turn_free_at = 0;
                car->cross_lock_x = -1;
                car->cross_lock_y = -1;
                car->cross_lock_dir = 0;
                car->arc_s = 0;
                car->arc_len = 0;
                car->lane_fix = 0;
                car->lane_target = lane_tgt;
                car->wait = 0;
                car->hold = GTA_HOLD_NONE;
                car->at_light = 0;
                car->reverse = 0;
                car->path_n = 0;
                car->path_i = 0;
                car->want_route = 1;
                car->route_cool = 0;
                car->hint_bx = -1;
                car->hint_by = -1;
                car->hint_val = 0;
                {
                    /* the first overlay colour no living car wears */
                    int ci, used, cand;
                    for (cand = 0; cand < 20; cand++) {
                        used = 0;
                        for (ci = 0; ci < tr->n; ci++)
                            if (!tr->cars[ci].done &&
                                &tr->cars[ci] != car &&
                                tr->cars[ci].ov_col == (unsigned char)cand) {
                                used = 1;
                                break;
                            }
                        if (!used)
                            break;
                    }
                    car->ov_col = (unsigned char)
                        (cand < 20 ? cand : (int)(car->serial % 20UL));
                }
                car->last_bx = cx;
                car->last_by = cy;
                car->dest_x = cx;
                car->dest_y = cy;

                /* remap8 is a list of palettes this model may wear, and a zero
                 * entry means "no remap" rather than "palette 0" - half the
                 * table is zeroed. Pick among the non-zero ones, or none. */
                car->remap = -1;
                {
                    int r, nr = 0, ok[GTA_CAR_REMAPS];
                    for (r = 0; r < GTA_CAR_REMAPS; r++)
                        if (info->remap8[r])
                            ok[nr++] = r;
                    if (nr > 0)
                        car->remap = ok[next_rand(tr) % (unsigned long)nr];
                }

                placed++;
            }
        }
    }
    return placed;
}

int gta_traffic_park(gta_traffic *tr, const gta_map *m,
                     int bx, int by, int radius, int want)
{
    return park_band(tr, m, bx, by, 1, radius, want);
}

/* --- driving ------------------------------------------------------------- */

/* Can a car be on this block at all?
 *
 * ONE INDEXED BYTE where the navigation grid is present, because the grid
 * zeroes railway blocks at build time (gta_nav.c) - it used to ask
 * `is_railway()` as well, and that walks a map column. With the ramp rule
 * asking about three layers at every step that column walk was a fifth of the
 * whole traffic tick on the 68020. The fallback below is for the case where
 * there is no grid at all, which is a diagnostic path. */
static int drivable(const gta_map *m, int bx, int by, int z)
{
    if (g_nav && g_nav->b)
        return gta_nav_ground(gta_nav_at_m(g_nav, bx, by, z)) == GROUND_ROAD;
    return ground_at(m, bx, by, z) == GROUND_ROAD && !is_railway(m, bx, by, z);
}

/* WHERE THE LANE SITS INSIDE A BLOCK, in world pixels across it, for traffic
 * travelling `dir` - the block centre, moved away from the kerb where there is
 * a kerb on exactly one side. See gta_car.lane_target for why the offset is
 * real and why it must never be sampled inside a junction.
 *
 * It is a function rather than inline code because TWO things need the same
 * answer and they have to agree exactly: the lane keeper, which holds a car on
 * the line, and the turn trigger, which has to know where the line IS in order
 * to put the arc down on it. They did not agree before - the trigger knew
 * nothing about lane targets at all - and that is what made a car come out of
 * every corner beside its lane and then slide across to it. */
/* A NOTE ON THE 1.5 PX THAT IS LEFT, so it is not re-derived a fourth time.
 *
 * The kerb test asks what lies beside a block. Beside a block that adjoins a
 * junction is more junction, which is road, so it answers "another lane that
 * way" and centres the car; one block further along the same street there is
 * pavement instead and the answer moves by GTA_LANE_KERB. Neither answer is
 * wrong - the lane really is wider at the crossing - but applying both, four
 * pixels apart, to a car driving straight is a visible slide.
 *
 * Refusing to sample such blocks was tried, in the turn aim alone and in both
 * the aim and the keeper, and made things worse each time (39419 -> 41497 and
 * -> 45990 px of corner correction). Holding a value across the crossing only
 * moves the disagreement to where the car picks the value up again. The two
 * have to become ONE mechanism - a lane line that belongs to the STREET rather
 * than being recomputed per block - and that is a bigger change than any of
 * the patches tried here. */
static int lane_target_at(const gta_map *m, int bx, int by, int z, int dir)
{
    int lo, hi, t = GTA_LANE_TARGET;

    /* ASK THE STREET, NOT THE CROSSING.
     *
     * The test below is "is there a lane beside me": no road on one side means
     * a kerb, and the drivable half of an outer lane is not centred on its
     * block. On an ordinary street that is exactly right. ON A JUNCTION BLOCK
     * IT IS A LIE, because there is road on all four sides, so every crossing
     * reads as a centred middle lane - and a car aiming its corner at that
     * lands GTA_LANE_KERB out from where the street's own line is.
     *
     * That is the fault as it is actually seen, and the two measurements only
     * agree once they are read together: the landing error is about three
     * pixels, and the lane keeper's dead band is two, so the car does not
     * correct - it lands beside its lane and STAYS there. "Cars come off the
     * junction on the wrong line" is literal; the sliding is the smaller half
     * of it.
     *
     * A junction block cannot answer for itself, so it asks the street it
     * belongs to, by walking ALONG `dir` until it is off the crossing. That is
     * sound because a step along the direction of travel does not change the
     * cross axis: going north keeps the same column, and the column is what
     * decides which side the kerb is on. So the block a few steps ahead
     * answers this block's question exactly.
     *
     * IT GOES HERE, WHERE EVERYONE SHARES IT. Four earlier attempts (LEFTOFF
     * item 1a) each patched ONE caller - the aim, or the keeper - and every
     * one of them lost, because moving the aim while the keeper still read the
     * block underneath only relocates the disagreement. The turn geometry, the
     * aim and the lane keeper all come through this function.
     *
     * The walk runs only on junction blocks and stops at the first block that
     * is not one. */
    /* AND THE BLOCK NEXT TO A CROSSING LIES TOO, which is the half of this
     * that a junction test alone misses. The question is about the block's
     * CROSS neighbours, so a block one step clear of the junction still has
     * the junction beside it, still sees road on both sides, and still answers
     * "middle lane". Skipping only junction blocks fixed one turn direction
     * and not the other - measured, RIGHT 84% on the line against LEFT 73% -
     * because which of the two a car lands on depends on which way it turned.
     *
     * So the walk continues until the block AND both of its cross neighbours
     * are clear of the crossing. That is the first block whose own answer is
     * about the street. */
    {
        int steps, dx, dy;
        heading_step(dir, &dx, &dy);
        for (steps = 0; steps < GTA_LANE_STREET_SCAN; steps++) {
            int lx, ly, hx, hy;
            if ((dir & 127) == 64) {
                lx = bx; ly = by - 1; hx = bx; hy = by + 1;
            } else {
                lx = bx - 1; ly = by; hx = bx + 1; hy = by;
            }
            if (!is_junction(m, bx, by, z) &&
                !is_junction(m, lx, ly, z) &&
                !is_junction(m, hx, hy, z))
                break;                  /* this block can answer for itself */
            if (!drivable(m, bx + dx, by + dy, z))
                break;                  /* the street ends here; keep this one */
            bx += dx;
            by += dy;
        }
    }

    if ((dir & 127) == 64) {            /* east-west: the kerbs are on y */
        lo = drivable(m, bx, by - 1, z);
        hi = drivable(m, bx, by + 1, z);
    } else {                            /* north-south: the kerbs are on x */
        lo = drivable(m, bx - 1, by, z);
        hi = drivable(m, bx + 1, by, z);
    }
    if (!lo && hi)      t += GTA_LANE_KERB;
    else if (lo && !hi) t -= GTA_LANE_KERB;
    return t;
}

/* WHICH LAYER A CAR ARRIVES ON when it leaves (bx,by,z) heading `dir` - or -1
 * if it cannot leave that way at all.
 *
 * ROADS CHANGE LAYER, and until this existed the traffic could not. The
 * city-wide sweep's worst site by a wide margin was (204,84), and it turned
 * out not to be a driving fault at all: layer 3 there carries a westbound and
 * an eastbound pair of lanes from x=194 to x=203 and nothing else,
 *
 *      dirmap ... 190 76 26 16 3
 *      81 ####<<<<<<<<<<############
 *      83 ####----------############
 *
 * because the flyover's RAMPS are on layer 2. Cars drove to the end of the
 * viaduct, stopped, and filled it up - 35% of the fleet moving, one car
 * standing for twelve seconds.
 *
 * The map has always said where the ramps are. At both ends of that bridge the
 * block one layer down is road and carries a SLOPE:
 *
 *      column 204 81 ->  z=2  slope=32  dirs=..W.      the down ramp
 *      column 193 81 ->  z=2  slope=40  dirs=..W.      and the other end
 *
 * so the rule is: if the next block is not drivable on this layer, try the one
 * below and the one above, and accept it when EITHER end of the step is
 * sloped - going down, the block being entered is the ramp; going up, the
 * block being left is. The navigation grid has carried the slope bit since it
 * was built (`gta_nav_sloped`); nothing walked it. */
static int nav_step_layer(const gta_map *m, int bx, int by, int z, int dir)
{
    int dx, dy, here_slope;

    heading_step(dir, &dx, &dy);
    if (drivable(m, bx + dx, by + dy, z))
        return z;

    here_slope = g_nav && g_nav->b
                 ? gta_nav_sloped(gta_nav_at_m(g_nav, bx, by, z)) : 0;

    if (z > 0 && drivable(m, bx + dx, by + dy, z - 1) &&
        (here_slope || (g_nav && g_nav->b &&
                        gta_nav_sloped(gta_nav_at_m(g_nav, bx + dx, by + dy,
                                                  z - 1)))))
        return z - 1;

    if (z + 1 < GTA_MAP_LAYERS && drivable(m, bx + dx, by + dy, z + 1) &&
        (here_slope || (g_nav && g_nav->b &&
                        gta_nav_sloped(gta_nav_at_m(g_nav, bx + dx, by + dy,
                                                  z + 1)))))
        return z + 1;

    return -1;
}

/* Where may a car standing on (bx,by) go next, given it arrived heading `cur`?
 *
 * The block's own direction bits, minus the way it came - a car does not turn
 * round in the road - and minus anything that leads off the tarmac. Straight on
 * is preferred where it is legal, because traffic that picks uniformly at
 * random turns at every junction and the city reads as a maze rather than as a
 * grid; three times in four is enough to look purposeful without ever leaving
 * a junction unused.
 *
 * Returns -1 when there is nowhere to go, which happens at the edge of the
 * direction data and is what retires the car. */
/* Do this block's own arrows allow travelling in `angle`? */
static int dir_allowed(const gta_map *m, int bx, int by, int z, int angle)
{
    gta_block b;
    int d;

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return 0;
    if (!gta_map_block(m, bx, by, z, &b)) return 0;
    d = gta_block_dirs(&b);
    switch (angle & 255) {
    case 0:   return (d & 0x01) != 0;    /* north */
    case 128: return (d & 0x02) != 0;    /* south */
    case 192: return (d & 0x04) != 0;    /* west  */
    case 64:  return (d & 0x08) != 0;    /* east  */
    }
    return 0;
}

/* Is there a way OUT of (bx,by) for a car that arrived heading `in`?
 *
 * A one-step lookahead, and it is what keeps traffic out of dead ends rather
 * than dealing with them afterwards. A car that drives into a block whose only
 * arrow points back the way it came has to stop there, and a stopped car in a
 * lane queues everything behind it - one bad block at the end of a street was
 * enough to lock a whole avenue in the drive test.
 *
 * Cheap: it runs once per block a car actually reaches, not per tick, and it
 * is four map lookups at the very worst. */
static int has_exit(const gta_map *m, int bx, int by, int z, int in, int length)
{
    gta_block b;
    int back = (in + 128) & 255;
    int i, axis;
    static const int dirs[4] = { 0, 64, 128, 192 };

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return 0;
    if (!gta_map_block(m, bx, by, z, &b)) return 0;

    for (i = 0; i < 4; i++) {
        int dx, dy, nz;
        if (dirs[i] == back) continue;
        if (!dir_allowed(m, bx, by, z, dirs[i])) continue;
        heading_step(dirs[i], &dx, &dy);
        nz = nav_step_layer(m, bx, by, z, dirs[i]);
        if (nz < 0) continue;   /* nz may be a layer up or down: a ramp */
        /* AND THE VEHICLE HAS TO FIT DOWN IT. A bus that turns into a street
         * it cannot lie in is stuck there, which is where two of the three
         * dead ends in the drive test came from - both were buses. */
        axis = ((dirs[i] & 127) == 64) ? 64 : 0;
        if (!fits(m, bx, by, z, axis, length)) continue;
        if (!fits(m, bx + dx, by + dy, nz, axis, length)) continue;
        return 1;
    }
    return 0;
}

/* HOW MUCH CITY CAN A CAR PARKED HERE ACTUALLY REACH?
 *
 * A bounded flood fill along the map's own arrows, through `nav_step_layer` so
 * ramps count, stopping as soon as GTA_REACH_MIN distinct blocks have been
 * seen. It answers one question: is this block part of the road network, or is
 * it part of a pocket that a car can drive into and never drive out of?
 *
 * THE MAP HAS SUCH POCKETS, and until this existed traffic was spawned into
 * them. The clearest is the flyover on layer 3 at rows 93-104:
 *
 *      gtadump dirmap dos/Grand_Theft_Auto/gtadata/nyc.cmp 206 88 16 22 3
 *       93 #######vv||#####
 *      ...  twelve rows of exactly this, then nothing
 *      104 #######vv||#####
 *
 * Four lanes with perfectly good arrows, so every other placement test is
 * happy - `fits` passes, `has_exit` passes, there is room. But the lanes never
 * interconnect (not one `+` in the whole structure) and both ramps are
 * PAVEMENT blocks with no direction bits, so `nav_step_layer` will not cross
 * them. A car put on column 213 can drive south and nothing else, for at most
 * twelve blocks, and then it is stopped for ever with the rest of the viaduct
 * queued behind it. That was 49761 car-ticks of "road ahead" at (204,108).
 *
 * `GTA_TRAFFIC_ABANDON` bounds the damage by deleting a car that has not
 * covered a block in thirty seconds - which is why the longest wait at that
 * site was exactly 30.0 s - but it still spawns cars into the trap and still
 * deletes them, sometimes in view. This is the actual fix: do not put one
 * there.
 *
 * Twelve blocks is the whole of the pocket, so a limit of twenty-four
 * separates it from a real street with a wide margin and without needing to
 * know anything about viaducts. Cost is bounded by construction and it runs
 * only for a car that has passed every other test, which is at most twenty
 * placements a second. */
static int can_get_away(const gta_map *m, int bx, int by, int z)
{
    unsigned char qx[GTA_REACH_MIN], qy[GTA_REACH_MIN];
    signed char qz[GTA_REACH_MIN];
    int head = 0, tail = 0, i, k;
    static const int dirs[4] = { 0, 64, 128, 192 };

    qx[tail] = (unsigned char)bx;
    qy[tail] = (unsigned char)by;
    qz[tail] = (signed char)z;
    tail++;

    while (head < tail) {
        int cx = qx[head], cy = qy[head], cz = qz[head];
        head++;

        for (i = 0; i < 4; i++) {
            int dx, dy, nz, nx, ny;

            if (!dir_allowed(m, cx, cy, cz, dirs[i]))
                continue;
            nz = nav_step_layer(m, cx, cy, cz, dirs[i]);
            if (nz < 0)
                continue;               /* a ramp still counts; see the note */
            heading_step(dirs[i], &dx, &dy);
            nx = cx + dx;
            ny = cy + dy;

            /* Linear, because the set is at most GTA_REACH_MIN long and a
             * visited bitmap would need a window big enough for wherever the
             * fill wanders. */
            for (k = 0; k < tail; k++)
                if (qx[k] == nx && qy[k] == ny && qz[k] == nz)
                    break;
            if (k < tail)
                continue;
            if (tail >= GTA_REACH_MIN)
                return 1;               /* seen enough: this is a real road */
            qx[tail] = (unsigned char)nx;
            qy[tail] = (unsigned char)ny;
            qz[tail] = (signed char)nz;
            tail++;
        }
    }
    return 0;                           /* the fill ran out: a pocket */
}

static int choose_heading(const gta_map *m, int bx, int by, int z,
                          int cur, int length, gta_traffic *tr)
{
    gta_block b;
    int cand[4], n = 0, i;
    int poor[4], np = 0;          /* legal, but with no way out of the far side */
    int back = (cur + 128) & 255;
    int all[4];
    int na = 0;

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM)
        return -1;
    if (!gta_map_block(m, bx, by, z, &b))
        return -1;

    if (gta_block_dir_north(&b)) all[na++] = 0;
    if (gta_block_dir_east(&b))  all[na++] = 64;
    if (gta_block_dir_south(&b)) all[na++] = 128;
    if (gta_block_dir_west(&b))  all[na++] = 192;

    for (i = 0; i < na; i++) {
        int dx, dy, axis, nz;
        if (all[i] == back) continue;
        heading_step(all[i], &dx, &dy);
        /* A ramp counts as a way out - see nav_step_layer(). Without it a
         * flyover is a cul-de-sac and its traffic never leaves. */
        nz = nav_step_layer(m, bx, by, z, all[i]);
        if (nz < 0) continue;

        /* AND THE VEHICLE HAS TO FIT FACING THAT WAY.
         *
         * A bus is 120 world pixels long - nearly four blocks - and its body
         * swings onto the OTHER axis the moment it turns. Checking the fit
         * only where a vehicle was first placed let buses turn into side
         * streets they could not possibly occupy, and the driving test caught
         * it in every seed: every single failure was a bus, none was a car.
         *
         * Both cells are tested because the turn happens at the CURRENT one -
         * the angle snaps while the car still sits at this block's centre -
         * and then it travels into the next. A vehicle that fits in only one
         * of the two would clip a building for the ticks in between, which is
         * exactly the kind of fault no single frame catches. */
        axis = ((all[i] & 127) == 64) ? 64 : 0;
        if (!fits(m, bx, by, z, axis, length)) continue;
        if (!fits(m, bx + dx, by + dy, nz, axis, length)) continue;

        /* AND IT MUST NOT BE A CUL-DE-SAC. A car that enters a block it can
         * never leave stops there for good, and everything behind it queues
         * on a lane that will never move again - one such block at the end of
         * a street locked a whole avenue in the drive test. Blocks with an
         * exit are collected first and the rest only used if there is no
         * choice at all. */
        if (has_exit(m, bx + dx, by + dy, nz, all[i], length)) cand[n++] = all[i];
        else                                          poor[np++] = all[i];
    }
    if (n == 0 && np > 0) {
        /* Nothing but dead ends. Take one - it is still better than stopping
         * here, and the car will be retired once it is off screen. */
        n = np;
        for (i = 0; i < np; i++) cand[i] = poor[i];
    }
    if (n == 0) {
        /* NOWHERE TO GO IS NOT A REASON TO DELETE A CAR.
         *
         * This used to return -1 and the caller retired the car on the spot -
         * in full view, wherever it happened to be standing, which is half of
         * what "cars disappear here and there" was. The direction data runs
         * out at map edges and at the odd block whose arrows point only the
         * way the car came, and a driver who reaches one turns round.
         *
         * So a U-turn is offered as a last resort, and only as a last resort:
         * it is excluded from the normal candidates above, because traffic
         * that may reverse at any block turns the grid into a shuffle. */
        int dx, dy, axis;
        heading_step(back, &dx, &dy);
        axis = ((back & 127) == 64) ? 64 : 0;
        /* AND ONLY ONTO A LANE THAT ALLOWS IT.
         *
         * Without this test the U-turn put cars onto the opposing one-way
         * lane, driving the wrong way up it - and since gap_ahead() treats
         * anything on its own axis as the car in front, the wrong-way car and
         * the first car it met each stopped for the other and neither ever
         * moved again. That is how one turned-round car locked an avenue: the
         * drive test showed a northbound car sitting in a block whose arrows
         * read S, with five southbound cars queued behind it.
         *
         * On a two-way road both bits are set and the turn is legal, which is
         * exactly where a driver would make one. */
        if (dir_allowed(m, bx, by, z, back) &&
            drivable(m, bx + dx, by + dy, z) &&
            dir_allowed(m, bx + dx, by + dy, z, back) &&
            fits(m, bx, by, z, axis, length) &&
            fits(m, bx + dx, by + dy, z, axis, length))
            return back;
        return -1;
    }

    for (i = 0; i < n; i++)
        if (cand[i] == cur && (next_rand(tr) & 3) != 0)
            return cur;
    return cand[next_rand(tr) % (unsigned long)n];
}

static int is_junction(const gta_map *m, int bx, int by, int z);
static void box_build(const gta_nav *nav);

/* --- traffic lights -------------------------------------------------------
 *
 * Where they are comes from the map (see the note in gta_traffic.h); when they
 * change is ours. The phase is a function of the tick and of the junction's
 * own coordinates, so nothing is stored per junction and neighbouring
 * junctions are half a cycle apart instead of the whole city blinking at once.
 *
 * >> 3 groups eight blocks together, which is about one city block of the map,
 * so the four stop lines of ONE junction always agree with each other - they
 * are what has to be consistent, and they are within a few blocks. */
int gta_traffic_light_green(const gta_traffic *tr, int bx, int by, int along_x)
{
    unsigned long cycle = (unsigned long)GTA_LIGHT_PHASE * 2UL;
    unsigned long off   = (((unsigned long)((bx >> 3) + (by >> 3))) & 1UL)
                          * (unsigned long)GTA_LIGHT_PHASE;
    unsigned long p     = (tr->tick + off) % cycle;
    int ew = (p < (unsigned long)GTA_LIGHT_PHASE);   /* east-west has it */

    /* The amber at the end of a phase is all-red: nobody new enters the box
     * while whatever is inside it clears. */
    if ((p % (unsigned long)GTA_LIGHT_PHASE) >=
        (unsigned long)(GTA_LIGHT_PHASE - GTA_LIGHT_AMBER))
        return 0;
    return along_x ? ew : !ew;
}

/* Is this block a stop line for a car heading `angle`?
 *
 * A hinted block on its own is not enough: the hint runs all the way round a
 * junction, so the block a car LEAVES by carries it as surely as the one it
 * arrives at. What makes a stop line is the hint plus a junction box directly
 * ahead - the near side of the crossing. */
static int at_stop_line(const gta_map *m, int hint, int bx, int by, int z,
                        int angle)
{
    int dx, dy;

    if (hint != 1) return 0;
    heading_step(angle, &dx, &dy);
    return is_junction(m, bx + dx, by + dy, z);
}

/* The block's direction bits: N and S are bits 0..1, W and E bits 2..3.
 * -1 when off the map or not a block. */
static int dirs_at(const gta_map *m, int bx, int by, int z)
{
    gta_block b;

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return -1;
    if (g_nav && g_nav->b)
        return gta_nav_dirs(gta_nav_at_m(g_nav, bx, by, z));
    if (!gta_map_block(m, bx, by, z, &b)) return -1;
    return gta_block_dirs(&b);
}

/* --- the box bitmap ------------------------------------------------------
 *
 * One bit per block per layer: "this block is part of a junction box".
 * Filled once, from the NAV grid, when gta_traffic_set_nav() runs - 48 KB.
 *
 * Every per-query heuristic tried here broke on some real crossing,
 * because the map's direction bits under-report: a blind square carries
 * only the axis of ONE of the two flows that use it, and which one is the
 * designers' whim. The developer's photograph of (120..123, 249..252) is
 * the definitive case: a four-lane road ends on another four-lane road,
 * and the western stem column's squares carry only the bar's E-W bits,
 * with not one seed anywhere in that column - no scan along either axis
 * of those squares can prove they are crossed. The box is only knowable
 * from the CLUSTER: seeds mark the turn squares, the stem is the run of
 * columns carrying N-S bits around them, the bar the run of rows carrying
 * E-W bits, and the box is the product rectangle. So that is computed,
 * per seed, at set-nav time. */
static unsigned char g_box[((long)GTA_MAP_DIM * GTA_MAP_DIM *
                            GTA_MAP_LAYERS) / 8];
static int g_box_built = 0;

static int box_bit(int x, int y, int z)
{
    long n = ((long)z * GTA_MAP_DIM + y) * GTA_MAP_DIM + x;
    return (g_box[n >> 3] >> (int)(n & 7)) & 1;
}

static void box_bit_set(int x, int y, int z)
{
    long n = ((long)z * GTA_MAP_DIM + y) * GTA_MAP_DIM + x;
    g_box[n >> 3] |= (unsigned char)(1 << (int)(n & 7));
}

/* nav byte helpers for the builder */
static int nav_road(const gta_nav *nav, int x, int y, int z)
{
    if (x < 0 || x >= GTA_MAP_DIM || y < 0 || y >= GTA_MAP_DIM) return 0;
    return gta_nav_ground(gta_nav_at_m(nav, x, y, z)) == GROUND_ROAD;
}

static int nav_dirs2(const gta_nav *nav, int x, int y, int z)
{
    if (x < 0 || x >= GTA_MAP_DIM || y < 0 || y >= GTA_MAP_DIM) return 0;
    return gta_nav_dirs(gta_nav_at_m(nav, x, y, z));
}

static int nav_seed(const gta_nav *nav, int x, int y, int z)
{
    int d = nav_dirs2(nav, x, y, z);
    return nav_road(nav, x, y, z) &&
           ((d & 0x03) != 0) && ((d & 0x0C) != 0);
}

/* Grow one seed's rectangle to the crossing it belongs to.
 *
 * A COLUMN joins when it carries a north/south bit on road within one row
 * of the current rows (the stem's own lane runs into the bar, so its N-S
 * bit sits just outside the bar rows - (120,248) `v` above the bar was the
 * proof) and has road inside the current rows. A ROW joins symmetrically
 * on east/west bits. The window of one, no more: at (71,68) the innocent
 * bend two blocks from a crossroads has no such bit anywhere in the
 * cluster's window, which is what keeps that peninsula out. Capped at 8x8:
 * bigger is map damage, not a crossing. */
static void box_grow(const gta_nav *nav, int z, int sx, int sy)
{
    int x0 = sx, x1 = sx, y0 = sy, y1 = sy;
    int changed = 1, guard = 0, x, y;

    while (changed && ++guard < 16) {
        changed = 0;
        if (x1 - x0 < 7) {
            for (x = x0 - 1; x <= x1 + 1; x += (x1 - x0 + 2)) {
                int hasns = 0, hasroad = 0;
                for (y = y0 - 1; y <= y1 + 1; y++)
                    if (nav_road(nav, x, y, z) &&
                        (nav_dirs2(nav, x, y, z) & 0x03) != 0)
                        hasns = 1;
                for (y = y0; y <= y1; y++)
                    if (nav_road(nav, x, y, z))
                        hasroad = 1;
                if (hasns && hasroad) {
                    if (x < x0) x0 = x; else x1 = x;
                    changed = 1;
                }
            }
        }
        if (y1 - y0 < 7) {
            for (y = y0 - 1; y <= y1 + 1; y += (y1 - y0 + 2)) {
                int hasew = 0, hasroad = 0;
                for (x = x0 - 1; x <= x1 + 1; x++)
                    if (nav_road(nav, x, y, z) &&
                        (nav_dirs2(nav, x, y, z) & 0x0C) != 0)
                        hasew = 1;
                for (x = x0; x <= x1; x++)
                    if (nav_road(nav, x, y, z))
                        hasroad = 1;
                if (hasew && hasroad) {
                    if (y < y0) y0 = y; else y1 = y;
                    changed = 1;
                }
            }
        }
    }
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            if (nav_road(nav, x, y, z))
                box_bit_set(x, y, z);
}

static void box_build(const gta_nav *nav)
{
    int x, y, z;
    long i;

    for (i = 0; i < (long)sizeof(g_box); i++)
        g_box[i] = 0;
    g_box_built = 0;
    if (!nav || !nav->b)
        return;
    for (z = 0; z < GTA_MAP_LAYERS; z++)
        for (y = 0; y < GTA_MAP_DIM; y++)
            for (x = 0; x < GTA_MAP_DIM; x++)
                if (nav_seed(nav, x, y, z))
                    box_grow(nav, z, x, y);
    g_box_built = 1;
}

/* IS THIS BLOCK PART OF A JUNCTION - the crossing is the RECTANGLE where the
 * two roads overlap, not only the blocks whose arrows carry both axes.
 *
 * The map paints a second axis on a block only where a second flow actually
 * drives over it, so at every T in the city the through lanes of the top bar
 * keep a single direction bit - "blind" squares in the middle of the
 * crossing. Measured on nyc.cmp: every full-`+` 4x4 box is a four-arm cross
 * (five in the whole city); every 3-arm box has blind squares, up to 15 of
 * 16. With the arrow test alone, bookings truncated at the first blind
 * square (a straight car "reserved its lane" two squares deep), the gate
 * could not see the box from a blind through lane at all, and two granted
 * shapes met on squares neither could book - the developer's two reports of
 * 2026-08-25: the T-junction barge-in, and the small-street car driven onto
 * the pavement by a turn whose bend block the walk never reached.
 *
 * With the nav grid in place the answer is a precomputed bit (box_build
 * above); before that - host tools that never set a nav - the seed test
 * stands alone, which is the old arrow-based answer. */
static int is_junction(const gta_map *m, int bx, int by, int z)
{
    int d;

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM)
        return 0;
    if (g_box_built && z >= 0 && z < GTA_MAP_LAYERS)
        return box_bit(bx, by, z);
    d = dirs_at(m, bx, by, z);
    if (d < 0)
        return 0;
    return ((d & 0x03) != 0) && ((d & 0x0C) != 0);
}

/* FOLLOWING THE CAR IN FRONT.
 *
 * The gap, in 16.16 world pixels, to the nearest vehicle ahead in this car's
 * own lane, or -1 if the road is clear. `lead_speed` comes back with that
 * car's speed so this one can match it rather than close on it and stop.
 *
 * "In its own lane" is a lateral test, not a block test: a car is compared
 * only against vehicles within half a block sideways, so oncoming traffic in
 * the next lane and cross traffic on the other axis are ignored. A block test
 * would have made a car brake for anything sharing its square, which at a
 * junction is every direction at once.
 *
 * The gap is bumper to bumper - centre distance less both half-lengths - so a
 * bus is given the room a bus needs without the caller knowing anything about
 * lengths. */
/* HOW FAR CAN THIS CAR DRIVE FORWARD BEFORE ITS BODY TOUCHES THAT ONE?
 *
 * "powinien byc jakby prostokat detekcji przed autem [...] one tylko sprawdzaja
 * czy tam nie ma auta zamiast sprawdzic auto + prostokat z jego przodu"
 *
 * Exactly that, and computed rather than sampled. Slide this car's oriented
 * box forward along its own heading and ask at what distance it first touches
 * the other car's oriented box. That distance IS the rectangle in front of the
 * car - it is as long as the car needs it to be, it points where the car is
 * actually pointing, and it does not care which lane or which axis anybody is
 * on.
 *
 * WHAT IT REPLACES, AND WHY THAT HAD TO GO. gap_ahead() projected the offset
 * onto the compass and then REFUSED TO LOOK at any car travelling on the other
 * axis - the comment beside it says so, and the reason was real: without the
 * junction rules, a crossing car read as the leader and every queue stopped.
 * But it means a car turning across your bonnet is invisible to you, and a car
 * you are turning across is invisible to it. That is the "auta na siebie
 * najezdzaja" report and no amount of work on the lane projection can reach
 * it, because the fault is that the projection is not a shape.
 *
 * THE MATHS. Separating-axis, but solved for the distance rather than tested
 * at one position. On each of the four box normals the two projections overlap
 * for an interval of forward distances; the boxes overlap when all four
 * intervals do, so the answer is the largest of the four lower bounds. An axis
 * the car does not move along at all (h.n == 0) either always separates - no
 * contact ever - or never does, and drops out.
 *
 * All Q14 against whole-pixel extents, so the biggest product is about 5e6 and
 * this is safe where a long is four bytes.
 *
 * `hx,hy` is the RELATIVE velocity in Q14 world pixels a tick, so the answer
 * comes back in TICKS: 0 if the boxes are already touching, -1 if they do not
 * touch within `maxt`. Sweeping along a velocity rather than a heading is what
 * lets a car ignore the one that is driving out of its way. */
static long sweep_toi(long ax, long ay, int aang, int ahl, int ahw,
                      long bx_, long by_, int bang, int bhl, int bhw,
                      long hx, long hy, long maxt)
{
    long dx = (bx_ - ax) >> FP, dy = (by_ - ay) >> FP;
    long axis[4][2];
    long tlo = 0, thi = maxt;
    int k;

    axis[0][0] = gta_sin(aang);     axis[0][1] = -gta_cos(aang);
    axis[1][0] = gta_cos(aang);     axis[1][1] = gta_sin(aang);
    axis[2][0] = gta_sin(bang);     axis[2][1] = -gta_cos(bang);
    axis[3][0] = gta_cos(bang);     axis[3][1] = gta_sin(bang);

    for (k = 0; k < 4; k++) {
        long nx = axis[k][0], ny = axis[k][1];
        long r  = dx * nx + dy * ny;               /* Q14 pixels */
        long hn = (hx * nx + hy * ny) >> 14;       /* Q14, dimensionless */
        long pa, pb, s, t0, t1, t;

        t = (axis[0][0] * nx + axis[0][1] * ny) >> 14;
        pa = (t < 0 ? -t : t) * ahl;
        t = (axis[1][0] * nx + axis[1][1] * ny) >> 14;
        pa += (t < 0 ? -t : t) * ahw;
        t = (axis[2][0] * nx + axis[2][1] * ny) >> 14;
        pb = (t < 0 ? -t : t) * bhl;
        t = (axis[3][0] * nx + axis[3][1] * ny) >> 14;
        pb += (t < 0 ? -t : t) * bhw;
        s = pa + pb;

        if (hn == 0) {
            if (r > s || r < -s)
                return -1;                         /* this axis always parts them */
            continue;                              /* and never separates them */
        }
        t0 = (r - s) / hn;
        t1 = (r + s) / hn;
        if (t0 > t1) { t = t0; t0 = t1; t1 = t; }
        if (t0 > tlo) tlo = t0;
        if (t1 < thi) thi = t1;
        if (tlo > thi)
            return -1;
    }
    return tlo;
}

/* THE RECTANGLE IN FRONT OF THIS CAR, AGAINST EVERY OTHER CAR IN THE CITY.
 *
 * "one tylko sprawdzaja czy tam nie ma auta zamiast sprawdzic auto + prostokat
 * z jego przodu [...] jedno rusza kiedy drugie juz jedzie"
 *
 * Both halves of that, and the second half is the one that makes it work. The
 * box is swept along the RELATIVE velocity - this car's minus the other one's -
 * so the answer is a time to contact rather than a distance, and a vehicle
 * that is getting out of the way is not an obstacle. Sweeping along this car's
 * own heading instead, which is the obvious version, brakes for everything
 * crossing a junction anywhere ahead: 96% of the fleet moving down to 30%.
 *
 * A car that is STOPPED still has to answer "if I pull away now, do I hit
 * anybody" - and with a velocity of zero the relative motion is the other
 * car's alone and the sweep says nothing useful. So a stationary car is
 * measured as if it were already rolling at the speed it would pull away at.
 * That is precisely "one rusza kiedy drugie juz jedzie".
 *
 * IT RETURNS TICKS AND THE CALLER TREATS IT AS TICKS. Converting the answer
 * into a following distance was tried and it is nonsense: contact six ticks
 * away came out as a gap of eleven pixels, which is inside GTA_TRAFFIC_MIN_GAP,
 * so every detection became a dead stop and the fleet fell to a third of
 * itself at every horizon tried. Time to contact is not a distance and must
 * not be fed to a rule that wants one. */
static long gap_ahead_box(const gta_traffic *tr, int idx, long *lead_speed)
{
    const gta_car *c = &tr->cars[idx];
    const gta_car_info *ci = &tr->tiles->cars[c->model];
    int ahl = gta_car_world_len(ci) / 2;
    int ahw = gta_car_world_wid(ci) / 2;
    long cull = (GTA_TRAFFIC_LOOKAHEAD >> FP) + 64;
    long vme  = c->speed;
    long vx, vy, best = -1;
    int i;

    *lead_speed = 0;
    if (vme < (long)GTA_SPEED_TURN_TIGHT * GTA_SPEED_UNIT)
        vme = (long)GTA_SPEED_TURN_TIGHT * GTA_SPEED_UNIT;

    /* This car's velocity, Q14 world pixels a tick. */
    /* v_q14 = sin * speed / 65536, split as two shifts of eight so the
     * product cannot overflow a 32-bit long on a fast car. */
    vx =  ((long)gta_sin(c->face) * (vme >> 8)) >> 8;
    vy = -((long)gta_cos(c->face) * (vme >> 8)) >> 8;

    for (i = 0; i < tr->n; i++) {
        const gta_car *o = &tr->cars[i];
        const gta_car_info *oi;
        long ddx, ddy, ovx, ovy, t;

        if (i == idx || o->done || o->layer != c->layer)
            continue;

        /* ONLY ONE OF ANY TWO CARS GIVES WAY, AND THIS DECIDES WHICH.
         *
         * Without it the rectangle is symmetric and so is the answer: two cars
         * approaching a crossing each see the other inside their horizon, each
         * brakes to a stop, and neither ever starts again. Traced tick by tick
         * at (64,64) - car 4 heading east and car 1 heading north, both
         * reporting contact in one or two ticks, both decaying to zero speed,
         * both still there a thousand ticks later. 43% of all car-ticks in the
         * run were a car held by this rule, and the fleet ran at 15%.
         *
         * The moving car has the right of way over the stopped one - it is
         * already committed and stopping it is what causes the knot - and two
         * cars at the same speed are separated by serial, which is arbitrary
         * but STABLE, so the pair cannot oscillate. Following distance is not
         * this rule's job: gap_ahead() keeps a car off the one in front of it
         * in its own lane, whoever has the lower serial. */
        if (!(o->speed > c->speed ||
              (o->speed == c->speed && o->serial < c->serial)))
            continue;

        ddx = (o->x - c->x) >> FP; if (ddx < 0) ddx = -ddx;
        ddy = (o->y - c->y) >> FP; if (ddy < 0) ddy = -ddy;
        if (ddx > cull || ddy > cull)
            continue;

        ovx =  ((long)gta_sin(o->face) * (o->speed >> 8)) >> 8;
        ovy = -((long)gta_cos(o->face) * (o->speed >> 8)) >> 8;

        oi = &tr->tiles->cars[o->model];
        t = sweep_toi(c->x, c->y, c->face, ahl, ahw,
                      o->x, o->y, o->face,
                      gta_car_world_len(oi) / 2, gta_car_world_wid(oi) / 2,
                      vx - ovx, vy - ovy, tr->opt_horizon);
        if (t < 0)
            continue;
        if (best < 0 || t < best) {
            best = t;
            *lead_speed = o->speed;
        }
    }
    return best;        /* TICKS to contact, or -1 */
}

static long gap_ahead(const gta_traffic *tr, int idx, long *lead_speed,
                      int *lead_i)
{
    const gta_car *c = &tr->cars[idx];
    long best = -1;

    *lead_i = -1;
    int i;
    long fx = gta_sin(c->face), fy = -gta_cos(c->face);

    *lead_speed = 0;

    for (i = 0; i < tr->n; i++) {
        const gta_car *o = &tr->cars[i];
        long along, side, gap;

        long ox, oy;

        if (i == idx || o->done || o->layer != c->layer)
            continue;

        /* WHAT IS IN FRONT OF THIS CAR, MEASURED ALONG ITS OWN HEADING - not
         * along the compass direction it is nearest to, and not only against
         * cars that happen to face the same way.
         *
         * Both of those restrictions used to be here and both had to go when
         * the steering went in. A car half way round a corner is on no axis at
         * all, so an axis test makes it invisible to the car behind it AND
         * makes everything invisible to it: six cars piled into one block in
         * the drive test, thirteen overlaps in nine hundred ticks. What
         * matters is whether something is physically ahead, which is what the
         * projection below asks. The deadlock this rule once caused - a
         * crossing car treated as the leader of a queue - is now handled by
         * the junction reservation and its patience, where it belongs. */
        ox = o->x - c->x;
        oy = o->y - c->y;
        if (ox > GTA_TRAFFIC_LOOKAHEAD || ox < -GTA_TRAFFIC_LOOKAHEAD ||
            oy > GTA_TRAFFIC_LOOKAHEAD || oy < -GTA_TRAFFIC_LOOKAHEAD)
            continue;               /* and this keeps the multiply in range */

        along = ((ox >> 8) * fx + (oy >> 8) * fy) >> 6;
        side  = ((ox >> 8) * -fy + (oy >> 8) * fx) >> 6;
        if (side < 0) side = -side;

        /* TWELVE PIXELS SIDEWAYS, AND WIDENING IT DOES NOT WORK - measured
         * twice, because it looks like the obvious fix for the one pile-up
         * this test still finds and it is not.
         *
         * Twelve is narrower than a lane, so two cars CONVERGING on one do not
         * see each other until they are nearly touching. At (36,156) that ends
         * with eight vehicles at px(1053..1055, 4752) - eight cars within two
         * pixels of one another, every pair overlapping, all "held by gap" and
         * none of them able to separate again, because a car here has no
         * reverse gear.
         *
         *                          overlaps    (64,64)   (61,52)
         *      12 px (this)           55         94%       68%
         *      16 px always           15         21%       13%
         *      16 px while turning    28         72%       60%
         *
         * The reference sites are over 12000 ticks, the overlaps over 3000 at
         * (36,156). A wider window makes every car brake for traffic in the
         * NEXT lane wherever two lanes run side by side, which is most of
         * downtown, and that costs far more than the pile-up it prevents.
         *
         * What is actually wrong is that nothing refuses the MERGE: two cars
         * arriving at one lane out of two others are each legally placed right
         * up to the moment they occupy the same pixel. That wants a check when
         * the turn is ISSUED - room_for() along the arc the car is about to
         * describe, rather than at the next block's centre - not a wider
         * follow window. */
        /* ...AND WIDENING IT FOR A TURNING CAR WAS RE-MEASURED IN 2026-08-23
         * AND IS STILL WRONG. The table's third row was taken before the lane
         * target became a latched per-car field, so it was worth repeating now
         * that every car sits somewhere slightly different; it came out the
         * same way. 16 px while turning, 96 sites, 3000 ticks: overlaps
         * SUMMED 96 -> 111, flow 88% -> 87%, longest stood 27.2 s -> 30.0 s.
         * Do not try it a third time. */
        /* THE CORRIDOR IS A FIXED WIDTH AND IT HAS TO BE.
         *
         * Widening it by the two bodies whenever either car is turning - so
         * that a vehicle lying diagonally across a junction is visible to the
         * one behind it - is correct geometry and it was measured: 96% of the
         * fleet moving down to 32%, 82% to 44%, and the overlap count did not
         * move. Every rule that makes a car brake for CROSS traffic gridlocks
         * this city, because with twenty cars and no traffic lights the
         * junctions are permanently contested. Five versions of that idea are
         * recorded in the notes (31); the arbitration a junction needs is
         * right of way, not a wider bumper. */
        if (along <= 0 || side > GTA_FOLLOW_SIDE)
            continue;

        /* THE MUTUAL BLOCK, BROKEN BY PRIORITY.
         *
         * box_busy() stops a car ENTERING a crossing that already holds cross
         * traffic, but it cannot stop two cars entering the same crossing on
         * the same tick from two different streets: neither centre is inside
         * yet, so neither sees the other. Once both are in, each is "the car
         * in front" of the other and neither ever moves again - the frozen
         * pair at (60,63), one heading west and one heading south, that made
         * the whole southbound column back up eight cars deep.
         *
         * Nothing can separate them afterwards; a car here has no reverse
         * gear. So one of them is given priority and drives out, which is what
         * a pair of drivers would sort out between them. The tie-break is the
         * fleet index, which is arbitrary but STABLE - both cars agree on it,
         * so exactly one of the two moves and the knot unpicks. It applies
         * only when both are stationary, in the same block, and on crossing
         * axes; ordinary following is untouched. */
        if (o->speed == 0 && c->speed == 0 && i > idx &&
            (((o->angle & 127) == 64) != ((c->angle & 127) == 64)) &&
            (int)(o->x >> (FP + 5)) == (int)(c->x >> (FP + 5)) &&
            (int)(o->y >> (FP + 5)) == (int)(c->y >> (FP + 5)))
            continue;
        if (along > GTA_TRAFFIC_LOOKAHEAD)
            continue;

        gap = along - (((long)gta_car_world_len(&tr->tiles->cars[c->model]) / 2 +
                        (long)gta_car_world_len(&tr->tiles->cars[o->model]) / 2) << FP);
        if (gap < 0) gap = 0;
        if (best < 0 || gap < best) {
            best = gap;
            *lead_speed = o->speed;
            *lead_i = i;
        }

    }

    /* AND THE PLAYER'S CAR, exactly the same projection. lead_i stays -1 -
     * nothing downstream may index the fleet with the player - and the lead
     * speed is the player's own, so a queue behind them rolls when they
     * roll. */
    if (tr->pl_active && tr->pl_layer == c->layer) {
        long ox = tr->pl_x - c->x, oy = tr->pl_y - c->y;
        if (ox <= GTA_TRAFFIC_LOOKAHEAD && ox >= -GTA_TRAFFIC_LOOKAHEAD &&
            oy <= GTA_TRAFFIC_LOOKAHEAD && oy >= -GTA_TRAFFIC_LOOKAHEAD) {
            long along = ((ox >> 8) * fx + (oy >> 8) * fy) >> 6;
            long side  = ((ox >> 8) * -fy + (oy >> 8) * fx) >> 6;
            if (side < 0) side = -side;
            if (along > 0 && side <= GTA_FOLLOW_SIDE &&
                along <= GTA_TRAFFIC_LOOKAHEAD) {
                long pgap = along -
                    (((long)gta_car_world_len(&tr->tiles->cars[c->model]) / 2
                      + (long)tr->pl_hl) << FP);
                if (pgap < 0) pgap = 0;
                if (best < 0 || pgap < best) {
                    best = pgap;
                    *lead_speed = tr->pl_speed;
                    *lead_i = -1;
                }
            }
        }
    }
    return best;
}

/* IS SOMEBODY ALREADY IN THIS JUNCTION, CROSSING?
 *
 * The deadlock this exists to prevent is the one the developer photographed -
 * an ambulance and a tanker nose to nose in the middle of a crossing - and the
 * drive test reproduces it exactly: at seed 11, car 0 heading north and car 2
 * heading west were both standing in block (63,58), each of them reported as
 * "held by gap", for the last seven hundred ticks of the run. gap_ahead() sees
 * whatever is physically in front of the car, cross traffic included, so two
 * cars that get into the same square from two different streets each treat the
 * other as the car in front and neither ever moves again.
 *
 * Nothing can untie that afterwards. A car has no reverse gear here, so the
 * rule has to be about ENTERING: only one stream may be in the box.
 *
 * THIS IS NARROWER THAN THE RESERVATION IT REPLACES, and the difference is the
 * whole reason that one was deleted on 2026-08-22. That claimed the block a
 * car was driving into from a block and a half away, and held it for the
 * approach as well as the crossing - 67 to 80 per cent of the fleet moving,
 * cars standing for ten seconds. This asks only whether a vehicle's CENTRE is
 * inside the one block being entered, only about traffic on the other axis,
 * and only while the car is still outside it. A car already in a junction is
 * never held by it and can always leave. */
static int box_busy(const gta_traffic *tr, const gta_map *m, int idx,
                    int bx, int by, int layer, int ew)
{
    int rx = bx, ry = by, i;

    /* THE WHOLE CROSSING, NOT THE ONE BLOCK.
     *
     * "jedzie skrzyzowaniem motor - po prostu prosto, jest na 2 z 4 klockow a
     * tam z jego prawej wyjezdza auto, mimo ze on powinien generowac zajetosc
     * wszystkiego przed nim"
     *
     * This asked whether a car on the other axis had its CENTRE in the single
     * block being entered. A crossing in Liberty City is two to four blocks
     * across, so a vehicle already half way through it - the motorbike in the
     * report, sitting on block two of four - was invisible to everybody
     * entering by another arm. The question a driver actually asks is "is
     * anybody crossing this junction", and the junction is the root. */
    /* AND IT IS OFF, because making it true costs the city. Measured over
     * five seeds: (64,64) 96% of the fleet moving -> 62%, (61,48) 79% -> 21%,
     * (50,44) 82% -> 24%. A crossing four blocks across, held against all
     * cross traffic until it is completely empty, serialises every junction in
     * Liberty City - the same failure as every other rule that brakes for
     * traffic on the other axis (the notes 31). The fault the developer
     * describes is real; the cure has to be finer than a whole crossing.
     * `GTA_OPT_BOXROOT=1` turns it on. */
    if (tr->opt_boxroot && is_junction(m, bx, by, layer))
        junction_root(m, bx, by, layer, &rx, &ry);

    for (i = 0; i < tr->n; i++) {
        const gta_car *o = &tr->cars[i];
        int ox, oy, orx, ory;

        if (i == idx || o->done || o->layer != layer)
            continue;
        ox = (int)(o->x >> (FP + 5));
        oy = (int)(o->y >> (FP + 5));
        /* THE WHOLE BODY, not the middle - see car_on_block(). A long vehicle
         * stopped across a crossing was invisible to every arm but its own. */
        if (tr->opt_body ? car_on_block(tr, o, bx, by)
                         : (ox == bx && oy == by)) {
            /* The axis test is kept exactly as it was: a car on the same
             * axis as us is the queue, and blocking on it turns every junction
             * into a second brake. Adding "or it is stopped" here was measured
             * and it costs a quarter of the city (90% -> 64% at (64,64)) while
             * the overlap count does not move. */
            if ((((o->angle & 127) == 64) ? 1 : 0) != ew)
                return 1;
            continue;
        }
        if (!tr->opt_boxroot ||
            !is_junction(m, bx, by, layer) || !is_junction(m, ox, oy, layer))
            continue;
        junction_root(m, ox, oy, layer, &orx, &ory);
        if (orx != rx || ory != ry)
            continue;
        if ((((o->angle & 127) == 64) ? 1 : 0) != ew)
            return 1;
    }
    return 0;
}

/* WHICH CROSSING IS THIS, as one pair of coordinates.
 *
 * A junction here is a PATCH of junction blocks - the roads are two lanes wide,
 * so the usual one is 2x2 and some are 5 blocks across - and the four arms all
 * enter it by different blocks. A claim on "the block I am entering" would let
 * all four cars claim the same crossing at once, which is the bug it is meant
 * to fix. So the patch is named by its top-left block: walk west while there is
 * still junction, then north the same way. Every arm of one crossing reaches
 * the same answer, and it costs a handful of nav reads. */
static void junction_root(const gta_map *m, int bx, int by, int z,
                          int *rx, int *ry)
{
    int n;

    for (n = 0; n < GTA_BOX_SCAN && is_junction(m, bx - 1, by, z); n++)
        bx--;
    for (n = 0; n < GTA_BOX_SCAN && is_junction(m, bx, by - 1, z); n++)
        by--;
    *rx = bx;
    *ry = by;
}

/* MAY THIS CAR FORCE THIS CROSSING? Takes the claim if it is free or already
 * ours, refuses if somebody else holds a live one. See the claim table in
 * gta_traffic.h for why this exists at all.
 *
 * IT FAILS OPEN. If the table is full - which needs eight crossings held at
 * once, more than a screen of traffic can manage - this says yes. A claim that
 * cannot be recorded must not become a claim that blocks: the worst case of
 * saying yes is the deadlock this is trying to prevent, and the worst case of
 * saying no is a car that never moves again. */
static int junction_claim(gta_traffic *tr, int rx, int ry, int z,
                          unsigned long serial)
{
    int i, free_slot = -1;

    for (i = 0; i < tr->claim_top; i++) {
        if (tr->claim_ttl[i] <= 0) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (tr->claim_x[i] == (unsigned char)rx &&
            tr->claim_y[i] == (unsigned char)ry &&
            tr->claim_z[i] == (signed char)z) {
            if (tr->claim_car[i] != serial)
                return 0;                    /* somebody else is in there */
            tr->claim_ttl[i] = GTA_CLAIM_TTL;   /* ours - renew and go */
            return 1;
        }
    }
    if (free_slot < 0) {
        /* Nothing free below claim_top - every slot at or above it is dead
         * by the claim_top invariant, so take the first of those. */
        if (tr->claim_top < GTA_CLAIM_MAX)
            free_slot = tr->claim_top;
        else
            return 1;                        /* fail open, see above */
    }
    if (free_slot >= tr->claim_top)
        tr->claim_top = free_slot + 1;
    tr->claim_x[free_slot]   = (unsigned char)rx;
    tr->claim_y[free_slot]   = (unsigned char)ry;
    tr->claim_z[free_slot]   = (signed char)z;
    tr->claim_car[free_slot] = serial;
    tr->claim_ttl[free_slot] = GTA_CLAIM_TTL;
    tr->claim_seen[free_slot] = 0;
    return 1;
}

/* AND A CAR GIVES EACH SQUARE BACK AS ITS BODY LEAVES IT - the developer's
 * rule, implemented in the sweep in gta_traffic_tick(): a booked square is
 * released the moment the owner's body has covered it and then completely
 * left it. Length-aware for free, because the body test is the same
 * oriented-box test the rest of the file uses - a bus frees its squares
 * later than a saloon. No timer releases ground under a standing car. */
/* THE ROAD A CAR IS ABOUT TO CROSS IS ITS OWN, BLOCK BY BLOCK.
 *
 * "jesli ktos jedzie prosto to cala droga na skrzyzowaniu jest przez niego
 *  zajeta i nie powinno takiemu wjechac na skrzyzowaniu"
 *
 * Exactly that, and at the granularity that makes it affordable. Claiming the
 * whole junction ROOT was measured and it strangles the city - a crossing is
 * up to four blocks across, so one car holding all of it shuts out the arms it
 * is never going to touch: 96% of the fleet moving down to 36%, and 83% down
 * to 30%.
 *
 * What a car actually occupies is its ROUTE through the crossing - turn and all -
 * plus the landing square beyond, not just the line straight ahead
 * of it, from the edge it enters by to the far side. Two cars crossing the
 * same junction on paths that do not share a block do not obstruct each other
 * and are both allowed through; two whose paths cross are not.
 *
 * Returns 1 and takes the whole line when every block on it is free or already
 * this car's; returns 0 and takes NOTHING when any of it belongs to somebody
 * else - all-or-nothing, because half a path reserved is a car stopped in the
 * middle of a crossing, which is the fault this exists to prevent. */
static int block_full(const gta_traffic *tr, int idx, int bx, int by,
                      int layer);

/* THE BOOKED PATH IS THE REAL PATH - the ROUTE's blocks through the box, turn
 * and all, not a straight ray from the entry - plus the LANDING block beyond,
 * so two cars whose turns converge on one exit lane cannot both be let in.
 * All-or-nothing exactly as before: any square somebody else's, nothing is
 * taken. force_straight books the straight run instead of the route - the
 * "left is busy, go straight" fallback books what the car will then drive. */
static int route_exit_side(const gta_map *m, const gta_car *c,
                           int nx, int ny, int dxs, int dys,
                           int *olx, int *oly);

/* THE BOOKED SHAPE IS THE DRIVEN SHAPE. The entry line is walked from where
 * the car REALLY is (never from a route node - the route's column and the
 * driving lane's column are different blocks half the time, and matching
 * them exactly is how a turner booked a straight line it was not going to
 * drive); the route contributes only the DECISION - straight, left or
 * right - and the turn's squares are the geometric L: a right turn pivots
 * on the near square, a left crosses to the far one, the exit line runs
 * from the pivot to the box edge, then the landing square. */
static int claim_route(gta_traffic *tr, const gta_map *m, gta_car *c,
                       int idx, int nx, int ny, int dxs, int dys,
                       int force_mode, int join_only)
{
    int force_straight = (force_mode == 1);
    unsigned long joinid = 0;
    const gta_car *jtail = 0;
    int did_chain = 0;
    int bbx = -1, bby = -1, bdir = 255;
    int cbx = -1, cby = -1;
    int px[2 * GTA_PATH_MAX + 3], py[2 * GTA_PATH_MAX + 3], n;
    int lx = -1, ly = -1;
    int exd = dxs, eyd = dys;
    int k, i, depth = 0;
    int x = nx, y = ny;

    /* THE SHAPE IS SYNTHESISED, NEVER COPIED. Route nodes are block-level
     * and lane-blind - a BFS path staircases between the columns of a wide
     * road, and copying its shape gave a straight car a booking with a
     * sideways step into the neighbour's column (and, once the shape
     * became binding, a drive that lurched a column mid-box - the fire
     * truck photograph). The route contributes exactly two facts - which
     * way the car leaves and WHERE it lands - and the squares are built
     * from geometry: straight = the car's own column; a turn = the entry
     * column to the landing's row, the exit run to the landing, and the
     * arc's inner-corner square. */
    n = 0;
    g_claim_why = 0;
    g_claim_fell = 0;
    if (force_mode == 2 || force_mode == 3) {
        /* A TURN BY THE ARROWS, NO ROUTE. The ladder's last rungs: the
         * route is unusable from this lane (its straight runs on the
         * neighbour column and OURS ends at the box - the narrowing the
         * probe caught at (36,85) and (59,95), why 17 side 0) - so the car
         * takes a turn the map allows, books it, and re-plans beyond. A
         * right turn pivots on the first box square, a left crosses to the
         * deepest one in the entry column; the exit run walks out of the
         * box and the landing must be legal road, or this rung fails too. */
        int side = (force_mode == 2) ? 1 : 2;
        int ok = 0;

        exd = (side == 2) ? dys : -dys;
        eyd = (side == 2) ? -dxs : dxs;
        x = nx;
        y = ny;
        while (n < 2 * GTA_PATH_MAX && is_junction(m, x, y, c->layer)) {
            px[n] = x;
            py[n] = y;
            n++;
            bbx = x;
            bby = y;
            if (side == 1)
                break;                  /* right: pivot on the near square */
            x += dxs;
            y += dys;
        }
        if (n > 0) {
            x = bbx + exd;
            y = bby + eyd;
            while (n < 2 * GTA_PATH_MAX + 1 &&
                   is_junction(m, x, y, c->layer)) {
                px[n] = x;
                py[n] = y;
                n++;
                x += exd;
                y += eyd;
            }
            if (!is_junction(m, x, y, c->layer)) {
                int ang3 = (eyd < 0) ? 0 : (eyd > 0) ? 128
                         : (exd > 0) ? 64 : 192;
                if (ground_at(m, x, y, c->layer) == GROUND_ROAD &&
                    dir_allowed(m, x, y, c->layer, ang3)) {
                    lx = x;
                    ly = y;
                    bdir = ang3;
                    cbx = bbx - dxs + exd;
                    cby = bby - dys + eyd;
                    ok = 1;
                }
            }
        }
        if (!ok) {
            g_claim_why = 17;
            return 0;
        }
    } else if (!force_straight) {
        int rlx = -1, rly = -1;
        int side = route_exit_side(m, c, nx, ny, dxs, dys, &rlx, &rly);

        g_claim_side = side;

        /* LANE DISCIPLINE, DECIDED AT THE BOOKING. A right turn belongs to
         * the outermost right lane and a left to the outermost left - on a
         * two-lane carriageway the INNER lane cutting right slices across
         * the outer one, and the painted arrows say so ("z drugiej lane nie
         * powinno sie dac skretow w prawo"). The old version vetoed this at
         * the commit, which the booked-bend rule rightly overrules - so it
         * moved HERE: a turn from the wrong lane is never booked at all,
         * the car books straight instead and re-plans beyond. The check: is
         * the square beside my APPROACH square, on the turning side, road
         * running my way? Then another lane of my own carriageway lies
         * between me and that turn. */
        if (side == 1 || side == 2) {
            int svx = (side == 1) ? -dys : dys;
            int svy = (side == 1) ? dxs : -dxs;
            int ax2 = nx - dxs, ay2 = ny - dys;
            int ang2 = (dys < 0) ? 0 : (dys > 0) ? 128
                     : (dxs > 0) ? 64 : 192;

            if (ground_at(m, ax2 + svx, ay2 + svy, c->layer) == GROUND_ROAD &&
                dir_allowed(m, ax2 + svx, ay2 + svy, c->layer, ang2)) {
                /* ...but only when straight on actually LEADS anywhere. At
                 * a T-junction the inner lane's turn may be the only legal
                 * move, and booking it "straight" instead drove a car into
                 * the wall and wedged it for two minutes (measured at
                 * (50,44): flow 37%, one car stood 132 s). Terrain beats
                 * lane discipline. */
                int wx = nx, wy = ny, steps2 = 0;

                while (steps2 < 2 * GTA_PATH_MAX &&
                       is_junction(m, wx, wy, c->layer)) {
                    wx += dxs;
                    wy += dys;
                    steps2++;
                }
                if (ground_at(m, wx, wy, c->layer) == GROUND_ROAD &&
                    dir_allowed(m, wx, wy, c->layer, ang2))
                    side = 0;
            }
        }

        if ((side == 1 || side == 2) && rlx >= 0) {
            int ok = 0;

            exd = (side == 2) ? dys : -dys;
            eyd = (side == 2) ? -dxs : dxs;
            bbx = (dxs != 0) ? rlx : nx;
            bby = (dxs != 0) ? ny : rly;
            x = nx;
            y = ny;
            while (n < 2 * GTA_PATH_MAX &&
                   is_junction(m, x, y, c->layer)) {
                px[n] = x;
                py[n] = y;
                n++;
                if (x == bbx && y == bby) {
                    ok = 1;
                    break;
                }
                x += dxs;
                y += dys;
            }
            if (ok) {
                x = bbx + exd;
                y = bby + eyd;
                while (n < 2 * GTA_PATH_MAX + 1 &&
                       !(x == rlx && y == rly) &&
                       is_junction(m, x, y, c->layer)) {
                    px[n] = x;
                    py[n] = y;
                    n++;
                    x += exd;
                    y += eyd;
                }
                if (x == rlx && y == rly) {
                    lx = rlx;
                    ly = rly;
                    bdir = (eyd < 0) ? 0 : (eyd > 0) ? 128
                         : (exd > 0) ? 64 : 192;
                    cbx = bbx - dxs + exd;
                    cby = bby - dys + eyd;
                } else {
                    ok = 0;
                }
            }
            if (!ok) {
                /* the geometry did not line up (an odd complex): book the
                 * straight run and let the route re-plan on the far side */
                g_claim_fell = 1;
                n = 0;
                bbx = -1;
                bby = -1;
                bdir = 255;
                cbx = -1;
                cby = -1;
                exd = dxs;
                eyd = dys;
                lx = -1;
                ly = -1;
            }
        }
    }
    if (n == 0) {
        depth = 0;
        x = nx;
        y = ny;
        while (depth < 2 * GTA_PATH_MAX && is_junction(m, x, y, c->layer)) {
            px[depth] = x;
            py[depth] = y;
            depth++;
            x += dxs;
            y += dys;
        }
        if (depth == 0)
            return 1;                   /* not a crossing: nothing to hold */
        n = depth;
        if (!is_junction(m, x, y, c->layer)) {
            lx = x;
            ly = y;
        }
        /* A STRAIGHT BOOKING NEVER LANDS OFF THE ROAD - any straight
         * booking, not only the "left is busy, go straight instead"
         * redirection. The silent fallback (a turn whose geometry did not
         * line up) was booking the straight run at T-junctions, where
         * straight on is the pavement opposite the mouth - and the shape
         * being binding, the car then DROVE there (the developer's
         * photograph of a car parked on the pavement across from the side
         * street). If the landing is not road running our way there is
         * nothing legal to book: refuse, the gate holds the car, and the
         * route re-plans. */
        {
            int ang = (dys < 0) ? 0 : (dys > 0) ? 128 : (dxs > 0) ? 64 : 192;
            if (lx >= 0) {
                if (ground_at(m, lx, ly, c->layer) != GROUND_ROAD ||
                    !dir_allowed(m, lx, ly, c->layer, ang)) {
                    g_claim_why = 17;
                    return 0;
                }
            } else if (force_straight) {
                g_claim_why = 19;
                return 0;               /* no landing found to redirect onto */
            }
        }
    }
    if (cbx >= 0 && cbx >= 0 && cbx < GTA_MAP_DIM &&
        cby >= 0 && cby < GTA_MAP_DIM) {
        /* THE ARC'S INNER-CORNER SQUARE. The booked shape is
         * square-aligned; the body sweeps a circle that cuts the corner
         * between approach and exit, clipping the diagonal square that
         * lies in neither line - two bookings disjoint on paper met there
         * in the flesh. It is booked too, so a path that would meet the
         * sweep is refused with everything else. */
        px[n] = cbx;
        py[n] = cby;
        n++;
    }
    if (lx >= 0) {
        /* ROOM TO LAND CLEAR, NOT JUST A LANDING SQUARE. One free square
         * lets a car in whose exit lane is jammed one square further on -
         * it lands, stops behind the queue, and its tail is still in the
         * box. Measured: 95%+ of stopped-in-the-box ticks were exactly
         * that. So the exit lane must hold the WHOLE vehicle plus the
         * bumper gap - length-aware, a bus asks for more squares than a
         * saloon. A square that is another junction ends the scan: that
         * box is its own gate's business. */
        const gta_car_info *ci = &tr->tiles->cars[c->model];
        int sq = (gta_car_world_len(ci) +
                  (int)(GTA_TRAFFIC_MIN_GAP >> FP) + 31) / 32;
        int q, qx = lx, qy = ly;
        int chained = 0;

        (void)did_chain;

        for (q = 0; q < sq; q++) {
            if (is_junction(m, qx, qy, c->layer)) {
                chained = 1;            /* box B starts before the car fits */
                break;
            }
            if (block_full(tr, idx, qx, qy, c->layer)) {
                g_claim_why = 18;
                return 0;               /* not enough room to land clear */
            }
            qx += exd;
            qy += eyd;
        }
        px[n] = lx;
        py[n] = ly;
        n++;
        /* CROSSINGS TOO CLOSE TO STAND BETWEEN - BOOK THE WHOLE CORRIDOR.
         * "przez to ze sa blisko siebie [...] wjezdzajac na 2. skrzyzowanie
         * nie rezerwuje drogi" - when the gap between one box's exit and
         * the next cannot hold this vehicle plus its bumper gap, entering
         * the first commits the car to the next, and in the dense wards
         * (the alley grid around (231,40), a box every second block) to
         * the one after that. So the booking keeps walking: gap squares,
         * the next box straight through, again - until a gap long enough
         * to REST in appears. Up to three boxes; a corridor longer than
         * that, or one that overflows the shape, is refused - and every
         * square pushed here goes through the same all-or-nothing below,
         * where the resting squares also refuse over a MOVING body (see
         * centre_on: a moving car can be STOPPED on them by its own
         * gate, which is how the (80,18) deadlock ring was born). */
        if (chained) {
            int hops = 0, done2 = 0, q2, r2;

            did_chain = 1;
            int cap = (int)(sizeof(px) / sizeof(px[0]));
            int ang2 = (eyd < 0) ? 0 : (eyd > 0) ? 128
                     : (exd > 0) ? 64 : 192;

            for (q2 = 1; q2 < q; q2++) {        /* the too-short gap */
                if (n >= cap) { g_claim_why = 18; return 0; }
                px[n] = lx + q2 * exd;
                py[n] = ly + q2 * eyd;
                n++;
            }
            while (hops < 2 && !done2) {
                int steps2 = 0, gap2 = 0;

                while (steps2 < 2 * GTA_PATH_MAX &&
                       is_junction(m, qx, qy, c->layer)) {
                    if (n >= cap) { g_claim_why = 18; return 0; }
                    px[n] = qx;
                    py[n] = qy;
                    n++;
                    qx += exd;
                    qy += eyd;
                    steps2++;
                }
                if (is_junction(m, qx, qy, c->layer)) {
                    g_claim_why = 18;
                    return 0;           /* the box never ended */
                }
                if (ground_at(m, qx, qy, c->layer) != GROUND_ROAD ||
                    !dir_allowed(m, qx, qy, c->layer, ang2)) {
                    g_claim_why = 17;
                    return 0;           /* nothing legal past this box */
                }
                while (gap2 < sq &&
                       !is_junction(m, qx + gap2 * exd, qy + gap2 * eyd,
                                    c->layer))
                    gap2++;
                if (gap2 >= sq) {
                    /* a gap the vehicle fits in: land on its first square,
                     * the room beyond it must be clear of bodies */
                    for (r2 = 0; r2 < sq; r2++)
                        if (block_full(tr, idx, qx + r2 * exd,
                                       qy + r2 * eyd, c->layer)) {
                            g_claim_why = 18;
                            return 0;
                        }
                    if (n >= cap) { g_claim_why = 18; return 0; }
                    px[n] = qx;
                    py[n] = qy;
                    n++;
                    done2 = 1;
                } else {
                    /* too short again: book the gap and walk on */
                    for (r2 = 0; r2 < gap2; r2++) {
                        if (n >= cap) { g_claim_why = 18; return 0; }
                        px[n] = qx;
                        py[n] = qy;
                        n++;
                        qx += exd;
                        qy += eyd;
                    }
                    hops++;
                }
            }
            if (!done2) {
                g_claim_why = 18;
                return 0;               /* a corridor with no rest in it */
            }
        }
    }

    /* Every square first, before anything is taken - and BODIES as well as
     * claims. A car waiting at a red light or at another gate holds no
     * claim, yet its body can be standing ON one of our squares (the hint
     * ring of a big complex puts stop lines on junction squares) - booking
     * "through" it commits a car into a guaranteed bumper stop inside the
     * box. Traced: a turner at (60,45) arcing into an eastbound car
     * standing at its light on (59,46). */
    /* THE CONVOY JOIN - the developer's design: a car whose booked shape
     * is EXACTLY the one a car already crossing holds (same landing, same
     * bend - "musza miec ten sam wyjazd") does not wait for the release;
     * it rides the same route id. Conflicts with exactly ONE other id are
     * therefore not an automatic refusal: if that id's tail is committed
     * and its exit matches ours, we may adopt the route - and from then on
     * WE are the tail, so the leader stops releasing ("to 1. auto nie
     * powinno od tego momentu kasowac juz swojej trasy"). */
    for (k = 0; k < n; k++)
        for (i = 0; i < tr->claim_top; i++)
            if (tr->claim_ttl[i] > 0 &&
                tr->claim_x[i] == (unsigned char)px[k] &&
                tr->claim_y[i] == (unsigned char)py[k] &&
                tr->claim_z[i] == (signed char)c->layer &&
                tr->claim_car[i] != c->serial) {
                if (joinid == 0)
                    joinid = tr->claim_car[i];
                else if (tr->claim_car[i] != joinid) {
                    g_claim_why = 15;
                    return 0;           /* two different crossers: no */
                }
            }
    if (joinid != 0 && did_chain) {
        gta_join_why[1]++;
        g_claim_why = 15;
        return 0;                       /* corridors are not for convoys:
                                         * the joiner would stand INSIDE a
                                         * box behind its leader - measured
                                         * 7046 SIB car-ticks at (48,82) */
    }
    if (joinid != 0) {
        for (i = 0; i < tr->n; i++)
            if (!tr->cars[i].done && tr->cars[i].serial == joinid) {
                jtail = &tr->cars[i];
                break;
            }
        if (!jtail) {
            gta_join_why[2]++;
            g_claim_why = 15;
            return 0;                   /* nobody to ride with any more */
        }
        /* A tail that is NO LONGER crossing left this box - its remaining
         * claims are body-leave latency and decaying leftovers. If OUR
         * shape is the same one, we TAKE the route OVER instead of
         * waiting out the decay ("nastepne jada ich trasa - maja
         * korzystac z okazji"). Its book/bend fields still describe THIS
         * box only until its next grant, so a leader already booked into
         * the next crossing fails the shape test below and nothing of its
         * new booking can be stolen. */
        if (lx < 0 ||
            jtail->book_lx != lx || jtail->book_ly != ly ||
            jtail->bend_bx != bbx || jtail->bend_by != bby ||
            jtail->bend_dir != bdir) {
            gta_join_why[3]++;
            g_claim_why = 15;
            return 0;                   /* not the same route: wait */
        }
        /* THE FAIRNESS VALVE. A convoy that keeps admitting joiners never
         * releases - each joiner moves the release to ITS tail, and a busy
         * arm becomes a green wave that starves every other: measured 119 s
         * of why-15 waiting at (121,250). So a join is allowed only while
         * NOBODY ELSE is stood at this crossing waiting on somebody's
         * claims - the moment cross traffic queues, the convoy is closed,
         * the current tail releases on its way out, and the queue gets its
         * turn. Smoothing in light traffic, fairness in heavy. */
        for (i = 0; i < tr->n; i++) {
            const gta_car *w = &tr->cars[i];
            int wdx, wdy;

            if (i == idx || w->done || w->layer != c->layer)
                continue;
            if (w->serial == joinid || w->convoy == joinid)
                continue;
            if (w->speed > 0 || w->hold != GTA_HOLD_BOX)
                continue;
            /* ...and only waiters from ANOTHER arm close the convoy. A car
             * queued BEHIND the joiner holds a box refusal too - counting
             * it meant the joiner's own queue vetoed every join, and the
             * developer watched followers "czekac na calkowicie wolna
             * droge" while the ground behind the leader stood empty. The
             * queue on our own arm only GAINS from the join. */
            if (w->angle == c->angle)
                continue;
            wdx = (int)(w->x >> (FP + 5)) - nx;
            wdy = (int)(w->y >> (FP + 5)) - ny;
            if (wdx < 0) wdx = -wdx;
            if (wdy < 0) wdy = -wdy;
            if (wdx + wdy <= 8) {
                gta_join_why[4]++;
                g_claim_why = 15;
                return 0;               /* somebody is waiting: no join */
            }
        }
    } else if (join_only) {
        return 0;                       /* nothing to join here */
    }

    for (k = 0; k < n; k++) {
        if (body_on_sq(tr, idx, px[k], py[k], c->layer, 1, joinid)) {
            if (joinid != 0) gta_join_why[5]++;
            g_claim_why = 16;
            return 0;                   /* a body is parked on our path */
        }
        if (!is_junction(m, px[k], py[k], c->layer) &&
            body_on_sq(tr, idx, px[k], py[k], c->layer, 0, joinid)) {
            if (joinid != 0) gta_join_why[5]++;
            g_claim_why = 16;
            return 0;                   /* somebody may STOP on this square */
        }
    }

    for (k = 0; k < n; k++)
        junction_claim(tr, px[k], py[k], c->layer, c->serial);
    if (joinid != 0) {
        /* adopt: the whole route becomes OURS to release. Seen flags are
         * cleared - they were the old tail's; a square freed because OUR
         * body "left" ground it never touched would open the leader's
         * path under it. */
        if (jtail->crossing) {
            /* LIVE JOIN: the whole route becomes ours, riders follow */
            for (i = 0; i < tr->claim_top; i++)
                if (tr->claim_ttl[i] > 0 && tr->claim_car[i] == joinid) {
                    tr->claim_car[i] = c->serial;
                    tr->claim_seen[i] = 0;
                }
            for (i = 0; i < tr->n; i++)
                if (!tr->cars[i].done &&
                    (tr->cars[i].serial == joinid ||
                     tr->cars[i].convoy == joinid))
                    tr->cars[i].convoy = c->serial;
        } else {
            /* TAKEOVER of a dead route: only the claims ON OUR SHAPE are
             * re-owned; nobody's convoy field moves - there are no riders,
             * and the old tail may be committed somewhere new under its
             * own serial. */
            for (i = 0; i < tr->claim_top; i++) {
                if (tr->claim_ttl[i] <= 0 || tr->claim_car[i] != joinid)
                    continue;
                for (k = 0; k < n; k++)
                    if (tr->claim_x[i] == (unsigned char)px[k] &&
                        tr->claim_y[i] == (unsigned char)py[k] &&
                        tr->claim_z[i] == (signed char)c->layer) {
                        tr->claim_car[i] = c->serial;
                        tr->claim_seen[i] = 0;
                        break;
                    }
            }
        }
        tr->stat_joins++;
        gta_join_why[0]++;
    }
    c->convoy = c->serial;
    c->book_lx = lx;
    c->book_ly = ly;
    c->bend_bx = bbx;
    c->bend_by = bby;
    c->bend_dir = bdir;
    return 1;
}

/* Which way does the ROUTE leave this crossing? 0 straight, 1 right, 2 left,
 * -1 not known (no route through it, or it never leaves within the scan).
 * Screen y grows downwards, so left of heading (dx,dy) is (dy,-dx). */
static int route_exit_side(const gta_map *m, const gta_car *c,
                           int nx, int ny, int dxs, int dys,
                           int *olx, int *oly)
{
    int k, k0 = -1, ox0 = 0, oy0 = 0, lastx = nx, lasty = ny, ex, ey;

    if (c->path_i >= c->path_n)
        return -1;
    /* TOLERANT MATCH: accept a route node in the entry square OR one block
     * to either side of it ACROSS the heading - the route's column and the
     * driving lane's column are different blocks half the time, and the
     * exact match was how a turner's route went unrecognised and its
     * booking silently became a straight line. */
    for (k = (c->path_i > 1 ? c->path_i - 2 : 0);
         k < c->path_n && k < c->path_i + 5; k++) {
        int ox = c->path[k].x - nx, oy = c->path[k].y - ny;
        int lat = ox * dys - oy * dxs;
        if (ox * dxs + oy * dys == 0 && lat >= -1 && lat <= 1) {
            k0 = k;
            ox0 = nx - c->path[k].x;
            oy0 = ny - c->path[k].y;
            break;
        }
    }
    if (k0 < 0)
        return -1;
    /* Twelve, not six: with the box the full overlap RECTANGLE a route
     * through a deep compound holds more in-box nodes than the old sparse
     * arrow test ever saw, and a BFS staircase spends two nodes a step. */
    for (k = k0; k < c->path_n && k < k0 + 12 &&
                 is_junction(m, c->path[k].x, c->path[k].y, c->layer); k++) {
        lastx = c->path[k].x;
        lasty = c->path[k].y;
    }
    if (k >= c->path_n || k >= k0 + 12 ||
        is_junction(m, c->path[k].x, c->path[k].y, c->layer))
        return -1;
    ex = c->path[k].x - lastx;
    ey = c->path[k].y - lasty;
    if (olx) {
        *olx = c->path[k].x + ox0;      /* the landing, in OUR column */
        *oly = c->path[k].y + oy0;
    }
    if (ex == dxs && ey == dys)
        return 0;
    if (ex == dys && ey == -dxs)
        return 2;                       /* left */
    if (ex == -dys && ey == dxs)
        return 1;                       /* right */
    return -1;
}

/* IS THERE A STOPPED CAR IN THIS BLOCK? The junction-entry rule wants to know
 * whether the EXIT of a crossing is available before a car commits to the box,
 * and "a stationary vehicle's centre is in it" is the answer that cannot
 * over-block: a moving car will have vacated by the time the asker arrives,
 * and a stopped one will not. */
/* IS THIS VEHICLE'S BODY OVER THIS BLOCK - not just its middle?
 *
 * "zobacz na te cysterne - wszyscy przez nia przejezdzaja. zamiast zajac te
 *  alejke co jest przed nia, to stoi a wszyscy po niej jezdza jakby nie
 *  istniala"
 *
 * Every block-level occupancy test in this file asked whether another car's
 * CENTRE was in the block. A tanker is sixty world pixels long and a block is
 * thirty-two, so a stopped long vehicle lies across two or three blocks and
 * every one of them except the middle read as empty. Traffic drove through it.
 *
 * A block is a 32x32 axis-aligned square, which is an oriented box with an
 * angle of zero, so the same separating-axis test the rest of the file uses
 * answers this exactly. */
static int car_on_block(const gta_traffic *tr, const gta_car *o,
                        int bx, int by)
{
    const gta_car_info *oi = &tr->tiles->cars[o->model];
    long cx = ((long)bx * 32 + 16) << FP;
    long cy = ((long)by * 32 + 16) << FP;
    long dx = (o->x - cx) >> FP, dy = (o->y - cy) >> FP;

    /* Nothing further than half a block plus half a bus can reach it. */
    if (dx > 64 || dx < -64 || dy > 64 || dy < -64)
        return 0;

    /* THE WORLD-AXIS HALF OF THE SAT, done first with the trigonometry read
     * once. The block is axis-aligned, so two of box_hit's four axes ARE the
     * world axes, and the body's exact projections on them are
     * |sin|*hl + |cos|*hw and |cos|*hl + |sin|*hw. A centre further apart
     * than projection + 16 on either axis is exactly what box_hit's own
     * axis-2/3 tests refuse - same answer, four multiplies instead of
     * sixteen. This is the hot question of the whole tick: occ_rebuild asks
     * it for the 3x3 blocks around every car, every tick, and most of those
     * blocks miss. Measured on the 68020 (tickprof): occ 2970 us of a 9000 us
     * tick before this.
     *
     * The naive Chebyshev bound (hl + hw) was tried first and never fired -
     * every asked block is within 48 px, the bound starts at 53. */
    {
        int hl = gta_car_world_len(oi) / 2, hw = gta_car_world_wid(oi) / 2;
        long s = gta_sin(o->face), co = gta_cos(o->face);
        long rx, ry;

        if (s < 0)  s = -s;
        if (co < 0) co = -co;
        rx = (s * hl + co * hw) >> 14;
        ry = (co * hl + s * hw) >> 14;
        if (dx > rx + 16 || dx < -(rx + 16) ||
            dy > ry + 16 || dy < -(ry + 16))
            return 0;
        return box_hit(cx, cy, 0, 16, 16, o->x, o->y, o->face, hl, hw);
    }
}

/* WHO IS STANDING ON THIS JUNCTION CELL - 0 for nobody.
 *
 * See GTA_OCC_MAX for the design. The table is small and rebuilt every tick,
 * so a linear scan is both simplest and fastest here. */
static unsigned long occ_owner(const gta_traffic *tr, int bx, int by, int z)
{
    int i = tr->occ_head[bx & 15];

    while (i) {
        int k = i - 1;
        if (tr->occ_x[k] == (unsigned char)bx &&
            tr->occ_y[k] == (unsigned char)by &&
            tr->occ_z[k] == (signed char)z)
            return tr->occ_car[k];
        i = tr->occ_next[k];
    }
    return 0;
}

/* Rebuild the whole matrix from where the vehicles actually are.
 *
 * Every junction block any part of a car's body covers is marked with that
 * car's serial. Rebuilding rather than maintaining is what makes "the cell
 * frees itself the moment the car is off it" true without a single release
 * path to get wrong - and there have been three of those in this file already.
 *
 * A cell already taken is left alone: whoever got there first in the fleet
 * order keeps it, and the other car is the one that has to wait. That is
 * arbitrary but stable, which is all a tie-break has to be. */
static void occ_rebuild(gta_traffic *tr, const gta_map *m)
{
    int i, dx, dy;

    tr->occ_n = 0;
    memset(tr->occ_head, 0, sizeof tr->occ_head);
    if (!(tr->opt_occ_hold || tr->opt_occ_look) || !tr->tiles)
        return;

    for (i = 0; i < tr->n; i++) {
        const gta_car *o = &tr->cars[i];
        const gta_car_info *oi;
        int bx, by, hl, hw;
        long s, co, rx, ry;

        if (o->done)
            continue;
        bx = (int)(o->x >> (FP + 5));
        by = (int)(o->y >> (FP + 5));

        /* The body's exact world-axis projections, computed ONCE per car
         * rather than once per asked block - the same arithmetic
         * car_on_block does, hoisted out of the 3x3 loop. See the note
         * there for why the pre-test equals box_hit's own world-axis half. */
        oi = &tr->tiles->cars[o->model];
        hl = gta_car_world_len(oi) / 2;
        hw = gta_car_world_wid(oi) / 2;
        s = gta_sin(o->face); co = gta_cos(o->face);
        if (s < 0)  s = -s;
        if (co < 0) co = -co;
        rx = ((s * hl + co * hw) >> 14) + 16;
        ry = ((co * hl + s * hw) >> 14) + 16;

        for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
            int cx = bx + dx, cy = by + dy;
            long ddx, ddy;
            if (tr->occ_n >= GTA_OCC_MAX)
                return;
            /* EVERY BLOCK, NOT ONLY THE JUNCTION ONES. Restricting the
             * matrix to crossings left the approach lanes unguarded, and a
             * vehicle stopped at the mouth of a junction is exactly the one
             * that STICKS OUT into them - "oba auta stoja wystajace i wszystko
             * po nich przejezdza". With junction blocks only, the penetration
             * count went UP at two of three sites. */
            ddx = (o->x >> FP) - ((long)cx * 32 + 16);
            ddy = (o->y >> FP) - ((long)cy * 32 + 16);
            if (ddx > rx || ddx < -rx || ddy > ry || ddy < -ry)
                continue;
            if (!box_hit(((long)cx * 32 + 16) << FP,
                         ((long)cy * 32 + 16) << FP, 0, 16, 16,
                         o->x, o->y, o->face, hl, hw))
                continue;
            if (occ_owner(tr, cx, cy, o->layer) != 0)
                continue;               /* somebody was here first */
            tr->occ_x[tr->occ_n]   = (unsigned char)cx;
            tr->occ_y[tr->occ_n]   = (unsigned char)cy;
            tr->occ_z[tr->occ_n]   = (signed char)o->layer;
            tr->occ_car[tr->occ_n] = o->serial;
            tr->occ_next[tr->occ_n] = tr->occ_head[cx & 15];
            tr->occ_head[cx & 15] = (unsigned char)(tr->occ_n + 1);
            tr->occ_n++;
        }
    }
}

/* THE CELLS A CORNER WILL SWEEP - free? and then MINE.
 *
 * See GTA_TURN_LOOK. The arc is a quarter circle whose centre and radius are
 * known before a wheel turns, so the blocks it will pass over can simply be
 * listed: five samples round it, plus - for a long vehicle - the two cells on
 * the OUTSIDE of the corner, which is ground a bus sweeps and a saloon never
 * touches.
 *
 * `commit` 0 asks the question, which is what a car does while it is still
 * approaching so that a refusal is an early lift off the accelerator. `commit`
 * 1 asks it and TAKES the cells, so that nothing else may enter them while the
 * corner is being driven.
 *
 * A cell is not free when another vehicle's BODY is on it (the occupancy
 * matrix, rebuilt every tick) or when another vehicle has already booked it
 * (the claim table, which survives between ticks and is what a reservation
 * has to be). */
static int arc_reserve(gta_traffic *tr, const gta_map *m, gta_car *c,
                       long cx, long cy, int r_px, int from_ang, int dir,
                       int long_car, int commit, int s0)
{
    int bxs[GTA_ARC_CELLS], bys[GTA_ARC_CELLS], n = 0;
    int s, i, k;

    /* FROM WHERE THE CAR IS, FORWARD. Booking the whole circle once and
     * holding it until the car is out the far side reserves eight to twelve
     * blocks - most of a crossing - and took the city from 77/67/68% of the
     * fleet moving to 26/31/28% (the notes 40). Re-booking each tick from
     * the sample the car has reached means the ground behind it frees itself
     * while it is still turning, which is the developer's own rule for the
     * matrix applied to the corner. */
    for (s = s0; s <= 4; s++) {
        int phi = s * (GTA_TURN_QUARTER / 4);
        int ang = (from_ang + dir * phi) & 255;
        int rd  = (ang - dir * GTA_TURN_QUARTER) & 255;
        long px = cx + (((long)r_px * gta_sin(rd)) << 2);
        long py = cy - (((long)r_px * gta_cos(rd)) << 2);
        int bx = (int)(px >> (FP + 5)), by = (int)(py >> (FP + 5));
        int extra;

        /* AND THE OUTER CELL ONLY BESIDE THE MIDDLE OF THE ARC, which is
         * where a long vehicle's tail actually swings out. One beside every
         * sample doubled the footprint for nothing. */
        for (extra = 0; extra <= ((long_car && s == 2) ? 1 : 0); extra++) {
            int ex = bx, ey = by;
            if (extra) {
                /* one cell to the OUTSIDE of the corner */
                int odx, ody;
                heading_step((ang - dir * GTA_TURN_QUARTER) & 255, &odx, &ody);
                ex -= odx;
                ey -= ody;
            }
            for (k = 0; k < n; k++)
                if (bxs[k] == ex && bys[k] == ey) break;
            if (k < n || n >= GTA_ARC_CELLS)
                continue;
            bxs[n] = ex;
            bys[n] = ey;
            n++;
        }
    }

    for (k = 0; k < n; k++) {
        unsigned long who = occ_owner(tr, bxs[k], bys[k], c->layer);
        if (who != 0 && who != c->serial)
            return 0;
        for (i = 0; i < tr->claim_top; i++)
            if (tr->claim_ttl[i] > 0 &&
                tr->claim_x[i] == (unsigned char)bxs[k] &&
                tr->claim_y[i] == (unsigned char)bys[k] &&
                tr->claim_z[i] == (signed char)c->layer &&
                tr->claim_car[i] != c->serial)
                return 0;
    }
    if (commit)
        for (k = 0; k < n; k++)
            junction_claim(tr, bxs[k], bys[k], c->layer, c->serial);
    return 1;
}

static int block_full(const gta_traffic *tr, int idx, int bx, int by, int layer)
{
    int i;

    for (i = 0; i < tr->n; i++) {
        const gta_car *o = &tr->cars[i];
        if (i == idx || o->done || o->layer != layer)
            continue;
        if (o->speed > 0)
            continue;
        /* THE CENTRE, on purpose. This is the "do not turn into a full exit
         * lane" test, and asking it by BODY refuses a turn whenever a queued
         * car's bumper reaches over the line - measured at a quarter of the
         * city's flow (90% -> 67% at (64,64)) with the overlap count flat.
         * The body test belongs where a vehicle is ACROSS somebody's path,
         * which is box_busy(). */
        if ((int)(o->x >> (FP + 5)) == bx && (int)(o->y >> (FP + 5)) == by)
            return 1;
    }
    return 0;
}

/* Move `v` towards `want` by at most `up` when speeding up and `down` when
 * slowing. Braking is allowed to be harder than accelerating, which is both
 * true of cars and what stops a queue concertina-ing. */
/* Does this car hold a claim on exactly that block? The handover gate asks
 * it to tell "approaching a box I already booked" from "approaching one I
 * have not" - only the second may ask again. */
static int claim_mine(const gta_traffic *tr, const gta_car *c, int x, int y)
{
    int i;

    for (i = 0; i < tr->claim_top; i++)
        if (tr->claim_ttl[i] > 0 &&
            tr->claim_car[i] == c->convoy &&
            (int)tr->claim_x[i] == x &&
            (int)tr->claim_y[i] == y &&
            (int)tr->claim_z[i] == c->layer)
            return 1;
    return 0;
}

static long approach(long v, long want, long up, long down)
{
    if (v < want) { v += up;   if (v > want) v = want; }
    else if (v > want) { v -= down; if (v < want) v = want; }
    return v;
}

/* --- driving, the original's way -----------------------------------------
 *
 * Everything from here to gta_traffic_tick() is a reading of GTA 1's own AI
 * driver: it steers a car along a ROUTE, turning by a fixed number of angle
 * units a frame, and sets the speed from how many blocks of road are clear in
 * front. The notes carry the evidence for each rule; the constants are in
 * gta_traffic.h with the arithmetic that converts the original's units into
 * ours.
 */

/* Which way the route wants to leave this block, or -1 if it has nothing to
 * say. Nodes the car has already reached are consumed here rather than in the
 * caller, so arriving and deciding are the same step. */
static int route_dir(gta_car *c, int bx, int by)
{
    while (c->path_i < c->path_n) {
        int nx = c->path[c->path_i].x;
        int ny = c->path[c->path_i].y;

        if (nx == bx && ny == by) { c->path_i++; continue; }   /* reached it */
        if (nx == bx     && ny == by - 1) return 0;            /* north */
        if (nx == bx + 1 && ny == by)     return 64;           /* east  */
        if (nx == bx     && ny == by + 1) return 128;          /* south */
        if (nx == bx - 1 && ny == by)     return 192;          /* west  */
        return -1;      /* not adjacent: the car has wandered off its route */
    }
    return -1;          /* the route ran out */
}

/* Blocks of road clear ahead, up to `maxb`.
 *
 * The original scans five to ten blocks and stops at the first
 * block it may not drive into; this is the same scan over the navigation grid,
 * and the distance to the nearest car in the lane is folded in afterwards by
 * the caller so that one number answers both questions. */
static int road_clear(const gta_traffic *tr, const gta_map *m, const gta_car *c,
                      int bx, int by, int z, int angle, int maxb)
{
    int dx, dy, i;

    /* ALONG THE ROUTE WHERE THERE IS ONE, and this is not a detail.
     *
     * Scanning blindly along the heading looks straight into the building at
     * the end of the street the car is about to turn out of - so it stopped
     * dead three blocks short of every corner in the city. That one mistake
     * cost 5376 car-ticks of standing still in the drive test and pinned the
     * fleet at 29% moving. What the ladder is FOR is "how much road is there
     * in the direction I am actually going", and where there is a route, the
     * route is that direction. */
    if (c->path_i < c->path_n) {
        for (i = 0; i < maxb; i++) {
            int k = c->path_i + i;
            if (k >= c->path_n)
                return maxb;            /* route ends: it will get another */
            if (gta_nav_dirs(gta_nav_at_m(tr->nav, c->path[k].x,
                                        c->path[k].y, z)) == 0)
                return i;
        }
        return maxb;
    }

    /* AND WITHOUT ONE, ALONG THE DIRECTION THE CAR IS ABOUT TO TAKE - which is
     * the same rule, applied to the only thing the car knows when the route
     * finder has not managed to give it a path.
     *
     * This mattered far more than it looks. The route-aware branch above was
     * added when scanning blindly along the heading was found to stop cars
     * three blocks short of every corner; but it only helps a car that HAS a
     * route, and the city-wide sweep found streets where the finder fails
     * nine times in ten. At (204,84) that was 27 failed searches against 9
     * good ones, 15 of 20 cars standing still, and 2956 car-ticks held by
     * "road ahead" - every one of them a car that was about to turn, looking
     * at the building on the far side of the junction and deciding the road
     * had run out. `angle` is where the car is pointing; `want` is where it is
     * going, and this question is about where it is going. */
    /* AND IT FOLLOWS THE ROAD ROUND, instead of walking in a straight line
     * until it hits something.
     *
     * A straight scan is not "how much road is ahead of me", it is "how far
     * away is the building at the end of this street", and the difference is a
     * permanent jam. At (204,84) fifteen of twenty cars were stopped with 2956
     * car-ticks held by this rule, every one of them on a one-way block whose
     * only exit turned within three blocks: the scan saw the wall, the ladder
     * said stop, the car stopped, and the street behind it queued for the
     * whole run. It never recovered, because nothing about the car's situation
     * changed by waiting.
     *
     * So where the block ahead is not road, the walk takes one of THIS
     * block's own arrows - which is what the car will do when it gets there,
     * and what a route would have told it if the finder had managed to
     * produce one. */
    {
        int cx = bx, cy = by, dir = angle;
        static const int bit4[4]  = { GTA_NAV_N, GTA_NAV_S, GTA_NAV_W,
                                      GTA_NAV_E };
        static const int dir_of[4] = { 0, 128, 192, 64 };

        for (i = 0; i < maxb; i++) {
            int nz = nav_step_layer(m, cx, cy, z, dir);

            heading_step(dir, &dx, &dy);
            if (nz < 0) {
                unsigned char here = gta_nav_at_m(tr->nav, cx, cy, z);
                int back = (dir + 128) & 255, k, found = -1;

                for (k = 0; k < 4; k++) {
                    int nd = dir_of[k], kz;
                    if (nd == back || nd == dir) continue;
                    if ((gta_nav_dirs(here) & bit4[k]) == 0) continue;
                    kz = nav_step_layer(m, cx, cy, z, nd);
                    if (kz < 0) continue;
                    found = nd;
                    nz = kz;
                    break;
                }
                if (found < 0)
                    return i;               /* genuinely nowhere to go */
                dir = found;
                heading_step(dir, &dx, &dy);
            }
            cx += dx;
            cy += dy;
            z = nz;                         /* a ramp changes the layer */
        }
    }
    return maxb;
}

/* Distance, in 16.16 world pixels, from the car to the far edge of the block
 * it is in, along the way it is going. This is what says when a turn has to
 * start and when the block ahead has to be claimed. */
static long to_block_edge(const gta_car *c, int bx, int by)
{
    long v;

    switch (c->angle) {
    case 0:   v = c->y - (((long)by * 32) << FP);          break;
    case 128: v = ((((long)by + 1) * 32) << FP) - c->y;    break;
    case 192: v = c->x - (((long)bx * 32) << FP);          break;
    default:  v = ((((long)bx + 1) * 32) << FP) - c->x;    break;
    }
    return v < 0 ? 0 : v;
}


/* ONE TICK OF A CAR THAT HAS BEEN KNOCKED LOOSE.
 *
 * Straight semi-implicit Euler on the velocity the collision gave it, with the
 * heading carried at 16.16 so a spin does not stutter through the 256-step
 * `face`. Everything else about the car - its route, its lane, its gap, its
 * booking - is simply not consulted while this is running; `drive_one` returns
 * before any of it.
 *
 * It ends either when the timer runs out or when the car has slowed to a
 * crawl, whichever comes first, and it hands back a car that is on the rails
 * again: `face` from the heading it finished with, `speed` from the magnitude
 * of the velocity it finished with. The AI then does what it always does -
 * looks at the block under it, finds a lane, and drives - which is exactly how
 * a real driver recovers from being shunted.
 */
static void knock_step(gta_traffic *tr, gta_car *c)
{
    long sp;
    /* THE BODY TURNS ABOUT ITS CENTRE OF MASS, not about the middle of its
     * sprite. The car table carries it - `cy`, signed source pixels, halved
     * here for the world scale - and the player's vehicle has used it since
     * the physics rewrite. A traffic car spun about its drawn centre looks
     * like a sprite being twirled; about the centre of mass it looks like a
     * car being slewed, because the nose and the tail travel differently. */
    long com = ((long)tr->tiles->cars[c->model].cy << 16) / 2;
    long cmx, cmy;

    c->x += c->kvx;
    c->y += c->kvy;

    /* Where the centre of mass is now, then put the drawn centre back the
     * same distance from it once the heading has changed. */
    cmx = c->x + UMUL_S(gta_sin(c->face), com);
    cmy = c->y - UMUL_S(gta_cos(c->face), com);

    c->face16 = (c->face16 + c->komega) & 0xFFFFFFL;
    c->face = (int)((c->face16 >> 16) & 255);

    c->x = cmx - UMUL_S(gta_sin(c->face), com);
    c->y = cmy + UMUL_S(gta_cos(c->face), com);

    c->angle = c->face;

    c->kvx = (c->kvx >> 8) * GTA_KNOCK_DAMP;
    c->kvy = (c->kvy >> 8) * GTA_KNOCK_DAMP;
    c->komega = (c->komega >> 8) * GTA_KNOCK_SPIN;

    sp = (c->kvx < 0 ? -c->kvx : c->kvx) + (c->kvy < 0 ? -c->kvy : c->kvy);
    if (--c->knock <= 0 || sp < GTA_KNOCK_REST) {
        /* BACK ON THE RAILS. The heading it settled on becomes its heading;
         * the speed it settled at becomes its speed, so a car shunted forwards
         * carries on rather than starting again from a standstill. `angle` is
         * snapped to the compass because everything downstream - the lane
         * keeper, the gap, the arrows - is written in terms of the four road
         * directions. */
        c->knock = 0;
        c->kvx = c->kvy = c->komega = 0;
        c->angle = ((c->face + 32) >> 6 << 6) & 255;
        c->speed = sp > c->top ? c->top : sp;
        c->hold = GTA_HOLD_NONE;
        /* THE DRAWN HEADING IS NOT SNAPPED. `angle` is the road direction and
         * the AI needs it square, but `face` is what is drawn - walking it
         * back over the next second is the difference between a car that
         * straightens out and one that clicks straight. */
        c->recover = GTA_RECOVER_TICKS;
        tr->stat_knock_ended++;
    }
}

static void drive_one(gta_traffic *tr, const gta_map *m, int idx)
{
    gta_car *c = &tr->cars[idx];
    const gta_car_info *info = &tr->tiles->cars[c->model];
    int bx, by, want_dir, clear, nx, ny, dxs, dys;
    int jbx, jby;               /* the junction box ahead, if within reach */
    int lead_i;                 /* who gap_ahead measured to, -1 nobody */
    long ej, stopr;             /* its distance, and the stopping reach */
    /* THE CAR GIVES WAY AT ITS CORNER INSTEAD OF ABANDONING IT. See the note
     * at the refusal below. */
    int give_way = 0, exit_busy = 0;
    int ahead;                      /* the corner is a block ahead - see below */
    int move_face = -1;         /* mid-arc heading for this step, -1 = not set */
    long gap, lead, want, edge, dx, dy;

    /* NOBODY IS DRIVING THIS ONE. A car the player parked and walked away
     * from stays exactly where it was left - it is still drawn, still solid,
     * still enterable, and it never books a square or asks for a route. */
    if (c->abandoned)
        return;

    /* AND NOBODY IS DRIVING THIS ONE EITHER, YET. A car that has just been hit
     * hard is loose: it travels on the velocity the collision gave it and is
     * not steered, not held to its lane and not held off the car in front,
     * because none of those are things a driver who has just been rammed is
     * doing. See knock_step(). */
    if (c->knock > 0) {
        knock_step(tr, c);
        return;
    }

    /* STRAIGHTENING UP AFTER A KNOCK. The car drives normally again - this
     * only walks the drawn heading back towards the road's, a step a tick, so
     * the slew fades instead of vanishing between two frames. */
    if (c->recover > 0) {
        long want = (long)c->angle << 16;
        long d = (want - c->face16) & 0xFFFFFFL;

        if (d > 0x800000L) d -= 0x1000000L;      /* the short way round */
        if (d > GTA_RECOVER_STEP)  d = GTA_RECOVER_STEP;
        if (d < -GTA_RECOVER_STEP) d = -GTA_RECOVER_STEP;
        c->face16 = (c->face16 + d) & 0xFFFFFFL;
        c->face = (int)((c->face16 >> 16) & 255);
        if (--c->recover <= 0 || (d < 4096L && d > -4096L)) {
            c->recover = 0;
            c->face = c->angle;
            c->face16 = (long)c->angle << 16;
        }
    }

    if (c->done)
        return;

    bx = (int)(c->x >> (FP + 5));
    by = (int)(c->y >> (FP + 5));

    /* THE REVERSE MANOEUVRE, IF ONE IS RUNNING. It pre-empts every other rule
     * for its duration, exactly as `ped+0x47` does in the original - the
     * follow logic and the stuck detector are both suppressed while it runs.
     * Backwards along the road direction, at the original's speed of 4. */
    if (c->reverse > 0) {
        int rdx, rdy;
        long step;
        c->reverse--;
        heading_step((c->angle + 128) & 255, &rdx, &rdy);
        step = (long)GTA_SPEED_REVERSE * GTA_SPEED_UNIT / 2;
        if (back_out_clear(tr, m, idx, bx, by)) {
            c->x += (long)rdx * step;
            c->y += (long)rdy * step;
        } else {
            c->reverse = 0;         /* something arrived behind us: stop */
        }
        c->speed = 0;
        c->hold = GTA_HOLD_STUCK;
        c->wait = 0;
        return;
    }

    /* PROGRESS IS A BLOCK, NOT A PIXEL, and this has to run before anything
     * can return early. `wait` used to be cleared whenever the car moved at
     * all, so a creeping queue reset it constantly and the three-second
     * override that lets a car push into a junction which will not clear never
     * fired: at (204,108) cars stood for 209 seconds with the escape hatch
     * right beside them, resetting itself every few ticks.
     *
     * And half a minute without covering one block means the car is not
     * queueing but trapped - see GTA_TRAFFIC_ABANDON. */
    if (bx != c->last_bx || by != c->last_by) {
        c->last_bx = bx;
        c->last_by = by;
        c->wait = 0;
    } else if (c->wait > GTA_TRAFFIC_ABANDON && in_view(tr, bx, by) &&
               c->reverse == 0 && back_out_clear(tr, m, idx, bx, by)) {
        /* WEDGED, AND THE PLAYER IS WATCHING: BACK OUT.
         *
         * The original's stuck handler has no destructor in it at all. State 1
         * checks the five blocks BEHIND the car - each must be road and free
         * of other cars - plus the diagonal block the tail sweeps into, and
         * then commits: speed -4, turn request -0x20, and on to state 2.
         *
         * It runs the reverse for at most nine frames, stops for five, then
         * probes the block ahead and pulls out. If that never comes free it
         * waits at that state indefinitely - it never gives up on the car.
         *
         * WHY IT HAD TO COME BACK. Gating the delete on visibility (below)
         * left the port with no way out of a gridlock ring at all, and the
         * ring is absorbing: measured at (204,108), flow fell to 26% with one
         * car standing 200 seconds, and the outcome was bimodal on the seed -
         * 5% on one, 79% on another - which is the signature of a lock that
         * forms or does not and never clears once it has. Deleting a car was
         * breaking it, in full view of the player, which is the fault that was
         * reported. This is the original's answer instead. */
        c->reverse = GTA_TRAFFIC_REVERSE;
        c->wait = 0;
        c->turn = 0;
        c->hold = GTA_HOLD_STUCK;
        return;
    } else if (c->wait > GTA_TRAFFIC_ABANDON && !in_view(tr, bx, by)) {
        /* AND NOT WHERE ANYBODY CAN SEE IT. That clause is the whole of the
         * developer's "cars vanish" report, and the original is unambiguous
         * about it: its stuck handler contains no destructor at all - it
         * reverses, or turns the car round on the spot, resets after fifty
         * frames and tries again, indefinitely. A route-less car that is ON
         * SCREEN is braked to a stop and left standing. The only way a traffic
         * car leaves the world other than being wrecked is the slot recycler,
         * which refuses any car that is on screen - and even that RE-USES the
         * object at a new spawn point rather than destroying it.
         *
         * So this keeps the backstop - a car wedged for thirty seconds is not
         * coming back and holding its slot starves the spawner - but it can no
         * longer take one out of the picture in front of the player. In view,
         * the car simply keeps waiting, which is what the original does. */
        tr->stat_abandoned++;
        tr->abandon_x = bx;
        tr->abandon_y = by;
        tr->abandon_z = c->layer;
        c->done = 1;
        return;
    }

    /* --- 0. the layer the car is actually on -------------------------------
     *
     * A ramp takes a car up or down (see nav_step_layer), and the layer is
     * stored rather than derived, so it has to follow. Doing it here - from
     * the block the car has ARRIVED in - rather than at the moment of stepping
     * means it is right however the car got there, including after a turn on a
     * ramp block or a lane correction across a boundary.
     *
     * Down is tried before up because a car that has just left a viaduct is on
     * the street, not on the roof of the building beside it. */
    if (!drivable(m, bx, by, c->layer)) {
        if (c->layer > 0 && drivable(m, bx, by, c->layer - 1))
            c->layer--;
        else if (c->layer + 1 < GTA_MAP_LAYERS &&
                 drivable(m, bx, by, c->layer + 1))
            c->layer++;
    }

    /* --- CROSSING A JUNCTION, MEASURED END TO END -------------------------
     *
     * Entering, crossing, leaving. See gta_car.in_cross: this asks whether the
     * car leaves a crossing on the SAME LINE it arrived on, which is the
     * question the whole cornering complaint is about and which no earlier
     * instrument here asked. */
    {
        int on_j = is_junction(m, bx, by, c->layer);
        long here = ((c->angle & 127) == 64) ? c->y : c->x;

        /* IS THE LINE ON THE STREET THE SAME AS THE LINE ON THE CROSSING?
         *
         * See stat_line_street_sum. Everything else in this file compares a
         * car with ITSELF at two points inside a junction, which cannot see a
         * difference between the open street and the box. This does: the
         * lateral offset within the block, every tick, for every car that is
         * driving straight, bucketed by whether it is on a junction block.
         *
         * Offsets are folded onto one side so the two carriageways of a dual
         * road do not cancel: what matters is the distance from the block
         * centre, not which way. A car mid-turn is skipped - its offset is
         * meaningless while it is describing an arc. */
        if (c->turn == 0 && c->speed > 0) {
            int off = (int)((here >> FP) & 31) - GTA_LANE_TARGET;

            /* AND IT IS THE CHANGE PER CAR, SIGNED, NOT TWO AVERAGES.
             *
             * The first version of this summed |offset| on street blocks and on
             * junction blocks and compared the two means. It reported 2.8 px
             * against 2.6 px - no jump - and it was wrong BY CONSTRUCTION:
             * folding to an absolute value makes a car that sits at +4 on the
             * street and -4 in the box look like it never moved, when that is
             * an EIGHT pixel jump and exactly the thing being reported. The
             * two carriageways of a dual road have opposite kerb sides, so the
             * signed offsets cannot simply be averaged across the fleet
             * either - they cancel.
             *
             * So the measurement is per car: remember the signed offset and
             * whether the block was a junction, and when that changes, record
             * how far the car's line moved. */
            /* AND WHAT IS MEASURED IS THE DISTANCE FROM THE CAR'S OWN INTENDED
             * LINE, not the change in its position.
             *
             * The second version of this recorded the position delta at the
             * tick the car crossed a block boundary, and that is zero BY
             * CONSTRUCTION: a car's position is continuous, it cannot jump. It
             * duly reported 0.1 to 1.1 px and meant nothing. What jumps is the
             * TARGET; the car then drifts towards it over several ticks, two
             * pixels at a time, and that drift is what is visible from the
             * pavement.
             *
             * So: how far is the car from `lane_target` - the line it is
             * actually trying to hold - summed separately over its ticks on
             * ordinary road and its ticks on a junction block. Two different
             * answers mean the car is holding a different line in the box from
             * the one it holds on the street, which is the report. */
            {
                int e = off + GTA_LANE_TARGET - c->lane_target;
                if (e < 0) e = -e;
                if (on_j) { tr->stat_line_cross_sum += e; tr->stat_line_cross_n++; }
                else      { tr->stat_line_street_sum += e; tr->stat_line_street_n++; }
            }
        }

        /* A CAR INSIDE A CROSSING KEEPS ITS CLAIM ALIVE. Without this the
         * claim expires under the car that is still using the box and the next
         * forcer walks straight into it, which is the deadlock again with an
         * extra three seconds in front of it. Renewing costs one table scan per
         * junction block driven; the claim is dropped by simply not renewing
         * it once the car is back on ordinary road. */
        /* AND ONLY WHILE IT IS STILL MOVING. A stopped car inside the box is
         * the one case where holding the crossing is worse than giving it up:
         * renewing regardless let a car that had wedged itself keep every
         * other arm out for ever, which is the deadlock again with one car in
         * it instead of four. Stop renewing and the claim ages out in three
         * seconds, and somebody else is allowed to try. */
        /* A CAR ON AN ARC KEEPS THE BOX WHETHER IT IS MOVING OR NOT.
         *
         * The rule below - stop renewing once stopped, so a wedged car ages
         * out of its claim in three seconds and somebody else may try - is
         * right for a car that is driving straight through and wrong for one
         * half way round a corner, which cannot leave and cannot be gone
         * round. Measured at (204,108): 3928 of 7252 shared-box car-ticks had
         * NOBODY owning the crossing, with 6354 of them stopped. */
        /* NO RENEWAL AND NO TIMER. A booking lives until the owner's body
         * has covered each square and left it - the sweep in
         * gta_traffic_tick(). A standing car keeps what it booked; a square
         * behind a moving one frees itself, length-aware. */

        if (on_j && !c->in_cross) {
            c->in_cross     = 1;
            c->cross_ang    = c->angle;
            c->cross_pos    = here;
            c->cross_worst  = 0;
            c->cross_turns  = 0;
            c->cross_routed = (c->path_i < c->path_n);
        } else if (on_j && c->in_cross) {
            /* Still inside. Only a car going STRAIGHT has a line to keep - a
             * turning one is meant to leave it - so only that is measured. */
            if (c->angle == c->cross_ang) {
                long d = here - c->cross_pos;
                if (d < 0) d = -d;
                if (d > c->cross_worst) c->cross_worst = d;
            }
        } else if (!on_j && c->in_cross) {
            int px_off, b;
            c->in_cross = 0;
            /* Squares free themselves one by one as the body leaves them -
             * the tick sweep. A bulk release here dropped a long vehicle's
             * squares while its tail was still on them. */

            if (c->angle == c->cross_ang) {
                /* STRAIGHT THROUGH: the line must not have moved at all. */
                long d = here - c->cross_pos;
                if (d < 0) d = -d;
                px_off = (int)(d >> FP);
                tr->stat_cross_virt += c->cross_worst >> FP;
                tr->stat_cross_virt_n++;
                b = (px_off <= 1) ? 0 : (px_off <= 3) ? 1 : (px_off <= 7) ? 2 : 3;
                tr->stat_cross_straight[b]++;
                /* A whole lane across while still pointing the same way is the
                 * "it changes lane in the middle of the crossing for no
                 * reason" report. It has two possible causes and they need
                 * different fixes: the driver STEERED across (two opposite
                 * turns, which turn_lock deliberately permits as a lane
                 * change), or the ROUTE stepped sideways through the junction
                 * and the car obediently followed it. */
                if (b == 3) {
                    if (c->cross_turns >= 2) tr->stat_lane_change_steered++;
                    if (c->cross_routed)     tr->stat_lane_change_routed++;
                }
            } else {
                /* TURNED: the axis has changed, so what is judged is how far
                 * the car sits from the centre of the lane it has joined. */
                int off = (int)((here >> FP) & 31);
                int want = lane_target_at(m, bx, by, c->layer, c->angle);
                int e = off - want;
                if (e < 0) e = -e;
                b = (e <= 1) ? 0 : (e <= 3) ? 1 : (e <= 7) ? 2 : 3;
                tr->stat_cross_turned[b]++;
            }
        }
    }

    /* ONE TURN PER CROSSING. `turn_lock` holds the direction of the turn the
     * car has just made and is released as soon as it is on a block that is
     * not part of a junction - which is what stops the second ninety-degree
     * turn that together with the first makes a U-turn. See the field. */
    if (c->turn == 0 && !is_junction(m, bx, by, c->layer))
        c->turn_lock = 0;
    /* Out the far side: the crossing decision is re-opened the moment the
     * middle is off the box. It used to wait until the NEXT block was clear
     * of junction too, so that complexes were sailed through in one go - but
     * that let a car enter the second box of a complex with no gate and NO
     * BOOKING (measured: thousands of shared-box ticks "with no claim at
     * all"). Each box is asked for, and booked, on its own now. */
    /* THE COMMITMENT IS PRESENCE, NOT A POSITION TEST ON THE APPROACH -
     * developer's design, 2026-08-25. A committed car stays committed
     * while it stands on the square it booked FROM or on one of its own
     * booked junction squares; the moment it is on neither (it landed
     * beyond, or it wandered off without entering) the commitment ends
     * and every square its body never reached is given back at once.
     * The old test ("middle not on a junction square") was true on EVERY
     * approach tick, so it silently dropped and re-made the booking each
     * tick all the way to the line - and whenever a rival booked into
     * that gap, the car stood at an empty crossing waiting for a claim
     * it no longer held. */
    if (c->crossing) {
        int held = (bx == c->book_ax && by == c->book_ay);

        if (!held && is_junction(m, bx, by, c->layer)) {
            int gi;
            for (gi = 0; gi < tr->claim_top; gi++)
                if (tr->claim_ttl[gi] > 0 &&
                    tr->claim_car[gi] == c->convoy &&
                    (int)tr->claim_x[gi] == bx &&
                    (int)tr->claim_y[gi] == by) {
                    held = 1;
                    break;
                }
        }
        /* A booking made from braking distance crosses ordinary road on
         * the way to the line. That road square is neither the start
         * square nor a booked one - so the ANCHOR SLIDES with the car:
         * still short of the box, own claims still standing, commitment
         * unbroken. Without this the commitment broke for one square and
         * the old every-tick race came back through the crack. */
        if (!held && !is_junction(m, bx, by, c->layer) &&
            (is_junction(m, bx + dxs, by + dys, c->layer) ||
             is_junction(m, bx + 2 * dxs, by + 2 * dys, c->layer))) {
            int gi;
            for (gi = 0; gi < tr->claim_top; gi++)
                if (tr->claim_ttl[gi] > 0 &&
                    tr->claim_car[gi] == c->convoy) {
                    c->book_ax = bx;
                    c->book_ay = by;
                    held = 1;
                    break;
                }
        }
        if (!held) {
            /* ...but a TAIL whose route others still ride releases NOTHING
             * on its way out - the unseen squares ahead are under the
             * members' wheels. The claims stay under this serial; the
             * countdown below is member-aware too, so they die only after
             * the last rider is out. */
            if (!convoy_riding(tr, c->serial)) {
                int gi;
                for (gi = 0; gi < tr->claim_top; gi++)
                    if (tr->claim_ttl[gi] > 0 &&
                        tr->claim_car[gi] == c->serial &&
                        !tr->claim_seen[gi])
                        tr->claim_ttl[gi] = 0;
            }
            c->crossing = 0;
        }
    }

    /* THE ONE BIT - see gta_car.allow_turn.
     *
     * It is cleared when a turn is COMMITTED, not when the car enters a
     * junction, and it comes back the moment the car is on a block that is not
     * part of one. That is still exactly one turn per crossing, and it is the
     * version that works.
     *
     * Clearing it on ENTRY, which is the obvious reading, throws the turn away
     * entirely wherever the route asks for it on the junction block itself -
     * and our route nodes are one block apart, so that is most of them.
     * Measured: turns missed 16 -> 252 at (64,64) and 3 -> 439 at (64,56),
     * left turns 228 -> 143, and 8708 turns refused at one site. The developer
     * saw it as "cars have stopped turning left altogether". */
    /* THE BIT COMES BACK on ordinary road, or once the car has put a block and
     * a half between itself and its last corner - see GTA_TURN_AGAIN. The
     * distance is THIS car's own odometer; the first version used the fleet's
     * total, which twenty cars cover in two ticks, so the gate was open again
     * before the car had moved and the lane changes came straight back (239
     * over a city sweep against 83). */
    /* STRICT AGAIN: the bit comes back only on a block that is not part of a
     * junction. The odometer escape (a second corner allowed after a block and
     * a half of travel, GTA_TURN_AGAIN) was added to recover turns the strict
     * rule refuses on wide crossings, and it re-opened the fault it exists to
     * prevent - the developer saw cars turning twice in one box again. One
     * turn per crossing means one. */
    if (!is_junction(m, bx, by, c->layer))
        c->allow_turn = 1;

    /* THE BLOCK THE CAR TURNED OUT OF - see gta_car.lane_bx.
     *
     * Updated every tick the car is NOT turning, so while an arc is running it
     * holds the block the car was standing on the moment before the corner
     * began. That is the lane a player reads the arrows off, and it is the
     * only definition that survived measurement:
     *
     *   - the block the car is on WHEN THE ARC RUNS is usually the junction
     *     itself, which carries every direction and has no opinion. The rule
     *     and its instrument both read "legal" while the screen showed a left
     *     turn out of a right-only lane;
     *   - the last block that is not `is_junction()` is far too strict,
     *     because is_junction() means "has both axes" and that is true of any
     *     lane block permitting a turn. Every corner at (61,48) was refused:
     *     107 turns became 0. */
    if (c->turn == 0) {
        gta_block ab;
        int ad;
        c->lane_bx = bx;
        c->lane_by = by;
        /* and the last PLAIN LANE block - exactly one direction bit */
        if (gta_map_block(m, bx, by, c->layer, &ab)) {
            ad = gta_block_dirs(&ab);
            if (ad && (ad & (ad - 1)) == 0) {
                c->appr_bx = bx;
                c->appr_by = by;
            }
        }
    }

    /* --- 1. the turn in progress ------------------------------------------
     *
     * THE CAR IS ON AN ARC OF A KNOWN RADIUS and it turns at speed / radius, so
     * the corner it describes is the corner the geometry asked for however
     * fast it happens to be going. See GTA_TURN_RADIUS in the header for why
     * that is the fix rather than a refinement: with a fixed rate the radius
     * was speed / rate, a bus took a five-pixel corner and a saloon a
     * thirteen-pixel one, and neither of them came out on its own lane.
     *
     * The heading used for the MOVE is the mid-point of this tick's arc, not
     * its start - the difference is a fifth of a pixel a tick and it is free,
     * where integrating from the start of each step quietly shrinks the whole
     * radius by half a step's worth. */
    /* THE ARROWS, CHECKED WHERE THE CAR PHYSICALLY IS, ON THE FIRST TICK OF
     * THE ARC. THIS IS THE ONE THAT CANNOT BE BYPASSED.
     *
     * "z prawej alejki nie moze skrecac w lewo"
     *
     * The same test at the moment the turn is COMMITTED has been wrong twice,
     * both times because the commit does not happen where the corner does: the
     * route can set a corner up a block early, and then the block the rule
     * asked and the block the car actually turns out of are different ones.
     * Measured with an instrument that shares no code with the rule
     * (`arrowwatch`): 174 turns out of a forbidden block still got through.
     *
     * Here there is no gap between the question and the fact. The car is
     * standing on block (bx,by); if that block does not carry the direction it
     * would finish in, the corner is cancelled and the car carries straight
     * on. That block is the LANE, and the arrows over a lane are exactly this
     * bit.
     *
     * IT RUNS EVERY TICK OF THE ARC, not only the first: a car that commits
     * right on a block boundary is measured on one block and rotates on the
     * next, and checking once let 187 turns through.
     *
     * AND IT DOES NOT EXEMPT "JUNCTIONS", which is what let the last 186
     * through. `is_junction()` means the block carries both a north/south and
     * an east/west bit - and a block marked S|W is a bend, not a crossroads.
     * Exempting those exempted exactly the blocks where the illegal turns
     * happen. A real crossroads carries all four bits and passes this test on
     * its own; nothing needs to be excused from it. */
    if (c->turn != 0 && tr->opt_arrows) {
        int endang = (c->turn_from + (c->turn > 0 ? GTA_TURN_QUARTER
                                                  : -GTA_TURN_QUARTER)) & 255;
        int abx = c->lane_bx, aby = c->lane_by;
        int sdx, sdy;
        int side = (c->turn_from + (c->turn > 0 ? GTA_TURN_QUARTER
                                                : -GTA_TURN_QUARTER)) & 255;
        (void)endang;
#ifdef GTA_ARROW_PROBE
        if (c->arc_s == 0) {
            gta_block pb;
            int pm = gta_map_block(m, bx, by, c->layer, &pb)
                     ? gta_block_dirs(&pb) : -1;
            int pn = g_nav && g_nav->b
                     ? gta_nav_dirs(gta_nav_at_m(g_nav, bx, by, c->layer)) : -1;
            fprintf(stderr, "PROBE car#%lu block(%d,%d) L%d endang %d "
                    "map-dirs %d nav-dirs %d allowed %d\n",
                    c->serial, bx, by, c->layer, endang, pm, pn,
                    dir_allowed(m, bx, by, c->layer, endang));
        }
#endif
        (void)abx; (void)aby; (void)sdx; (void)sdy; (void)side;
    }

    if (c->turn != 0) {
        /* THE CAR IS PLACED ON ITS ARC. It is not steered along one.
         *
         * arc_s is how far round it has travelled; everything else - position
         * and heading both - is read off the circle from that one number. A
         * car that brakes mid-corner because of the vehicle in front simply
         * gets a smaller arc_s this tick and stays on the same circle, where
         * with a turn RATE it drew a circle of radius speed/rate and came out
         * of the corner beside its lane. That was the fault reported over and
         * over as "sometimes it takes the right line and sometimes it does
         * not": it took the right line whenever it happened not to brake.
         *
         * The landing is exact by construction, not by luck: the circle is
         * tangent to the entry lane line where it starts and to the exit lane
         * line where it ends, so a completed arc puts the car ON the line it
         * was aiming at. There is nothing left for the lane keeper to drag
         * sideways afterwards. */
        long phi, a_fp, rd_fp;
        long arc_s_was = c->arc_s;

        c->turn_ticks++;
        c->arc_s += c->speed;
        if (c->arc_s > c->arc_len) c->arc_s = c->arc_len;

        phi = ((c->arc_s / c->turn_radius) * GTA_ARC_K) >> GTA_ARC_KSHIFT;
        if (phi > ((long)GTA_TURN_QUARTER << FP))
            phi = (long)GTA_TURN_QUARTER << FP;

        /* The heading is the tangent, and the radius vector is a quarter turn
         * behind it - to the LEFT of a right-hand corner, which is where the
         * centre was put when the arc was committed. */
        a_fp  = (((long)c->turn_from) << FP) + (long)c->turn * phi;
        rd_fp = a_fp - (long)c->turn * ((long)GTA_TURN_QUARTER << FP);

        {
            int  nface = (int)((a_fp >> FP) & 255);
            long nx_ = c->arc_cx + (((long)c->turn_radius * sin_fp(rd_fp)) << 2);
            long ny_ = c->arc_cy - (((long)c->turn_radius * cos_fp(rd_fp)) << 2);

            /* THE BACKSTOP - see car_free_at(). A corner that would put this
             * car inside another one simply does not advance this tick: arc_s
             * is wound back, so the car stays exactly where it was, on its
             * circle, and tries again next tick. Nothing about the arc is
             * lost - it is a path, and the car is still on it. */
            if (c->arc_s > 0 && tr->opt_nooverlap &&
                car_free_at(tr, idx, c->x, c->y, c->face) &&
                !car_free_at(tr, idx, nx_, ny_, nface)) {
                c->arc_s = arc_s_was;
                c->wait++;
                tr->stat_blocked_move++;
                if (c->hold == GTA_HOLD_NONE) c->hold = GTA_HOLD_GAP;
            } else {
                c->face = nface;
                c->x = nx_;
                c->y = ny_;
            }
        }

        c->turn_accum = (int)(phi >> FP);
        move_face = -2;         /* the arc has already placed the car */

        /* No re-booking mid-turn: the route booking took the turn's squares
         * before entry, and each frees itself as the body leaves it. The old
         * release-and-rebook here dropped the whole booking - landing square
         * included - part way round the corner. */

        if (c->arc_s >= c->arc_len && c->arc_s != arc_s_was) {
            /* LAND IT ON THE LINE, EXACTLY. The arc already ends within a
             * fraction of a pixel of it; snapping costs nothing and makes the
             * one thing the whole corner exists for true to the bit. */
            c->face = (c->turn_from + ((c->turn > 0) ? GTA_TURN_QUARTER
                                                     : -GTA_TURN_QUARTER)) & 255;
            if ((c->face & 127) == 64) c->y = c->arc_line;
            else                       c->x = c->arc_line;
            c->turn_accum = GTA_TURN_QUARTER;

            /* AND THE LANE KEEPER IS TOLD WHERE THE CORNER PUT THE CAR.
             *
             * `lane_target` is a within-block offset for the direction the car
             * was travelling in, and it is only refreshed on a block that is
             * not part of a junction - so a car that lands INSIDE a crossing
             * carries the offset for the lane it has just left, on an axis it
             * no longer travels along, and the keeper immediately drags it off
             * the line the arc just placed it on. Measured: the landing itself
             * was exact to the pixel and 54% of corners still read 2-3 px out
             * by the time the car left the crossing. The two mechanisms have
             * to agree, and the corner is the one that knows. */
            c->lane_target = c->turn_aim_tgt;


        }

        if (c->turn_accum >= GTA_TURN_QUARTER &&
            ((c->face + 32) & 0xC0) != c->turn_from) {
            c->face = (c->turn_from + ((c->turn > 0) ? 64 : -64)) & 255;
            c->turn = 0;
            c->turn_accum = 0;
            c->since_turn = 0;      /* the tests time the slide from here */

            /* AND HOW MANY TICKS THE CORNER TOOK. The developer says the
             * original turns more gradually than this does; that is a number,
             * so it gets measured rather than argued about. */
            {
                int t = c->turn_ticks;
                int tb = (t <= 4) ? 0 : (t <= 8) ? 1 : (t <= 16) ? 2
                       : (t <= 32) ? 3 : 4;
                tr->stat_turn_ticks[tb]++;
                tr->stat_turn_ticks_n++;
                tr->stat_turn_ticks_sum += t;
            }

            /* WHERE DID IT ACTUALLY LAND? Measured here, once, while both
             * numbers are still available - see gta_traffic.stat_landings.
             * `newang` is the road direction the car has just come round to;
             * its cross axis is the one the lane lives on. */
            {
                int newang = (c->face + 32) & 0xC0;
                int lx = (int)(c->x >> (FP + 5)), ly = (int)(c->y >> (FP + 5));
                long pos = ((newang & 127) == 64) ? c->y : c->x;
                int off  = (int)((pos >> FP) & 31);
                int want = lane_target_at(m, lx, ly, c->layer, newang);
                int eg = off - c->turn_aim_tgt;      /* missed its own aim */
                int ea = c->turn_aim_tgt - want;     /* aimed at the wrong line */

                tr->stat_landings++;
                tr->stat_land_geom += (eg < 0) ? -eg : eg;
                tr->stat_land_aim  += (ea < 0) ? -ea : ea;

                /* AND THE SAME SPLIT BY TURN DIRECTION - see the counters in
                 * gta_traffic.h. This is the measurement the left-turn lead
                 * asked for and it has to come before any change to the aim. */
                {
                    int w = (((newang - c->turn_from) & 255) == 64) ? 0 : 1;
                    tr->stat_land_n_dir[w]++;
                    tr->stat_land_geom_dir[w] += (eg < 0) ? -eg : eg;
                    {
                        int s = c->turn_step;
                        if (s < 0) s = 0;
                        if (s > 4) s = 4;
                        tr->stat_geom_by_step[s] += (eg < 0) ? -eg : eg;
                        tr->stat_geom_by_step_n[s]++;
                    }
                    tr->stat_land_aim_dir[w]  += (ea < 0) ? -ea : ea;
                }

                /* AND THE SLIDE IS ARMED HERE. See stat_slide: this records
                 * where the car IS at the end of the arc, so that the same
                 * coordinate can be read again once the lane keeper has had
                 * its say and the difference is the sideways move the
                 * developer actually sees. */
                c->slide_pos  = pos;
                c->slide_serial = c->serial;
                c->slide_ang  = newang;
                c->slide_dir  = (((newang - c->turn_from) & 255) == 64)
                                ? 0 : 1;    /* 0 right, 1 left */
                c->slide_left = GTA_AFTER_TURN;
            }
        }
    }

    /* HOW FAR DID IT SLIDE AFTER THE CORNER? - the developer's report, as a
     * distance rather than as a sum.
     *
     * `stat_land_*` measures the moment the arc ends and says the landing is
     * about two pixels out; `stat_lane_fix_corner` sums the correction over
     * the whole city. Neither answers "does a car come off a corner and then
     * visibly move sideways", which is what is being reported, because a sum
     * hides how many corners it is spread over and a landing error is not yet
     * a movement.
     *
     * So the cross-axis coordinate is remembered at the end of the arc and
     * read again GTA_AFTER_TURN ticks later, by which time the lane keeper has
     * finished. The difference is the slide, in world pixels, bucketed the
     * same way as everything else: 0-1 is invisible, 2-3 is the edge of
     * noticeable, 4+ is a car changing its line in front of you. */
    if (c->slide_left > 0) {
        c->slide_left--;
        if (c->slide_left == 0 && c->slide_serial == c->serial) {
            long now = ((c->slide_ang & 127) == 64) ? c->y : c->x;
            long d = now - c->slide_pos;
            int  px, b;
            if (d < 0) d = -d;
            px = (int)(d >> FP);
            b = (px <= 1) ? 0 : (px <= 3) ? 1 : (px <= 7) ? 2 : 3;
            tr->stat_slide[b]++;
            tr->stat_slide_dir[c->slide_dir & 1][b]++;

            /* WHERE THE CAR HAS ACTUALLY SETTLED, and this is the honest
             * version of the aim error.
             *
             * `stat_land_aim` compares the target the turn aimed at with the
             * target read at the tick the arc ends - and since lane_target_at()
             * learned to walk along the street, BOTH SIDES OF THAT SUBTRACTION
             * come from the same walk whenever the car is still on a junction
             * block. It duly reported 0.0 px for left turns, which is not a
             * result, it is the same number minus itself. Right turns, which
             * finish clear of the box, were compared against the street's real
             * answer and read 1.4 px. That difference was an artefact of WHERE
             * each turn finishes, not of how well it was aimed.
             *
             * Forty ticks later the car is on ordinary road whichever way it
             * turned, so the block under it answers for itself and the walk
             * does not run. This is therefore the one measurement of "is the
             * car on its lane after a corner" that cannot be contaminated -
             * and it is also exactly what the developer is looking at. */
            {
                int sx = (int)(c->x >> (FP + 5)), sy = (int)(c->y >> (FP + 5));
                int off = (int)((now >> FP) & 31);
                int want = lane_target_at(m, sx, sy, c->layer, c->slide_ang);
                int e = off - want;
                int sb;
                if (e < 0) e = -e;
                sb = (e <= 1) ? 0 : (e <= 3) ? 1 : (e <= 7) ? 2 : 3;
                tr->stat_settled[c->slide_dir & 1][sb]++;

                /* AND WHERE THE TURN AIMED, against that same settled lane,
                 * WITH ITS SIGN. See stat_aim_bias: an unsigned 1.4 px could be
                 * noise either way, a signed sum that sits near a whole
                 * GTA_LANE_KERB is an offset going the wrong way. */
                {
                    int w = c->slide_dir & 1;
                    int bias = c->turn_aim_tgt - want;
                    tr->stat_aim_bias[w] += bias;
                    tr->stat_aim_bias_n[w]++;
                    if (bias < 0)      tr->stat_aim_sign[w][0]++;
                    else if (bias > 0) tr->stat_aim_sign[w][1]++;
                }
            }
            tr->stat_slide_px += px;
        }
    }

    if (c->since_turn < GTA_AFTER_TURN)
        c->since_turn++;

    /* THE ROAD DIRECTION IS THE HEADING'S QUADRANT. The original recomputes
     * its direction field from the car's rotation every frame and
     * everything else reads that field; this is the same, and it is why the
     * following, the junction claims and the lights did not have to change
     * when the steering did. */
    c->angle = ((c->face + 32) & 0xC0);
    heading_step(c->angle, &dxs, &dys);

    /* WHERE THIS CAR'S LANE SITS INSIDE ITS BLOCK - latched, and only ever
     * updated out on the open road. See gta_car.lane_target: sampling the
     * blocks either side is right on a street and meaningless inside a
     * crossing, where there is road on all four sides, so the value the car
     * carried in is the one it keeps until it is out the other side.
     *
     * A car is nudged away from the kerb only when there is a kerb on exactly
     * one side. Between two lanes - both sides road - the block centre is the
     * lane centre and nothing is added. */
    if (!is_junction(m, bx, by, c->layer))
        c->lane_target = lane_target_at(m, bx, by, c->layer, c->angle);


    /* --- 2. where the route says to go ------------------------------------ */
    /* DID THE LAST BLOCK'S TURN GET TAKEN? Counted here, at the top, because
     * this is the one place that knows both the block the car is in NOW and
     * the turn it was asked for while it was in the previous one. See
     * gta_traffic.stat_turn_missed. */
    if (c->turn_want != 0 && (bx != c->turn_want_bx || by != c->turn_want_by)) {
        if (c->turn == 0 && c->angle == c->turn_want_from) {
            tr->stat_turn_missed++;
            tr->stat_turn_missed_kind[c->turn_want_routed ? 1 : 0]++;
        }
        c->turn_want = 0;
    }

    want_dir = route_dir(c, bx, by);
    /* THE BOOKED SHAPE IS BINDING. While the car is committed to a
     * crossing it bends ONLY on the square it booked the bend on, in the
     * booked direction, and drives straight everywhere else on or before
     * the box - whatever the route says. This is what makes the booked
     * squares and the driven squares the same thing, which is the whole
     * of the developer's reservation design; a route the shape diverged
     * from is simply re-planned on the far side. */
    if (c->crossing) {
        if (c->bend_dir != 255 && bx == c->bend_bx && by == c->bend_by)
            want_dir = c->bend_dir;
        else
            want_dir = c->angle;
    }

    /* ASK FOR THE NEXT ROUTE BEFORE THIS ONE RUNS OUT. See GTA_ROUTE_REFILL:
     * one search a tick for the whole fleet means a request can wait twenty
     * ticks, which is a block of driving, and a car that asks only once it is
     * already routeless gets its answer after the junction it needed it for. */
    if (c->path_n - c->path_i <= GTA_ROUTE_REFILL)
        c->want_route = 1;

    /* A ROUTE NEVER ASKS FOR A U-TURN. The search refuses to leave the start
     * block backwards (gta_route.h), but a car can still drift a block off its
     * path - and then the next node is behind it and the driver, obediently,
     * spins round in the road. That is the "it turns left onto the main road
     * and immediately turns left back into the side street" the developer
     * photographed. The route is thrown away instead; the car keeps following
     * the arrows and is given another one within a tick or two. */
    if (want_dir >= 0 && want_dir == ((c->angle + 128) & 255)) {
        tr->stat_drop_uturn++;
        tr->stat_drop_nodes += c->path_n - c->path_i;
        c->path_n = 0;
        c->path_i = 0;
        want_dir = -1;
    }

    if (want_dir >= 0) {
        /* THE ROUTE DOES NOT KNOW HOW LONG THE VEHICLE IS. It is found on the
         * navigation grid, which is one byte a block and says nothing about a
         * bus needing four of them in a line. So a step the vehicle cannot lie
         * in throws the route away and falls through to the arrow-following
         * below, which does check - otherwise buses end up wedged across the
         * pavement in streets the grid was perfectly happy with.
         *
         * STRAIGHT ON IS CHECKED TOO, not only the turns: the first version
         * tested turns alone and a bus drove itself straight into the dead end
         * of a one-block street, ending up with its back half inside a
         * building. */
        int tdx, tdy, axis;
        heading_step(want_dir, &tdx, &tdy);
        axis = ((want_dir & 127) == 64) ? 64 : 0;
        if (!fits(m, bx, by, c->layer, axis, gta_car_world_len(info)) ||
            !fits(m, bx + tdx, by + tdy, c->layer, axis, gta_car_world_len(info))) {
            tr->stat_drop_fits++;
            tr->stat_drop_nodes += c->path_n - c->path_i;
            c->path_n = 0;
            c->path_i = 0;
            want_dir = -1;
        }
    }
    if (want_dir < 0) {
        /* No usable route. Ask for one - the fleet gets a search a tick - and
         * meanwhile carry on the way the map allows.
         *
         * THIS IS WHERE THE JUNCTION WOBBLE COMES FROM, and it is not yet
         * fixed - three attempts are recorded in the notes (2026-08-23,
         * "the wobble is load-bearing"). choose_heading() is partly random and
         * is called every tick, so a car with no route re-decides its heading
         * dozens of times while crossing one junction and can turn one way
         * then back - the "cars leave their lane at the junction and return"
         * the developer keeps reporting, proved tick-by-tick with
         * `gtadump turntrace`. But stabilising the choice (commit per block;
         * commit while moving; drive straight for a cooldown after a turn)
         * crashed throughput to 23-63% every time, because the fleet relies on
         * this re-rolling to spread out and escape jams. The wobble and the
         * flow are the same mechanism. The real fix is upstream - give more
         * cars a working ROUTE so fewer ever reach this fallback - not another
         * patch here. */
        c->want_route = 1;
        tr->stat_no_route++;
        /* A route that still has nodes but cannot be followed: the car has
         * drifted off its path, so route_dir() found the next node is not
         * adjacent. The route is not thrown away here - it is simply unusable
         * this tick - but the car is on the fallback all the same. */
        if (c->path_i < c->path_n)
            tr->stat_drop_stray++;
        want_dir = choose_heading(m, bx, by, c->layer, c->angle,
                                  gta_car_world_len(info), tr);
    }

    if (want_dir < 0) {
        /* Nowhere legal at all: the dead end. Never delete the car in view -
         * see GTA_TRAFFIC_GIVEUP for the one exception. */
        c->speed = 0;
        c->hold = GTA_HOLD_DEADEND;
        c->wait++;
        if (c->wait > GTA_TRAFFIC_GIVEUP)
            c->done = 1;
        return;
    }
    /* `wait` counts TICKS NOT MOVING, whatever the reason, and it is cleared
     * at the bottom of this function when the car actually moves. It used to
     * be cleared here, which quietly disabled both places that read it. */

    /* --- 3. start a turn --------------------------------------------------
     *
     * When the crossing of the two lane lines is exactly one turning radius
     * away, and the lane being turned into has room. There is no "half a
     * block" in it any more: the trigger IS the radius, which is what makes
     * the arc tangent to both lanes and puts the car down on the new lane line
     * instead of beside it. */
    edge = to_block_edge(c, bx, by);

    /* SET THE CORNER UP A BLOCK EARLY WHEN THE ROUTE ALREADY KNOWS ABOUT IT.
     *
     * See the note further down beside `line`: with the turn decided on the
     * block the car is standing in, the whole approach is one block, and
     * measurement says the arc gets 12.5 px of a 29 px ceiling with NOT ONE
     * TURN IN ELEVEN THOUSAND reaching it. The original's arc crosses a whole
     * junction block, which needs the corner known before the car arrives.
     *
     * So when the next route node is straight ahead and the one after it
     * turns, the turn is requested now and its line measured one block along.
     * `ready` still requires dist <= GTA_TURN_RADIUS, which is less than a
     * 32-px block, so this cannot fire more than one block early. `ahead` is
     * read by the line computation and by nothing else. */
    ahead = 0;
    if (c->turn == 0 && want_dir == c->angle &&
        !is_junction(m, bx, by, c->layer)) {
        if (c->crossing) {
            /* THE PRE-AIM OBEYS THE BOOKING. This block used to read the
             * raw route nodes and overwrite want_dir AFTER the binding
             * override - so arcs armed one square early on the nodes'
             * staircase geometry, off the booked shape, and a turner ended
             * up standing on somebody else's square with its commitment
             * broken (car#4 at (50,44), 132 s). While committed, the only
             * corner the pre-aim may arm is the BOOKED bend, from the
             * square directly before it. */
            int hx2, hy2;

            heading_step(c->angle, &hx2, &hy2);
            if (c->bend_dir != 255 &&
                bx + hx2 == c->bend_bx && by + hy2 == c->bend_by) {
                want_dir = c->bend_dir;
                ahead = 1;
            }
        } else if (c->path_i + 1 < c->path_n) {
            int ax  = c->path[c->path_i].x,     ay  = c->path[c->path_i].y;
            int nx2 = c->path[c->path_i + 1].x, ny2 = c->path[c->path_i + 1].y;
            int d2 = -1;
            if      (nx2 == ax     && ny2 == ay - 1) d2 = 0;
            else if (nx2 == ax + 1 && ny2 == ay)     d2 = 64;
            else if (nx2 == ax     && ny2 == ay + 1) d2 = 128;
            else if (nx2 == ax - 1 && ny2 == ay)     d2 = 192;
            if (d2 >= 0 && ((d2 - c->angle) & 255) != 0 &&
                           ((d2 - c->angle) & 255) != 128) {
                want_dir = d2;
                ahead = 1;
            }
        }
    }

    if (c->turn == 0 && want_dir != c->angle) {
        int d = (want_dir - c->angle) & 255;

        /* Remember that a turn was wanted in THIS block, so the check at the
         * top of the next tick can tell whether it actually happened. */
        c->turn_want = 1;
        c->turn_want_bx = bx;
        c->turn_want_by = by;
        c->turn_want_from = c->angle;
        c->turn_want_routed = (c->path_i < c->path_n);


        if (d == 128) {
            /* A U-turn cannot be steered in one arc. Flip on the spot, which
             * is what the original does, and only where the map allows it.
             *
             * AND NEVER ON A CROSSING. The flip belongs to the
             * original's STUCK RECOVERY - a car wedged against scenery turning
             * itself round - not part of normal driving, and this port has no
             * stuck recovery (see GTA_TRAFFIC_STUCK for the two attempts and
             * their numbers). Reaching it in the middle of a junction is the
             * "it spins round in the road" the developer photographed. A
             * genuine dead end on ordinary road still turns the car, which is
             * the one place a driver really would. */
            /* AND A U-TURN OBEYS THE ARROWS TOO. It writes the heading
             * straight into the car without going through the corner logic,
             * so the check above never sees it. */
            if (is_junction(m, bx, by, c->layer) || c->turn_lock != 0 ||
                !dir_allowed(m, bx, by, c->layer, want_dir)) {
                want_dir = c->angle;            /* carry straight on instead */
            } else {
                c->face = want_dir;
                c->angle = want_dir;
                c->turn_lock = 0;
                heading_step(c->angle, &dxs, &dys);
            }
        } else {
            /* THE ORIGINAL'S TEST, AND IT IS A COMPARISON OF BLOCKS, NOT A
             * DISTANCE TO A CORNER.
             *
             * Take the point one lookahead in front of the car, ask which
             * block it is in, and compare that block with the one the route
             * wants on the CROSS axis. While they disagree, the turn request
             * stands and its accumulator is re-zeroed; when they agree, the
             * request is left to expire. That is the original's own rule
             * verbatim, and it is a closed loop: a corner
             * that comes out a little short or wide is simply still wrong on
             * the next tick, so the car keeps turning.
             *
             * The lookahead is the vehicle's own half length, floored at half
             * a block, which is what makes a bus commit to its turn before a
             * Mini does. */
            int ready, twx, twy, aim_tgt, aim_r;
            long aim_line = 0;    /* the lane line the arc lands on */
            long dist_look = 0;   /* how far the exit lane line still is */

            heading_step(want_dir, &twx, &twy);
            {
            int tgt, gx = bx, gy = by;
            long line, dist;

            /* THE TRIGGER IS THE GEOMETRY, NOT A GUESS ABOUT BLOCKS.
             *
             * A quarter circle of radius R displaces the car by exactly R
             * along the heading it started on. So to finish the corner sitting
             * on the centre line of the lane it is joining, the car has to
             * begin turning exactly R before that line - no more and no less.
             *
             * The line is on the car's CURRENT along-axis, which is the same
             * axis that becomes its cross-axis after the turn: a car heading
             * north into a westbound row ends up at that row's lane centre, a
             * y value, and y is what it is travelling along now.
             *
             * What was here instead compared BLOCKS - had the point one
             * lookahead in front of the car reached the block the route turns
             * at - which knows nothing about where inside that block the lane
             * actually is, and nothing about how big the arc will be. It began
             * the turn at whatever moment a block boundary happened to be
             * crossed, the car landed beside its lane, and the lane keeper
             * then dragged it across a few pixels after the junction. That
             * sideways correction is the fault that was reported over and over
             * and it was never in the lane keeper: it was here. */
            /* THE AIM IS THE BLOCK THE CAR LANDS IN, and cleverer was tried.
             *
             * The block just past a crossing has the crossing beside it, so its
             * kerb test answers differently from the street one block further
             * on, and the car is corrected when it gets there. Two ways of
             * dodging that were measured and both are WORSE than doing nothing:
             * walking past the crossing to the first ordinary street block
             * costs 39419 -> 41497 px of corner correction, and additionally
             * making the lane keeper skip those blocks so the two agree costs
             * 45990. The reason is the same both times - the keeper works from
             * the block the car is IN, so aiming anywhere else guarantees a
             * correction on arrival, and holding a value across the crossing
             * only moves the disagreement earlier. The remaining 1.5 px needs
             * the keeper and the aim to be ONE mechanism rather than two that
             * are talked into agreeing. */
            /* AND THE CORNER IS MEASURED FROM WHERE IT WILL BE DRIVEN, WHICH
             * IS NOT ALWAYS THIS BLOCK.
             *
             * `line` is the centre of the lane being joined, on the car's
             * current along-axis - so when the turn is taken in the block the
             * car is standing in, the whole approach is one block: at most 32
             * world px, and after subtracting where the car entered and one
             * step of travel, about twelve. Measured
             * (gta_traffic.stat_aim_r_sum): 12.5 px of a 29 px ceiling, and
             * NOT ONE TURN IN ELEVEN THOUSAND reaches the ceiling. That is why
             * raising GTA_TURN_RADIUS above 20 changed nothing at all.
             *
             * The original's arc consumes a whole 64-px block - it enters a
             * junction at one edge and leaves by the perpendicular one - which
             * needs the turn to be KNOWN a block before it is taken. So when
             * the route's next node is straight ahead and the one after it
             * turns, the corner is set up now and its line measured in the
             * block ahead. `ready` still needs dist <= GTA_TURN_RADIUS, and
             * that is less than the 32 px of a block, so this cannot fire more
             * than one block early. */
            {
            int abx = bx + (ahead ? dxs : 0), aby = by + (ahead ? dys : 0);
            /* SAMPLE THE LANE PAST THE CROSSING, NOT INSIDE IT.
             *
             * lane_target_at() nudges a car away from a kerb, and inside a
             * junction there is road on all four sides, so it answers "middle
             * of the block" there and something else one block further on -
             * which is where the car will actually be driving. Aiming at the
             * junction's answer lands the car exactly on a line that stops
             * existing two blocks later, and the lane keeper then drags it
             * across: 54% of corners ended 2-3 px out with the landing itself
             * measured at ZERO error.
             *
             * Walking along want_dir does not change the block index the LINE
             * is built from - that index is on the cross axis and want_dir is
             * along it - so this changes only which block's kerbs are asked.
             *
             * This was tried once before and lost (the notes, "the aim is
             * the block the car lands in"), when the arc still missed its aim
             * by 1.8 px in a direction that happened to cancel most of this.
             * With the landing exact there is nothing left to cancel it. */
            {
                int k = 1;
                while (k < 4 && is_junction(m, abx + k * twx, aby + k * twy,
                                            c->layer))
                    k++;
                gx = abx + k * twx;
                gy = aby + k * twy;
                tgt = lane_target_at(m, gx, gy, c->layer, want_dir);
            }
            if ((c->angle & 127) == 0)      /* travelling on y: the line is a y */
                line = (((long)(aby + twy) * 32) + tgt) << FP;
            else                            /* travelling on x: the line is an x */
                line = (((long)(abx + twx) * 32) + tgt) << FP;
            }

            switch (c->angle) {
            case 0:   dist = c->y - line; break;    /* north: y falls to it */
            case 128: dist = line - c->y; break;
            case 192: dist = c->x - line; break;    /* west: x falls to it */
            default:  dist = line - c->x; break;
            }

            /* At or past the point: turn. PAST it counts, because a car that
             * has overshot - it was held at the line by a queue, or the route
             * changed under it - still has to make the corner, and turning
             * late is recoverable where not turning at all is not. */
            ready = (dist <= ((long)GTA_TURN_RADIUS << FP));

            /* AN ARC MAY NEVER LAND ON THE PAVEMENT. The line is measured
             * from the block the request stood in, and a stray car -
             * re-routed mid-approach, or steering by the arrows after a
             * stuck spell - can raise a corner whose landing block is the
             * PAVEMENT past a T-mouth: the arc then drives it exactly there
             * (traced at (50,31): a re-routed car armed north with its line
             * in the pavement column and parked on the footway - the
             * developer's photo from the other small T). Road-ness is the
             * whole test: direction is NOT checked, because the turn-back a
             * route asks for at a dead end legitimately crosses into the
             * oncoming lane, and refusing it sent a bus straight off the
             * end of the street instead. */
            if (ready && ground_at(m, gx, gy, c->layer) != GROUND_ROAD)
                ready = 0;

            /* AND THE ARC IS DRIVEN AT WHATEVER DISTANCE THE CAR ACTUALLY HAS,
             * not at the nominal radius - see gta_car.turn_radius. This is what
             * takes the tick boundary out of the answer. Clamped low so a car
             * that has already overshot still describes a real corner rather
             * than pivoting, and high so an early trigger cannot swing it wide. */
            /* ...MINUS ONE STEP, because the tick that ISSUES a turn does not
             * turn: the rotation is applied at the top of drive_one and the
             * trigger is below it, so the car travels one more step straight
             * before the arc begins. Setting the radius to the distance the
             * car has NOW therefore overshoots by exactly one step, every
             * time, which is most of what was left of the landing error. */
            /* AND IT STAYS IN WHOLE PIXELS, THOUGH THAT LOOKS LIKE A BUG.
             *
             * A quarter arc displaces the car by EXACTLY its radius, so
             * truncating a radius that is only 3 to 8 pixels long throws up to
             * a whole pixel straight into the landing, every corner - and with
             * the aim measured as dead on 97% of the time (stat_aim_bias) the
             * geometry is all that is left. Keeping the radius in 16.16 and
             * dividing by (R_fp >> 14) instead of (R << 2) is four lines.
             *
             * IT WAS MEASURED AND IT IS A CLEAR LOSS. The geometry error did
             * not move - 1.6 px per corner either way, RIGHT 1.5/LEFT 1.6
             * against 1.5/1.7 - and `BOX DEADLOCK` went from 114 car-ticks
             * over the 96-site sweep to 3228, with the worst site's flow
             * falling to 45%. A finer radius makes the arc rate vary
             * continuously between cars, and cars that take marginally
             * different times through a crossing stop clearing it together.
             *
             * ROUNDING INSTEAD OF TRUNCATING WAS ALSO MEASURED AND ALSO LOST.
             * `+ (1 << (FP-1))` before the shift keeps the radius a whole
             * number - which the experiment above showed it has to be - and
             * should halve the truncation. Geometry error 1.6 px either way,
             * turns on the line 32% -> 31%, flow 87% -> 86%, `BOX DEADLOCK`
             * 114 -> 204. Two attempts at the radius, both flat on the thing
             * they targeted and both worse elsewhere.
             *
             * WHICH MEANS THE ARITHMETIC HERE IS NOT THE FAULT, and the
             * measurement says where it is. Bucketing the geometry error by
             * the step length at the tick the turn was issued
             * (`stat_geom_by_step`) gives:
             *
             *      0.0 px/tick    193 corners - 2.6 px      <- rotating on the spot
             *      0.5 px/tick   2628 corners - 1.2 px
             *      1.0 px/tick   6626 corners - 1.6 px
             *      1.5 px/tick   2167 corners - 1.9 px
             *
             * It rises with the step, and `aim_r` ALREADY subtracts one step -
             * so the trigger's tick is compensated and something else is
             * proportional to the step. That something is the ARC ITSELF: the
             * rotation rate is v/R per tick, so a faster car goes round in
             * fewer, longer chords, and the displacement of a discrete chord
             * approximation falls short of the true arc by more the coarser it
             * is. The fix is a correction on the radius that depends on the
             * step, or an integration that is not a chord - not a rounding
             * mode. Neither has been tried. */
            /* THE RADIUS IS THE DISTANCE THAT IS ACTUALLY LEFT, and no
             * longer a guess corrected by a fudge.
             *
             * A quarter circle displaces the car by exactly its radius along
             * the heading it started on, so the ONLY radius that lands it on
             * the exit lane line is the distance to that line at the instant
             * the arc begins. `- c->speed` used to be subtracted because the
             * old code turned at the top of the tick and armed at the bottom,
             * so a step of straight driving happened in between; the arc is
             * evaluated from the position it is committed at, so there is no
             * such step and nothing to compensate for.
             *
             * The trigger already guarantees dist <= GTA_TURN_RADIUS, so there
             * is no upper clamp: clamping would break the tangency and put the
             * car back beside its lane, which is the fault being fixed. The
             * floor is for a car that has overshot the line - it makes a tight
             * corner and the lane keeper tidies the last pixel or two, which
             * is better than pivoting on the spot. */
            dist_look = dist;
            aim_r = (int)(dist >> FP);
            if (aim_r < GTA_TURN_RADIUS_MIN) aim_r = GTA_TURN_RADIUS_MIN;
            aim_tgt = tgt;
            aim_line = line;
            }

            /* THE BLOCK TO CHECK IS THE ONE THE TURN LANDS IN - one step along
             * want_dir - and for a long time this tested one step along the
             * CURRENT heading instead: the block the car would have gone
             * STRAIGHT into. So "does the lane I am turning into have room"
             * was asked about a lane the car was not going to enter, turns
             * were issued into fully occupied exits, and that is both the
             * eight-cars-in-two-pixels merge pile-up at (36,156) and half of
             * every junction knot: a turner that commits into a full lane
             * stops inside the crossing, across everybody else's path. */
            nx = bx + twx;
            ny = by + twy;
            /* AND NOT TWICE INSIDE ONE CROSSING, EITHER WAY.
             *
             * Two legal ninety-degree turns a second apart are a U-turn if
             * they go the same way, and a LANE CHANGE if they go opposite
             * ways - right then left leaves the car pointing where it started,
             * one lane over, in the middle of a junction. This test used to
             * name the direction and so caught only the first of those; the
             * junction-line instrument counted the second at 62 crossings a
             * sweep. `turn_lock` is released the moment the car reaches a
             * block that is not part of a junction, so an S-bend on ordinary
             * road is untouched - what is refused is both halves being taken
             * inside the same box. */
            /* AND THE LANE-CHANGE LOCK DOES NOT APPLY TO A ROUTED TURN.
             *
             * The lock stops two turns being taken inside one crossing, which
             * leaves the car pointing where it started one lane over. That is
             * a real fault and the lock fixed it - but the thing that produces
             * it is `choose_heading()`, the arrow-following FALLBACK, which is
             * partly random and re-rolls every tick.
             *
             * A ROUTED turn cannot be a lane change. The search refuses to
             * leave a block backwards, requires the direction bit at every
             * step, and the route audit over a full run reports 27157 nodes
             * with zero diagonal steps and zero off-road blocks. So a lock
             * that refuses one is refusing a legal corner - and measured, ALL
             * of the missed turns at all three gate sites were routed ones:
             * 111/111, 412/412, 196/196. The lock was the single largest
             * source of them at (61,52), at 6075 refusal-ticks.
             *
             * The original has no such rule at all. Its equivalent protection
             * is structural: route nodes are at least TWO BLOCKS apart -
             * it discards any candidate closer than that to its parent - so
             * two corners inside one crossing are never asked for.
             * Ours are one per block, so the lock stays for the fallback. */
            if (ready && c->turn_lock != 0 && c->path_i >= c->path_n) {
                ready = 0;
                tr->stat_turn_refused_lock++;
            }

            /* AND NOT A SECOND TIME IN THE SAME CROSSING. See
             * gta_car.cross_lock_x: this is the "they turn more than once at
             * one junction" report, and unlike `turn_lock` above it names the
             * crossing rather than any junction block, so it cannot refuse a
             * legal corner at the next one along. It applies to routed turns
             * too - a route CAN ask for two corners inside one box when the
             * car has drifted and the search re-planned around it. */
            /* IS THE CORNER FREE? ASKED EARLY, AND ANSWERED BY EASING OFF.
             *
             * See GTA_TURN_LOOK. From a margin before the commit point the car
             * asks whether the cells its arc would sweep are free; if they are
             * not it slows instead of arriving at the line and stopping dead.
             * The radius used is the distance it has NOW, which is the radius
             * it would commit with, so the question is about the corner it
             * would actually drive. */
            if (tr->opt_occ_look && dist_look <= (long)(GTA_TURN_RADIUS +
                                                   GTA_TURN_LOOK) << FP) {
                int rr = (int)(dist_look >> FP);
                int sideang = (c->angle + ((d == 64) ? GTA_TURN_QUARTER
                                                     : -GTA_TURN_QUARTER)) & 255;
                int sdx2, sdy2, dirsign = (d == 64) ? 1 : -1;
                long ccx, ccy;
                if (rr < GTA_TURN_RADIUS_MIN) rr = GTA_TURN_RADIUS_MIN;
                heading_step(sideang, &sdx2, &sdy2);
                ccx = c->x + (((long)rr * gta_sin(sideang)) << 2);
                ccy = c->y - (((long)rr * gta_cos(sideang)) << 2);
                if (!arc_reserve(tr, m, c, ccx, ccy, rr, c->angle, dirsign,
                                 gta_car_world_len(info) > GTA_LONG_CAR, 0, 0)) {
                    ready = 0;
                    give_way = 1;
                    tr->stat_turn_refused_occ++;
                }
            }

            /* AND ONLY FROM ORDINARY ROAD. See gta_car.allow_turn: a car
             * already standing on a junction block has used up its turn for
             * that crossing and carries straight on through it. */
            if (ready && tr->opt_cross_lock && !c->allow_turn) {
                ready = 0;
                tr->stat_turn_refused_cross++;
            }

            /* AND THE ARROWS PAINTED ON THE BLOCK THE CAR IS STANDING ON.
             *
             * That block is the LANE, and the arrows over a lane say which
             * ways traffic may leave it - "straight or right" over the kerb
             * lane, "left" over the inner one. A car turns OUT of its lane, so
             * the lane's own bits are the ones that govern.
             *
             * Asking the block one step along instead - which is where the
             * corner is DRIVEN when the route set it up early - was tried and
             * is wrong: that block is the junction itself, and a junction
             * permits every direction by definition, so the check refused
             * nothing at all. Measured with an instrument that shares no code
             * with this rule (`arrowwatch` in gtadump): 532 of 1103 turns at
             * (64,64) came out of a block whose arrows forbid them, and this
             * rule had refused ZERO of them.
             *
             * "z alejek tylko w prawo lub prosto [...] skrecaja w lewo tez"
             *
             * The map says, per block, which ways traffic may LEAVE it - that
             * is what the arrows over the road are drawn from. The route
             * search honours them when it plans, but a car that has drifted a
             * lane, or is following the arrow FALLBACK, or is taking a corner
             * set up one block earlier, can reach the commit with a direction
             * this block does not permit. One lookup closes it. */
            c->turn_chk_bx = c->lane_bx;
            c->turn_chk_by = c->lane_by;
            c->turn_ahead  = ahead;
            c->turn_want_dir = want_dir;
            /* YOU TURN LEFT FROM THE LEFTMOST LANE AND RIGHT FROM THE KERB
             * LANE. See GTA_LANE_TURN_RULE.
             *
             * Tested on the last PLAIN lane block the car stood on - one
             * direction bit - because inside a crossing every neighbour is
             * road going every way and the question has no answer there.
             *
             * Refused at the COMMIT, never half way round: cancelling an arc
             * in flight leaves the car off its line, and the share of
             * crossings left on the correct line fell from 99% to 87% when it
             * was done that way. */
            if (ready && tr->opt_arrows) {
                int sdx, sdy;
                int d3 = (want_dir - c->angle) & 255;
                int side = (c->angle + ((d3 == 64) ? GTA_TURN_QUARTER
                                                   : -GTA_TURN_QUARTER)) & 255;
                heading_step(side, &sdx, &sdy);
                if (d3 != 128 &&
                    ground_at(m, c->appr_bx + sdx, c->appr_by + sdy,
                              c->layer) == GROUND_ROAD &&
                    dir_allowed(m, c->appr_bx + sdx, c->appr_by + sdy,
                                c->layer, c->angle)) {
                    ready = 0;
                    tr->stat_turn_refused_lane++;
                }
            }

            /* The commit-time version of this asked whether the LANE block
             * carried `want_dir`. It carries exactly one bit, the way the lane
             * runs, so that test refused every corner taken from a straight
             * run and permitted every corner taken from a junction block -
             * which is neither what the arrows say nor what a driver does. The
             * rule that replaces it is in section 1, on the arc. */

            /* THE EXIT LANE DECIDES THE SPEED, THE ONCOMING LANE DECIDES
             * WHETHER TO GO AT ALL. See oncoming_moving() for the two
             * functions in the original this splits apart, and why running
             * them together deadlocks. */
            if (ready) {
                exit_busy = !room_for(tr, idx, ((long)nx * 32 + 16) << FP,
                                      ((long)ny * 32 + 16) << FP,
                                      c->layer, want_dir,
                                      gta_car_world_len(info),
                                      (c->wait > GTA_TRAFFIC_PATIENCE)
                                          ? 0 : GTA_TRAFFIC_MIN_GAP);
                if (exit_busy)
                    tr->stat_turn_refused_room++;
                /* DO NOT BLOCK THE BOX.
                 *
                 * A corner whose exit lane has no room ends with the car
                 * stopped ACROSS the crossing, which is the pile-up the
                 * developer photographed: five vehicles wedged diagonally,
                 * every arm blocked, nothing able to move. Until now
                 * `exit_busy` only capped the SPEED of the turn - the car went
                 * in anyway, slowly.
                 *
                 * Refusing costs a tick on the approach, where a stopped car
                 * blocks one lane instead of four. GTA_SWEEP_PATIENCE is the
                 * escape, so an exit that never clears cannot hold the
                 * approach for ever. */
                if (tr->opt_keepclear && exit_busy &&
                    c->wait <= GTA_SWEEP_PATIENCE)
                    ready = 0;
            }

            /* THE BOOKED BEND IS TAKEN, FULL STOP. The overlay caught a car
             * with its right turn booked in brown sailing straight through
             * the bend square: one of the commit-time vetoes (lane
             * discipline, exit room, keep-clear) said no, the car slid past
             * its own bend, off its own squares, the presence commitment
             * broke, and it finished the crossing with no reservation at
             * all - the exact "ignoruje rezerwacje" on the developer's two
             * screenshots. The gate already validated the exit when it
             * GRANTED the bend; nothing at the commit is entitled to
             * overrule the booking. The hard bumper gap still stops it from
             * ramming - nothing else does. */
            if (c->crossing && c->bend_dir != 255 &&
                bx == c->bend_bx && by == c->bend_by &&
                want_dir == c->bend_dir &&
                dist_look <= ((long)GTA_TURN_RADIUS << FP)) {
                /* only the VETOES are overruled - the geometric trigger
                 * (close enough to the exit lane line to arc onto it)
                 * still decides WHEN the wheel turns */
                ready = 1;
                give_way = 0;
            }

            if (!ready) {
                /* Not yet, and nothing said - this is every tick of every
                 * approach to every corner in the city. */
            } else {
                /* AND A SECOND CORNER CANCELS THE FIRST ONE'S SLIDE
                 * MEASUREMENT, which is not bookkeeping - leaving it armed
                 * makes the number nonsense. The slide is read on ONE axis,
                 * the cross axis of the corner it followed; once the car is
                 * turning again it travels ALONG that axis, so ordinary
                 * forward motion is counted as sideways movement. That is what
                 * put 6% of corners in the 8+ bucket and dragged the average to
                 * 6 px while 80% of them were inside a pixel. */
                c->slide_left = 0;
                c->turn = (d == 64) ? 1 : -1;   /* the DIRECTION; the arc it
                                                 * describes is committed just
                                                 * below and never changes */
                c->turn_from = c->angle;
                c->turn_accum = 0;
                c->turn_frac = 0;
                c->turn_lock = (d == 64) ? 1 : -1;
                c->turn_routed = (c->path_i < c->path_n);
                c->cross_turns++;
                c->turn_aim_tgt = aim_tgt;
                c->turn_aim_bx  = nx;      /* the block the aim named */
                c->turn_aim_by  = ny;
                c->turn_radius  = aim_r;
                c->turn_ticks   = 0;

                /* COMMIT THE CIRCLE. Everything the corner needs is decided
                 * here, once, and nothing about it can move afterwards.
                 *
                 * First the car is put ON its own lane line, because an arc
                 * that starts a pixel to one side ends a pixel to that side.
                 * The keeper holds the car inside a band rather than on the
                 * line, so this is normally a pixel or two; a car further out
                 * than that is somewhere the geometry cannot vouch for and is
                 * left alone rather than yanked across. */
                {
                    long *p = ((c->angle & 127) == 64) ? &c->y : &c->x;
                    long ctr = (((((*p) >> (FP + 5)) * 32) + c->lane_target)
                                << FP);
                    long d2 = *p - ctr;
                    if (d2 <= (8L << FP) && d2 >= -(8L << FP))
                        *p = ctr;
                }
                /* Then the centre: exactly `aim_r` pixels to the side the car
                 * is turning towards. The side is an axis, so the table gives
                 * 0 or +-16384 and this is exact rather than nearly. */
                {
                    int side = (c->angle + ((d == 64) ? GTA_TURN_QUARTER
                                                      : -GTA_TURN_QUARTER)) & 255;
                    c->arc_cx = c->x + (((long)aim_r * gta_sin(side)) << 2);
                    c->arc_cy = c->y - (((long)aim_r * gta_cos(side)) << 2);
                }
                c->arc_len  = (long)aim_r * GTA_ARC_QLEN;
                c->arc_s    = 0;
                c->arc_line = aim_line;

                /* AND THE CELLS ARE TAKEN NOW, so nothing else enters them
                 * while the corner is driven - see arc_reserve(). */
                if (tr->opt_occ_hold)
                    arc_reserve(tr, m, c, c->arc_cx, c->arc_cy, aim_r,
                                c->angle, c->turn,
                                gta_car_world_len(info) > GTA_LONG_CAR, 1, 0);
                c->allow_turn = 0;      /* one turn per crossing - the bit */
                c->turn_free_at = c->odo + GTA_TURN_AGAIN;

                /* AND THE CORNER HAS TO BOOK THE CROSSING IT IS DRIVING INTO.
                 *
                 * The gate in section 4 below claims a junction for a car that
                 * ENTERS it in a straight line. A turn does not go through
                 * that gate: it is committed here, on the approach block,
                 * which is not part of the junction, and the arc then carries
                 * the car inside. That is the leak the box instrument found -
                 * 8101 car-ticks of two cars sharing a crossing at (64,64) and
                 * only 10 of them with no owner, so the reservation was being
                 * honoured by everybody except the car turning.
                 *
                 * Refusing costs a tick on the approach; the car asks again
                 * next tick, and `wait > GTA_SWEEP_PATIENCE` is the escape so
                 * a crossing whose owner has wedged itself cannot block the
                 * approach for ever. */
                {
                    int jx = -1, jy = -1, want_box = 0;
                    if (is_junction(m, nx, ny, c->layer)) {
                        junction_root(m, nx, ny, c->layer, &jx, &jy);
                        want_box = 1;
                    } else if (is_junction(m, bx, by, c->layer)) {
                        junction_root(m, bx, by, c->layer, &jx, &jy);
                        want_box = 1;
                    }
                    if (tr->opt_arcclaim && want_box && c->wait <= GTA_SWEEP_PATIENCE &&
                        !junction_claim(tr, jx, jy, c->layer, c->serial)) {
                        c->turn = 0;
                        c->arc_s = 0;
                        tr->stat_turn_refused_claim++;
                        give_way = 1;
                    }
                }


                /* AND IF ANOTHER CAR IS STANDING ON THAT ARC, WAIT FOR IT -
                 * but not for ever. See GTA_SWEEP_PATIENCE. */
                if (tr->opt_sweep && c->wait <= GTA_SWEEP_PATIENCE &&
                    !arc_clear(tr, idx, c->arc_cx, c->arc_cy, aim_r,
                               c->angle, c->turn)) {
                    c->turn = 0;
                    c->arc_s = 0;
                    tr->stat_turn_refused_sweep++;
                    give_way = 1;
                }

                tr->stat_aim_r_sum += aim_r;
                tr->stat_aim_r_n++;
                if (aim_r >= GTA_TURN_RADIUS) tr->stat_aim_r_capped++;
                c->turn_step    = (int)(c->speed >> (FP - 1));  /* HALF pixels:
                                    * whole ones gave only two buckets, because
                                    * no car issues a turn above 2 px a tick.
                                    * For the test in
                                    * gta_traffic.stat_geom_by_step */
                if (c->turn_routed) tr->stat_turns_routed++;
                else                tr->stat_turns_fallback++;
                /* AND SLOW DOWN NOW, not over the next few ticks - the
                 * original ASSIGNS the speed when it issues the turn rather
                 * than asking the brake for it.
                 *
                 * It no longer changes the SHAPE of the corner, which it did
                 * when the turn rate was fixed and the radius was therefore
                 * speed over rate; the arc is now the same arc whatever the
                 * speed. What it still does is keep a bus from crossing a
                 * junction at full cruise, and give the car behind time to
                 * react to a vehicle that is about to slow to a walk. */
                /* The original writes 2 when the next node is adjacent to
                 * the lookahead block and 5 otherwise; ours takes the tighter
                 * of the two when the exit lane already has somebody in it,
                 * which is the same idea reached from the occupancy side. */
                {
                    long cap = (long)(exit_busy ? GTA_SPEED_TURN_TIGHT
                                                : GTA_SPEED_TURN_WIDE)
                               * GTA_SPEED_UNIT;
                    if (c->speed > cap) c->speed = cap;
                }
            }
        }
    }

    /* --- 4. what the block ahead is, and who has claimed it ---------------- */
    nx = bx + dxs;
    ny = by + dys;
    /* A car claims the block it is about to enter once it is within a block of
     * it, which is what makes the junction reservation work with free
     * steering: `cell_x/cell_y` used to be the block being driven into and
     * still is. */
    if (edge < (32L << FP) && c->speed > 0) {
        /* AND ONLY WHILE IT IS ACTUALLY MOVING TOWARDS IT. A car that has
         * stopped short of a junction is not in it and must not hold it: two
         * cars that stop on opposite approaches would otherwise each wait for
         * the other's claim for ever. A car that is IN the box still blocks
         * it, because that is its own block. */
        c->cell_x = nx;
        c->cell_y = ny;
    } else {
        c->cell_x = bx;
        c->cell_y = by;
    }

    /* --- 5. speed ---------------------------------------------------------
     *
     * The original's ladder, in blocks of clear road, with our own
     * bumper-to-bumper gap kept underneath it: the ladder is coarse (it works
     * in whole blocks) and the gap is what stops two cars actually touching. */
    clear = road_clear(tr, m, c, bx, by, c->layer, want_dir, 10);
    /* THE LANE PROJECTION DECIDES THE FOLLOWING DISTANCE, THE RECTANGLE STOPS
     * THE CRASH. Two different jobs and they want two different ranges.
     *
     * gap_ahead() is the classic follow rule and it is what keeps 96% of the
     * city moving: keep a distance from the car in front, in your own lane, on
     * your own axis. It cannot see a car crossing your bonnet and that is the
     * fault being fixed - but making the rectangle do BOTH jobs brakes for
     * every vehicle crossing every junction within a second and takes the
     * fleet to a quarter of itself.
     *
     * So the rectangle runs with a short horizon and only ever tightens the
     * answer: it is an emergency brake, not a following distance. */
    gap = gap_ahead(tr, idx, &lead, &lead_i);
    /* THE RESERVATION IS THE JUDGE - "jesli robi rezerwacje to musi jechac
     * rezerwacja i koniec". A committed car brakes for a body standing ON
     * one of ITS booked squares - its own queue, or a genuine violation -
     * and for nothing else. The perpendicular passer clipping the follow
     * cone twenty pixels off the nose is on its OWN booked path, protected
     * by the same table; braking for it is what read as "zacina sie" all
     * over the city (traced: car#2 held 7 by car#12 whose line shared no
     * square with its own). */
    if (c->crossing && gap >= 0 && lead_i >= 0) {
        const gta_car *lo = &tr->cars[lead_i];
        int obx = (int)(lo->x >> (FP + 5));
        int oby = (int)(lo->y >> (FP + 5));
        int qi, held2 = 0;

        for (qi = 0; qi < tr->claim_top; qi++)
            if (tr->claim_ttl[qi] > 0 &&
                tr->claim_car[qi] == c->convoy &&
                (int)tr->claim_x[qi] == obx &&
                (int)tr->claim_y[qi] == oby &&
                (int)tr->claim_z[qi] == lo->layer) {
                held2 = 1;
                break;
            }
        if (!held2)
            gap = -1;
    }
    /* THE LADDER IS ABOUT THE ROAD, NOT ABOUT THE CAR IN FRONT, and mixing the
     * two gridlocks the city. Folding the gap into `clear` was tried first:
     * cars then stopped three blocks apart, `clear` stayed below the
     * three-block threshold for every one of them, and nothing ever started
     * again - 21% of the fleet moving and one car stationary for sixteen
     * seconds. The road ladder decides how fast it is sensible to go down THIS
     * street; the pixel gap below decides how close it may get to the car in
     * front. */

    want = c->top;
    if (c->turn != 0) {
        /* Slow down to take it. The original writes 2 for the tighter case and
         * 5 for the wider one; ours is tighter when there is a vehicle to
         * merge with and wider when the road is empty. */
        want = ((clear >= GTA_CLEAR_EASE) ? GTA_SPEED_TURN_WIDE
                                          : GTA_SPEED_TURN_TIGHT)
               * GTA_SPEED_UNIT;
        if (want > c->top) want = c->top;
    }
    if (clear < GTA_CLEAR_EASE && want > 4L * GTA_SPEED_UNIT)
        want = 4L * GTA_SPEED_UNIT;
    if (clear < GTA_CLEAR_HARD && want > 2L * GTA_SPEED_UNIT)
        want = 2L * GTA_SPEED_UNIT;
    if (clear < GTA_CLEAR_STOP) {
        want = 0;
        c->hold = GTA_HOLD_ROAD;
    }

    /* GIVING WAY AT A CORNER. Set where the turn was refused for want of room
     * in the exit lane; see the long note there. The brake is rate-limited by
     * `approach()` below, so this is a car slowing into the give-way line
     * rather than stopping dead on it - which is also what the original does,
     * its `car+0xb1` brake taking one unit of speed off per frame. */
    if (give_way) {
        want = 0;
        c->hold = GTA_HOLD_MERGE;
    }
    (void)exit_busy;

    /* The pixel-level floor: whatever the ladder says, do not close on the car
     * in front. This is ours, not the original's, and it is what keeps the
     * overlap test at zero. */
    if (gap >= 0) {
        if (gap <= GTA_TRAFFIC_MIN_GAP) {
            want = 0;
            c->hold = GTA_HOLD_GAP;
        } else if (gap < GTA_TRAFFIC_FOLLOW_GAP) {
            long room = (gap - GTA_TRAFFIC_MIN_GAP) >> FP;
            long base = (lead < want) ? lead : want;
            long eased = (base * room) >> GTA_TRAFFIC_GAP_SHIFT;
            if (eased < want) want = eased;
        }
    }

    /* A red light at a stop line, and a junction box somebody else is in.
     * Both are simply "want to be stopped", which with a rate-limited brake is
     * a car slowing into the line rather than stopping dead on it. */
    /* WHERE IS THE BOX, AND FROM HOW FAR MUST THE QUESTION BE ASKED. The
     * decision distance is half the vehicle plus its TRUE braking distance
     * at its current speed - and when that reach spills past the next
     * block, the block after next is checked too. This is what makes the
     * entry decision final: a blocked car never begins braking so late
     * that its nose drifts over the line, and a free one books before its
     * nose is in. */
    jbx = jby = -1;
    ej = 0;
    if (is_junction(m, nx, ny, c->layer)) {
        jbx = nx;
        jby = ny;
        ej = edge;
    } else if (is_junction(m, nx + dxs, ny + dys, c->layer)) {
        jbx = nx + dxs;
        jby = ny + dys;
        ej = edge + (32L << FP);
    }
    stopr = GTA_TRAFFIC_BOX_LOOK +
            (((long)gta_car_world_len(info) / 2) << FP);
    if (c->brake > 0)
        stopr += (c->speed * (c->speed / c->brake)) >> 1;

    c->at_light = 0;
    if (c->hint_bx != bx || c->hint_by != by) {
        gta_block hb;
        c->hint_bx = bx;
        c->hint_by = by;
        c->hint_val = gta_map_block(m, bx, by, c->layer, &hb)
                      ? gta_block_traffic_hint(&hb) : 0;
    }
    /* A COMMITTED CAR IGNORES A RED that arrives after its decision -
     * "auto NIE MA PRAWA odwrocic swojej decyzji o ruszeniu ze swiatel".
     * And the lights run AT ALL only when opt_lights says so: nothing
     * draws them yet, and a car stopping at an invisible red at an empty
     * crossing reads as "ruszylo i sie zatrzymalo bez powodu" - the
     * developer's standing instruction is lights OFF until then. */
    if (tr->opt_lights && !c->crossing &&
        at_stop_line(m, c->hint_val, bx, by, c->layer, c->angle) &&
        !gta_traffic_light_green(tr, bx, by, (c->angle & 127) == 64)) {
        want = 0;
        c->at_light = 1;
        c->hold = GTA_HOLD_LIGHT;
    } else if ((!c->crossing ||
                (!is_junction(m, bx, by, c->layer) &&
                 !claim_mine(tr, c, jbx, jby))) &&
               jbx >= 0 && ej < stopr) {
        /* THE HANDOVER. A committed car that has left its booked squares
         * and finds ANOTHER box ahead it holds no claim on is exiting one
         * crossing towards the next - the pair the developer photographed.
         * It asks this same gate for the next box, every tick, from the
         * neutral square onwards; granted, the one irreversible commitment
         * simply extends (crossing stays up, the anchor re-seats below);
         * refused, it brakes and waits ON the neutral square, which is
         * exactly what that square is for. Cars that do not fit between
         * the boxes never get here - claim_route chained both boxes at the
         * first gate. */
        /* THE BOX IS THE WHOLE CROSSING, NOT ONE BLOCK OF IT.
         *
         * The roads here are two lanes wide, so a junction is a 2x2 patch of
         * junction blocks - and treating each block separately let a car stop
         * "at the stop line" of the second block while already standing in the
         * first, i.e. in the middle of the crossing, across the other
         * street's path. The frozen ring at (64,64) was made of exactly such
         * cars: every one of them had obeyed the one-block rule perfectly.
         *
         * So before entering the FIRST junction block, the car checks the
         * run of junction blocks along its heading (the whole crossing, up to
         * three) for cross traffic, and the first non-junction block beyond
         * them - the EXIT - for a stopped car. If the route turns inside the
         * box, the exit it checks is the route's, around the corner. Blocked
         * either way means wait HERE, outside the box, which is the rule that
         * keeps a crossing passable however long the queues behind it are.
         *
         * `wait <= PATIENCE` above still lets a car edge in after three
         * seconds, so a genuinely wedged crossing degrades to pushing through
         * rather than to a city-wide freeze. */
        int jx = jbx, jy = jby, steps = 0, blocked = 0;
        int ew = ((c->angle & 127) == 64) ? 1 : 0;
        int exx, exy, found_exit;

        /* THE CONVOY FAST PATH. If our booked shape is exactly the route a
         * committed car already holds, join it and go - the body-in-line,
         * first-square and room checks below would all refuse over the very
         * convoy we are joining, so they are not asked. Everything else
         * (third-party bodies, other claims) was checked inside the join. */
        if (tr->opt_holdbox &&
            claim_route(tr, m, c, idx, jbx, jby, dxs, dys, 0, 1)) {
            c->crossing = 1;
            c->book_ax = bx;
            c->book_ay = by;
            c->why_box = 0;
            goto gate_done;
        }

        while (steps < GTA_BOX_SCAN && is_junction(m, jx, jy, c->layer)) {
            if (box_busy(tr, m, idx, jx, jy, c->layer, ew)) {
                if (!blocked) { tr->stat_box_why[0]++; c->why_box = 1; }
                blocked = 1;
            }
            jx += dxs;
            jy += dys;
            steps++;
        }
        /* DID THE SCAN ACTUALLY REACH THE FAR SIDE? Liberty City has junction
         * complexes wider than any fixed scan - around (213,117) the arrows
         * run `+++` for five blocks - and when the walk runs out of steps the
         * block it stops on is still part of the crossing. Treating that as
         * "the exit" and asking whether it is occupied refuses entry more or
         * less permanently, because a junction block usually has somebody in
         * it: at (204,108) that was 65659 car-ticks held by the box and six
         * per cent of the fleet moving. If the far side cannot be seen, the
         * cross-traffic test above stands on its own and the exit test is
         * skipped - letting a car in and having it queue is recoverable,
         * refusing it for ever is not. */
        found_exit = !is_junction(m, jx, jy, c->layer);
        exx = jx;                     /* straight-through exit */
        exy = jy;

        /* Where the ROUTE leaves the crossing, if it turns inside it: the
         * first path node past the junction run that is not a junction
         * block. That is the lane the car will actually need. */
        if (!blocked && c->path_i < c->path_n) {
            int k;
            for (k = c->path_i; k < c->path_n && k < c->path_i + 12; k++) {
                if (!is_junction(m, c->path[k].x, c->path[k].y, c->layer)) {
                    exx = c->path[k].x;
                    exy = c->path[k].y;
                    found_exit = 1;
                    break;
                }
            }
        }
        /* SOMEBODY IS ALREADY CROSSING: THE WHOLE JUNCTION IS THEIRS.
         *
         * junction_claim() answers 1 when the crossing is free or already
         * ours - and TAKES it in the first case - and 0 when another car holds
         * it. Nothing used to ask it here; it was only the escape valve
         * further down. See gta_traffic.opt_holdbox. */
        /* AND ONLY WHEN THE CAR IS ACTUALLY ABOUT TO GO IN.
         *
         * Taking the path a whole block out, or while stopped in a queue,
         * hands the crossing to a car that is not using it: every arm waits
         * for a vehicle that is itself waiting for the car in front of it.
         * Measured without this guard: (50,44) 77% of the fleet moving down to
         * 31%. Within half a block, and moving, is "going in". */
        /* THE CELL AHEAD IS SOMEBODY ELSE'S - see GTA_OCC_MAX. This is the
         * whole of the developer's matrix: a junction block another vehicle's
         * BODY is standing on may not be driven into, and it frees itself the
         * moment that vehicle is off it. */
        if (!blocked && tr->opt_occ_hold) {
            unsigned long who = occ_owner(tr, jbx, jby, c->layer);
            /* AND A CELL SOMEBODY HAS BOOKED IS TAKEN TOO, not only one they
             * are standing on. arc_reserve() claims the squares a corner will
             * sweep precisely so that nothing else enters them - and this gate
             * was only ever reading the BODY matrix, so the booking had no
             * effect on anybody. That is why two cars still ended up on one
             * square with the reservation switched on. */
            if (who == 0) {
                int q;
                for (q = 0; q < GTA_CLAIM_MAX; q++)
                    if (tr->claim_ttl[q] > 0 &&
                        tr->claim_x[q] == (unsigned char)jbx &&
                        tr->claim_y[q] == (unsigned char)jby &&
                        tr->claim_z[q] == (signed char)c->layer) {
                        who = tr->claim_car[q];
                        break;
                    }
            }
            if (who != 0 && who != c->serial) {
                blocked = 1;
                tr->stat_box_why[1]++;
                c->why_box = 2;
                tr->stat_occ_refused++;
            }
        }
        if (!blocked && found_exit && block_full(tr, idx, exx, exy, c->layer)) {
            blocked = 1;
            tr->stat_box_why[2]++;
            c->why_box = 3;
        }
        /* ROOM FOR THE WHOLE CROSSING AND THE WAY OUT OF IT.
         *
         *   "sprawdzaja czy wolne np. 4 prosto [...] jesli potrzebuje 4 to
         *    rezerwuje 4 - nie ma prawa sie wtedy zatrzymac juz"
         *
         * The old test asked for one fixed clearance regardless of how wide the
         * crossing was, so a car was let into a four-block junction with room
         * for two - and stopped inside it. Measured at (64,64): of 6309
         * car-ticks spent standing still with a wheel in a crossing, 6209 were
         * held by the BUMPER GAP, which is the vehicle ahead not having got out
         * of the way. The gate could have known that and did not ask.
         *
         * What it asks for now is the depth of the crossing straight ahead,
         * plus this car's own length, plus the minimum gap - the distance the
         * car needs to be completely out the far side. */
        {
            int dblk = 0, jjx = jbx, jjy = jby;
            long need;
            while (dblk < GTA_PATH_MAX &&
                   is_junction(m, jjx, jjy, c->layer)) {
                dblk++;
                jjx += dxs;
                jjy += dys;
            }
            need = (((long)dblk * 32) + gta_car_world_len(info)) << FP;
            need += GTA_TRAFFIC_MIN_GAP;
            if (!blocked && gap >= 0 && gap < need) {
                blocked = 1;
                tr->stat_box_why[3]++;
                c->why_box = 4;
            }
        }

        /* THE DEVELOPER'S ENTRY SEQUENCE - shipped 2026-08-24 at their
         * instruction. The car asks from far enough out that its NOSE is
         * still outside the box (the gate distance above includes half its
         * own length), it keeps asking every tick INCLUDING WHILE STOPPED
         * (this branch does not depend on speed), and the instant every
         * check above says free it books the whole path in one go and
         * commits - c->crossing below, the gate is never asked again, the
         * car goes immediately. The booking is checked and taken LAST, after
         * every other refusal, so a car never holds ground it was not going
         * to be allowed onto anyway. */
        if (!blocked && tr->opt_holdbox &&
            !claim_route(tr, m, c, idx, jbx, jby, dxs, dys, 0, 0)) {
            tr->stat_box_why[4]++;
            c->why_box = (unsigned char)g_claim_why;
            c->why_side = (signed char)g_claim_side;
            c->why_fell = (unsigned char)g_claim_fell;
            /* LEFT IS BUSY - GO STRAIGHT INSTEAD, as specified: a car whose
             * route turns left across the box does not stand blocking its
             * arm for the turn. If the straight run books, it takes that,
             * drops the route, and asks for a fresh one beyond. */
            if (route_exit_side(m, c, jbx, jby, dxs, dys, 0, 0) == 2 &&
                claim_route(tr, m, c, idx, jbx, jby, dxs, dys, 1, 0)) {
                c->path_i = c->path_n;
                c->want_route = 1;
                tr->stat_left_skipped++;
            } else if (g_claim_why == 17 &&
                       (claim_route(tr, m, c, idx, jbx, jby, dxs, dys, 2, 0) ||
                        claim_route(tr, m, c, idx, jbx, jby, dxs, dys, 3, 0))) {
                /* STRAIGHT ON IS A WALL FROM THIS LANE - TURN BY THE
                 * ARROWS. The narrowing case: the route's straight runs on
                 * the neighbour column, ours ends at the box, and no
                 * booking the route offers is drivable from here. Right
                 * first (the kerb-side merge), left as the last resort;
                 * the route is dropped and re-planned beyond, exactly like
                 * the left-is-busy redirection above. */
                c->path_i = c->path_n;
                c->want_route = 1;
                tr->stat_left_skipped++;
            } else {
                blocked = 1;
                /* A ROUTE THE GATE CANNOT USE IS DEAD - REPLACE IT. The
                 * autopsy at (121,250): a route ENDING inside the box makes
                 * route_exit_side() answer -1, the straight fallback is a
                 * wall at a T, and with three nodes still left the refill
                 * never fires - the car stood ten seconds at an empty
                 * crossing (why 17, side -1). Dropping the stub makes the
                 * refill ask for a fresh route this tick; the car waits at
                 * the line meanwhile, which is what the line is for. */
                if (g_claim_why == 17 && g_claim_side < 0) {
                    c->path_i = 0;
                    c->path_n = 0;
                    c->want_route = 1;
                }
            }
        }

        /* THE ESCAPE VALVE IS CLOSED - developer's order 2026-08-25:
         * nothing pushes into a crossing past the reservation. Blocked
         * means wait, however long. (The old valve let a car force the
         * box after three seconds; a body holding no claim - and any
         * mid-manoeuvre car - was invisible to it.) */

        /* AND THE BOX HOLD IS LOAD-BEARING - measured 2026-08-23. Removing it
         * entirely, on the grounds that the original has no junction record
         * for an unlit crossing, took (204,108) from 26% of the fleet moving
         * to 4% with a car standing for 237 seconds. Cars simply pile into a
         * crossing they cannot leave. The original gets away without one
         * because it runs SEVEN cars near the view, not twenty. */
        if (blocked) {
            want = 0;
            c->hold = GTA_HOLD_BOX;
        } else {
            /* DECIDED. From here the car is going through, and the gate is not
             * asked again until it is out - see gta_car.crossing. */
            c->crossing = 1;
            c->book_ax = bx;
            c->book_ay = by;
            c->why_box = 0;
        }
    gate_done: ;
    }

    /* AND A CAR DOES NOT STOP HALF WAY ROUND A CORNER.
     *
     * With the arc's rate derived from the speed (GTA_TURN_RADIUS), a car that
     * stops mid-turn stops ROTATING, and until it moves again its heading is a
     * diagonal - so `angle`, which every other rule reads, is a rounded guess
     * at a direction the car is not travelling in. The old fixed rate hid this
     * by rotating a stationary car anyway; the honest version has to say what
     * happens instead.
     *
     * It is not a small effect. Car-ticks spent stopped mid-turn over 96
     * sites: 3217 with the fixed rate, 207953 with the derived one - and that
     * alone took the fleet from 88% moving to 80%.
     *
     * So a committed corner is finished. It is twelve pixels; the turn was
     * only issued because room_for() found space at the far end, and stopping
     * across a junction is the exact behaviour the rest of this file exists to
     * prevent. The one thing that still overrides it is the bumper-to-bumper
     * gap, because driving into the car in front is worse than any of this.
     *
     * AND THIS IS THE CODE THAT MAKES THAT PARAGRAPH TRUE. It was described
     * here and never written, which did not show while the turn was a rate -
     * a stopped car went on rotating - and showed the moment the corner became
     * a path. */
    if (c->turn != 0 && want < (long)GTA_SPEED_ARC_MIN * GTA_SPEED_UNIT &&
        (gap < 0 || gap > GTA_TRAFFIC_MIN_GAP ||
         (tr->opt_unwedge && c->wait > tr->opt_unwedge) ||
         (c->wait > tr->opt_creep && arc_step_free(tr, idx)))) {
        want = (long)GTA_SPEED_ARC_MIN * GTA_SPEED_UNIT;
        if (c->hold == GTA_HOLD_BOX || c->hold == GTA_HOLD_QUEUE)
            c->hold = GTA_HOLD_NONE;
    }
    /* THE RECTANGLE IN FRONT OF THE CAR, AS AN EMERGENCY BRAKE.
     *
     * Everything above decides how fast to go; this decides whether going at
     * all is about to put this car inside another one. It is deliberately the
     * LAST word, after the arc floor - a car frozen half way round a corner is
     * a nuisance and a car drawn inside another car is a bug the player sees.
     *
     * It is a trigger, not a distance: contact within opt_horizon ticks of
     * RELATIVE motion means stop, and nothing else. Feeding it into the
     * following-distance ladder instead was measured and is in the note on
     * gap_ahead_box(). */
    if (tr->opt_boxgap) {
        long tt = gap_ahead_box(tr, idx, &lead);
        if (tt >= 0 && tt <= tr->opt_horizon) {
            want = 0;
            tr->stat_blocked_move++;
            c->hold = GTA_HOLD_GAP;
        }
    }
    /* THE DECISION IS FINAL. From the moment a car books its crossing
     * (c->crossing) nothing short of the hard bumper gap, a physical
     * overlap or a dead end stops it - "auto NIE MA PRAWA odwrocic swojej
     * decyzji". The follow easing may slow it, never stand it still of
     * its own accord; GTA_HOLD_QUEUE is the unnamed-stop label, so it is
     * overridden too and a car it stamped restarts. */
    if (c->crossing &&
        (c->hold == GTA_HOLD_NONE || c->hold == GTA_HOLD_QUEUE) &&
        want < (long)GTA_SPEED_TURN_TIGHT * GTA_SPEED_UNIT)
        want = (long)GTA_SPEED_TURN_TIGHT * GTA_SPEED_UNIT;

    if (gta_traffic_trace_serial != 0 &&
        c->serial == gta_traffic_trace_serial)
        fprintf(stderr,
                "  WANT car#%lu want %ld.%02ld gap %ld clear %d lead %ld "
                "give %d hold %d cross %d turn %d\n",
                c->serial, want >> FP, ((want & 0xffffL) * 100) >> FP,
                gap >= 0 ? (gap >> FP) : -1, clear, lead >> FP,
                give_way, c->hold, c->crossing, c->turn);
    c->speed = approach(c->speed, want, c->accel, c->brake);
    if (c->speed <= 0) {
        c->speed = 0;
        if (c->hold == GTA_HOLD_NONE) c->hold = GTA_HOLD_QUEUE;
        c->wait++;

        return;
    }
    /* A MOVING CAR IS NOT BEING HELD BY ANYTHING, and the two exceptions that
     * used to be here made every diagnostic in the project unreliable: LIGHT
     * and BOX were never cleared, so a car that once waited at a light
     * reported "held by light" for the rest of its life, however far it drove
     * afterwards. The histograms in `drive` and in the game's own five-second
     * log were counting those ghosts - at (62,52) "light 45070" was the
     * largest hold in the run and most of it was cars that were queueing, or
     * moving, somewhere else entirely. Diagnosis was being done on it. */
    c->hold = GTA_HOLD_NONE;

    /* --- 6. move along the heading ---------------------------------------- */
    /* NOTHING CLAMPS THIS STEP AGAINST THE CAR IN FRONT, AND THAT WAS TESTED.
     *
     * The residual overlaps are a car pulling in front of another - the first
     * one at (36,156) reads
     *
     *   OVERLAP t=359 cars 9 and 16: 21 px apart, need 30  angles 192/192
     *
     * with the SAME heading for both, so it is a merge, not two streams
     * meeting. The obvious answer is to limit the move to the gap the car
     * actually has rather than trusting the speed decision. It clamps the
     * WRONG CAR: gap_ahead() floors the gap at zero, so by the time the
     * merging vehicle has taken the space the follower's step is already
     * limited to nothing, and the penetration was made by the TURNING car
     * moving sideways into a vehicle it never looks at - gap_ahead() only ever
     * looks forward. Over 96 sites: overlaps SUMMED 96 -> 93, which is noise,
     * while the longest anyone stood went 27.2 s -> 38.1 s and reversals
     * 459 -> 471.
     *
     * What that rules out is the whole family of fixes aimed at the FOLLOWER,
     * and two others in the same family are recorded in gap_ahead() above. The
     * car that has to give way is the one turning, and it has to do it before
     * it commits - see START HERE item 1 in the notes. */
    if (move_face == -2) {
        /* Turning: the arc placed the car at the top of this function and a
         * straight step on top of it would take it off the circle. */
        dx = dy = 0;
    } else {
        if (move_face < 0)
            move_face = c->face;        /* not turning: just the heading */
        dx =  ((long)gta_sin(move_face) * (c->speed >> 4)) >> 10;
        dy = -((long)gta_cos(move_face) * (c->speed >> 4)) >> 10;
        /* THE BACKSTOP - see car_free_at(). The follow gap is supposed to make
         * this impossible; it is a lateral test with a threshold and it has
         * holes, and a sprite drawn inside another sprite is the fault the
         * player sees. */
        /* AND IT ONLY EVER REFUSES A MOVE THAT WOULD *START* AN OVERLAP.
         *
         * Two cars that are already touching - spawned into each other, or
         * pushed together by the unwedge rule - would otherwise be frozen for
         * ever, because from inside an overlap EVERY move is still an overlap.
         * Measured, without this guard: 15% of the fleet moving at (64,64)
         * where 96% had been, and 4% at (204,108). The city locked solid.
         *
         * The speed is NOT zeroed. A car held for a tick keeps its momentum
         * and goes the moment the gap opens; zeroing it made every queue
         * concertina, which is a second way to lose the same flow. */
        if (!tr->opt_nooverlap ||
            !car_free_at(tr, idx, c->x, c->y, c->face) ||
            car_free_at(tr, idx, c->x + dx, c->y + dy, c->face)) {
            c->x += dx;
            c->y += dy;
        } else {
            tr->stat_blocked_move++;
            c->wait++;
            if (c->hold == GTA_HOLD_NONE) c->hold = GTA_HOLD_GAP;
        }
    }

    /* --- 7. keep to the lane ---------------------------------------------
     *
     * The car's offset WITHIN its block against a target with a dead band
     * either side; outside the band the whole car is slid sideways by the
     * band's width a tick, without being turned, until it is back inside.
     * The target is the middle of the block and nothing else - see
     * GTA_LANE_TARGET for why that deletion is the fix for the junctions.
     *
     * THE SLIDE ONLY GOES SOMEWHERE THE CAR MAY DRIVE. The original probes the
     * navigation grid beside the car and abandons the correction if the block
     * there is not road, or if its arrows do not include the car's own
     * direction. That is what stops a car nudging itself over a kerb, or out
     * of a one-way lane into the one beside it going the other way. */
    if (c->turn == 0) {
        long *p = ((c->angle & 127) == 64) ? &c->y : &c->x;
        long off = (*p >> FP) & 31;
        long d = off - c->lane_target;

        if (c->lane_fix == 0) {
            if (d > GTA_LANE_BAND || d < -GTA_LANE_BAND)
                c->lane_fix = (d > 0) ? 1 : -1;
        } else if (d <= GTA_LANE_BAND && d >= -GTA_LANE_BAND) {
            c->lane_fix = 0;
        }

        if (c->lane_fix != 0) {
            /* The slide is ALWAYS towards the middle of the block the car is
             * already in, so it cannot take the car out of its own lane and
             * the original's probe of the block beside it has nothing left to
             * refuse. What the probe is there for is the pull-over manoeuvre,
             * where the target is 7 or 0x37 rather than the middle; this port
             * has no such manoeuvre yet, and adding a test that can only
             * misfire would be worse than leaving it out. The clamp below is
             * what guarantees the car stops at the middle instead of stepping
             * past it and oscillating. */
            long step = (long)GTA_LANE_BAND << FP;
            long centre = ((((*p >> (FP + 5)) * 32) + c->lane_target) << FP);

            long was = *p;

            /* A CAR STRAIGHTENING UP AFTER A KNOCK DRIFTS BACK, NOT DARTS.
             * At the full 2 px a tick a car shunted half a lane out is back
             * on its line in a third of a second, sliding sideways with no
             * turn - the developer's "auta za szybko probuja wrocic na swoj
             * tor". While the heading is still being walked back (recover),
             * the slide runs at a quarter rate, so the way back takes about
             * as long as the straightening does. */
            if (c->recover > 0)
                step >>= 2;

            if (c->lane_fix > 0) {
                *p -= step;
                if (*p < centre) *p = centre;
            } else {
                *p += step;
                if (*p > centre) *p = centre;
            }
            /* Counted, because this sideways drag IS the reported fault - see
             * gta_traffic.stat_lane_fix. A car placed on its lane by the turn
             * needs none of it. */
            {
                long moved_px = ((was > *p) ? (was - *p) : (*p - was)) >> FP;
                tr->stat_lane_fix += moved_px;
                if (c->since_turn < GTA_AFTER_TURN) {
                    tr->stat_lane_fix_corner += moved_px;
                    if (c->turn_routed) tr->stat_corner_routed   += moved_px;
                    else                tr->stat_corner_fallback += moved_px;
                }
            }
        }
    }
}

/* Give ONE car that wants a route a destination and a path to it.
 *
 * The destination is a road block a dozen or two away, picked at random - the
 * original picks from a pool of 1020 places built at map load, and a random
 * road block is the same idea without the table. The randomness being in the
 * DESTINATION rather than in each junction is the whole point: a car then
 * drives across town instead of shuffling. */
static void route_tick(gta_traffic *tr)
{
    int scanned;

    if (!tr->nav || tr->n <= 0)
        return;

    for (scanned = 0; scanned < tr->n; scanned++) {
        gta_car *c;
        int idx = tr->route_turn % tr->n;
        int bx, by, tx, ty;

        tr->route_turn = (tr->route_turn + 1) % tr->n;
        c = &tr->cars[idx];
        if (c->route_cool > 0) {
            c->route_cool--;
            continue;
        }
        if (!c->want_route || c->done)
            continue;

        bx = (int)(c->x >> (FP + 5));
        by = (int)(c->y >> (FP + 5));
        /* THE SEARCH CHOOSES THE DESTINATION NOW - see gta_route_wander().
         *
         * It used to throw sixteen darts at a ring, take the first that landed
         * on road, and search for it; 17148 of 19448 failed searches were that
         * target turning out not to connect to where the car was standing.
         * Liberty City is a one-way system inside a 48-block window, so "is
         * road" and "can be got to" are very different questions and only the
         * search can answer the second. One BFS answers both. */
        c->path_n = gta_route_wander(tr->nav, bx, by, c->layer,
                                     (c->angle + 128) & 255,
                                     GTA_ROUTE_TARGET_LO, GTA_ROUTE_TARGET_HI,
                                     &tr->seed, c->path, GTA_ROUTE_MAX);
        tx = c->path_n > 0 ? c->path[c->path_n - 1].x : bx;
        ty = c->path_n > 0 ? c->path[c->path_n - 1].y : by;
        c->path_i = 0;
        if (c->path_n > 0) {
            int fx = c->path[0].x - bx, fy = c->path[0].y - by, fd = -1;
            if      (fx ==  0 && fy == -1) fd = 0;
            else if (fx ==  1 && fy ==  0) fd = 64;
            else if (fx ==  0 && fy ==  1) fd = 128;
            else if (fx == -1 && fy ==  0) fd = 192;
            if (fd >= 0 && fd == ((c->angle + 128) & 255)) {
                tr->routes_backward++;
                if (c->turn != 0) tr->routes_while_turning++;
            }
            tr->routes_ok++;
            tr->route_nodes += c->path_n;
        } else {
            /* AND IT DOES NOT ASK AGAIN NEXT TICK. A car with no route has no
             * route every tick, so a failure used to buy another search
             * immediately: at the north edge of the map, where the window is
             * clipped and most targets are unreachable, that was 191 failed
             * searches against 59 good ones. A second of patience costs the
             * car nothing - it is following the arrows meanwhile. */
            tr->routes_failed++;
            {
                /* AND WHY. See gta_route_last_fail(): "62% of searches fail"
                 * was blamed on GTA_ROUTE_BUDGET for a week, and tripling the
                 * budget to 2200 changed the failure count by FOUR out of
                 * nineteen thousand. A count without a cause is how that
                 * happens. */
                int r = gta_route_last_fail();
                if (r >= 0 && r < GTA_ROUTE_FAIL_KINDS)
                    tr->stat_route_fail[r]++;
                /* AND WHERE, for the one that is a real bug rather than a
                 * property of the map: a car standing on a block the nav grid
                 * reads as having no exits at all should not be there. Keep
                 * the last one so it can be looked at with `gtadump dirmap`. */
                if (r == GTA_ROUTE_NO_START) {
                    tr->nostart_x = bx;
                    tr->nostart_y = by;
                    tr->nostart_z = c->layer;
                }
            }
            c->route_cool = 50;
        }
        c->dest_x = tx;
        c->dest_y = ty;
        /* A failed search is not retried immediately - the car keeps following
         * the arrows and asks again when it next runs out, which is what stops
         * a walled-in car from searching every tick for ever. */
        c->want_route = 0;
        return;
    }
}

/* IS THIS CAR ON THE ROAD? See gta_traffic.stat_offroad for why this exists
 * and why it tests the corners rather than the centre.
 *
 * The four corners are the car's own footprint - width across, length along -
 * rotated by the heading it actually STEERS along (c->face), not the road
 * direction quadrant, because the photograph that prompted this was a lorry
 * part way through a turn. The rotation is the same fixed-point cos/sin the
 * renderer uses, so a corner computed here lands under the drawn sprite.
 *
 * Cost: four nav-grid lookups per car per tick, on a grid that is one indexed
 * byte. Twenty cars is eighty byte reads, which is nothing beside the route
 * work already in this loop - and it is off the Amiga's critical path anyway,
 * because the number only ever gets printed.
 *
 * A car on a RAMP is not off the road even though the block above it is not
 * drivable, so the test asks the car's own layer and the one below it: a car
 * driving up onto a bridge straddles two layers for a few ticks and is not
 * doing anything wrong. */
static int on_road_at(const gta_map *m, const gta_car *c, long wx, long wy)
{
    int bx = (int)(wx >> (FP + 5));
    int by = (int)(wy >> (FP + 5));

    if (drivable(m, bx, by, c->layer))
        return 1;
    if (c->layer > 0 && drivable(m, bx, by, c->layer - 1))
        return 1;
    if (c->layer + 1 < GTA_MAP_LAYERS && drivable(m, bx, by, c->layer + 1))
        return 1;
    return 0;
}

/* CAN THE PLAYER SEE THIS CAR? The original's own on-screen test.
 *
 * The original tests the car's pixel position against each view rectangle
 * EXPANDED BY THE VIEW HALF-WIDTH (`rect+0x10`, written by the camera code as
 * `halfw`), so the exclusion zone is about three screen-widths across. Every
 * destructive or discontinuous thing the game does to a car - the slot
 * recycler, the off-road teleport, the ten-block jump forward, the swap with a
 * blocking car, the wreck cleanup - is gated on this returning false.
 *
 * Ours uses the despawn radius, which is the same idea in this port's units
 * and is already the distance at which cars are retired. */
static int in_view(const gta_traffic *tr, int bx, int by)
{
    int r = gta_traffic_despawn_blocks(tr->view_blocks);
    return bx - tr->cam_bx < r && tr->cam_bx - bx < r &&
           by - tr->cam_by < r && tr->cam_by - by < r;
}

/* PUT AN OFF-ROAD CAR BACK ON THE ROAD - the original's road snap.
 *
 * The original does not leave a car on the footway and it does not delete it
 * either. Every frame it tests the car's own block - no direction bits under
 * me, no slope - and if that is true it snaps the car back: position to the
 * block centre, rotation to that block's own cardinal, wheels straightened,
 * front and rear points rebuilt. If the block it stands in has no direction
 * bits at all there is a fallback, a spiral search out to 64 rings for the
 * nearest block that has some.
 *
 * AND IT REFUSES TO DO IT IN VIEW. Both are gated on the car being outside
 * every player's view rectangle, and the snap checks again after moving,
 * restoring the old position if the new one turns out to be visible. That is
 * the whole reason this is not the "cars teleport in front of me" fault: the
 * player never sees it happen.
 *
 * So this is the backstop, not the fix. The fix is that a car should not miss
 * its turn in the first place (stat_turn_missed); this is what stops the ones
 * that still do from becoming permanent scenery. It is deliberately narrow:
 * the car's CENTRE must be off the road, it must be off screen, and there must
 * be a drivable block within GTA_RECOVER_RINGS. */
#define GTA_RECOVER_RINGS 6

static int recover_offroad(gta_traffic *tr, const gta_map *m, int idx,
                           int camx, int camy)
{
    gta_car *c = &tr->cars[idx];
    int bx = (int)(c->x >> (FP + 5));
    int by = (int)(c->y >> (FP + 5));
    int r, dx, dy;

    (void)camx; (void)camy;
    if (in_view(tr, bx, by))
        return 0;

    for (r = 1; r <= GTA_RECOVER_RINGS; r++) {
        for (dy = -r; dy <= r; dy++) {
            for (dx = -r; dx <= r; dx++) {
                int nx, ny, dir;
                if (dx > -r && dx < r && dy > -r && dy < r)
                    continue;               /* the ring only, not the disc */
                nx = bx + dx;
                ny = by + dy;
                if (!drivable(m, nx, ny, c->layer))
                    continue;
                dir = road_heading(m, nx, ny, c->layer, tr);
                if (dir < 0)
                    continue;               /* no arrows: nothing to face along */

                c->x = ((long)nx * 32 + GTA_LANE_TARGET) << FP;
                c->y = ((long)ny * 32 + GTA_LANE_TARGET) << FP;
                c->angle = dir;
                c->face  = dir;
                c->turn = 0;
                c->turn_lock = 0;
                c->speed = 0;
                c->path_n = 0;              /* the route is meaningless now */
                c->path_i = 0;
                c->want_route = 1;
                c->wait = 0;
                c->offroad = 0;
                tr->stat_offroad_recovered++;
                return 1;
            }
        }
    }
    return 0;
}

static void offroad_check(gta_traffic *tr, const gta_map *m, int idx)
{
    gta_car *c = &tr->cars[idx];
    const gta_car_info *info = &tr->tiles->cars[c->model];
    long hl = ((long)gta_car_world_len(info) / 2) << FP;
    long hw = ((long)gta_car_world_wid(info) / 2) << FP;
    int cs = gta_cos(c->face);
    int sn = gta_sin(c->face);
    int k, off = 0, deep;

    /* The four corners, as (along, across) in the car's own frame. */
    static const int sgn[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

    if (hl <= 0 || hw <= 0)
        return;

    /* The corner sample is a diagnostic and is off in the game - see
     * gta_traffic.diag_corners. The centre test below is the one that matters
     * and it is one lookup. */
    if (!tr->diag_corners)
        goto centre_only;

    for (k = 0; k < 4; k++) {
        long a = sgn[k][0] * hl;
        long b = sgn[k][1] * hw;
        /* Heading 0 is north, i.e. -y, and x is +east: the same convention
         * gta_trig.h uses and the renderer draws with. */
        long wx = c->x + (((a >> 8) * sn) >> 6) + (((b >> 8) * cs) >> 6);
        long wy = c->y - (((a >> 8) * cs) >> 6) + (((b >> 8) * sn) >> 6);
        if (!on_road_at(m, c, wx, wy)) {
            off = 1;
            break;
        }
    }

centre_only:
    /* THE EVENT COUNT TRACKS THE CENTRE, NOT THE CORNERS, and that is not a
     * detail. A bus is 40 world pixels wide in a 32-pixel lane, so its flanks
     * hang over the kerb wherever it goes - the original's do too - and an
     * event counter driven off the corners would tick every time a bus passed
     * a pavement. The corner count stays as the sensitive A/B signal; the
     * EVENT is "a car put its middle on the footway", which is the thing that
     * was photographed and the thing that should never happen. */
    deep = !on_road_at(m, c, c->x, c->y);

    if (off)
        tr->stat_offroad++;
    if (deep) {
        tr->stat_offroad_deep++;
        if (!c->offroad) {
            int why;
            tr->stat_offroad_events++;
            tr->stat_offroad_x = (int)(c->x >> (FP + 5));
            tr->stat_offroad_y = (int)(c->y >> (FP + 5));
            c->offroad_since = tr->tick;

            if (c->path_i >= c->path_n)          why = 3;
            else if (c->turn != 0)               why = 0;
            else if (is_junction(m, (int)(c->x >> (FP + 5)),
                                    (int)(c->y >> (FP + 5)), c->layer))
                                                 why = 1;
            else                                 why = 2;
            tr->stat_offroad_why[why]++;
        }
    } else if (c->offroad) {
        int len = tr->tick - c->offroad_since;
        int b = len < 25 ? 0 : len < 100 ? 1 : len < 400 ? 2 : 3;
        tr->stat_offroad_len[b]++;
    }
    c->offroad = (unsigned char)deep;
}

/* ONE COLLISION BETWEEN TWO TRAFFIC CARS.
 *
 * The same exchange the player's collisions use, applied to both sides - which
 * is what the original does: it runs the pair through the same detection and
 * the same response, and switches both cars to the rigid-body model.
 *
 * `a` is treated as the striker for the purpose of the push direction, and the
 * function is called once per ordered pair, so each car gets its turn as the
 * striker and the exchange comes out symmetric without a Newton's-third-law
 * term - which is exactly how the original arranges it too.
 */
static void fleet_collide(gta_traffic *tr, gta_car *a, gta_car *b)
{
    const gta_car_info *ai = &tr->tiles->cars[a->model];
    const gta_car_info *bi = &tr->tiles->cars[b->model];
    long dx = (b->x - a->x) >> FP, dy = (b->y - a->y) >> FP;
    long avx, avy, bvx, bvy, rvx, rvy;
    long nx, ny, depth, vrel, am, bm;
    long termx, termy, pushx, pushy, rx, ry, torque, inertia;
    int ahl = gta_car_world_len(ai) / 2, ahw = gta_car_world_wid(ai) / 2;
    int bhl = gta_car_world_len(bi) / 2, bhw = gta_car_world_wid(bi) / 2;

    /* Each car's velocity: on the rails it is its speed along its face; loose
     * it is the vector it was knocked with. */
    if (a->knock > 0) { avx = a->kvx; avy = a->kvy; }
    else {
        avx = ((long)gta_sin(a->face) >> 6) * (a->speed >> 8);
        avy = ((long)-gta_cos(a->face) >> 6) * (a->speed >> 8);
    }
    if (b->knock > 0) { bvx = b->kvx; bvy = b->kvy; }
    else {
        bvx = ((long)gta_sin(b->face) >> 6) * (b->speed >> 8);
        bvy = ((long)-gta_cos(b->face) >> 6) * (b->speed >> 8);
    }
    rvx = avx - bvx;
    rvy = avy - bvy;

    if (!box_mtv(a->x, a->y, a->face, ahl, ahw,
                 b->x, b->y, b->face, bhl, bhw, rvx, rvy, &nx, &ny, &depth))
        return;

    vrel = ((((rvx) >> 8) * nx) + (((rvy) >> 8) * ny)) >> 6;
    if (vrel <= GTA_KNOCK_HARD)
        return;                     /* touching, not colliding - a queue */

    am = ai->mass >> 16;  if (am < 1) am = 1;
    bm = bi->mass >> 16;  if (bm < 1) bm = 1;

    /* Separate them first, by inverse mass, so the pair does not grind.
     * Half the overlap beyond a pixel of slop per tick, same recipe and same
     * reason as gta_traffic_ram(): all of it at once overshoots and the pair
     * visibly jumps apart and back. */
    {
        long sep = depth > GTA_SEP_SLOP ? depth - GTA_SEP_SLOP : 0;
        long mvx, mvy;
        sep = (sep << 1) / (am + bm);
        mvx = NRAM_MUL(sep * am, nx);
        mvy = NRAM_MUL(sep * am, ny);
        /* The excess beyond the band goes to the victim alone, in full -
         * same three rules as gta_traffic_ram(). */
        if (depth > GTA_SEP_DEEP) {
            long extra = (depth - GTA_SEP_DEEP) << 2;
            mvx += NRAM_MUL(extra, nx);
            mvy += NRAM_MUL(extra, ny);
        }
        if (mvx >  NRAM_MAXPUSH) mvx =  NRAM_MAXPUSH;
        if (mvx < -NRAM_MAXPUSH) mvx = -NRAM_MAXPUSH;
        if (mvy >  NRAM_MAXPUSH) mvy =  NRAM_MAXPUSH;
        if (mvy < -NRAM_MAXPUSH) mvy = -NRAM_MAXPUSH;
        b->x += mvx;  b->y += mvy;
        a->x -= NRAM_MUL(sep * bm, nx);
        a->y -= NRAM_MUL(sep * bm, ny);
    }

    /* ONE RESPONSE PER CONTACT - the original's `car+0x230` latch. The pair
     * has been separated above; while the latch stands it pays nothing else. */
    if (b->hit_latch)
        return;

    /* The original's push: (striker's step + the vector striker->victim) times
     * the striker's mass, clamped, over 42. */
    {
        long m = am > GTA_HIT_MASS_CAP ? GTA_HIT_MASS_CAP : am;
        termx = (avx >> FP) + dx;
        termy = (avy >> FP) + dy;
        pushx = (termx * m) / GTA_HIT_SCALE;
        pushy = (termy * m) / GTA_HIT_SCALE;
    }

    /* B takes it. Half of what it already had survives, as the original does
     * for a car under 15 mass. */
    if (!b->knock) {
        b->kvx = bvx;  b->kvy = bvy;
        if (bm < GTA_HIT_LIGHT) {
            b->kvx = (b->kvx >> 8) * GTA_KNOCK_KEEP;
            b->kvy = (b->kvy >> 8) * GTA_KNOCK_KEEP;
        }
        b->face16 = (long)b->face << 16;
    }
    b->kvx += (pushx << FP) / bm;
    b->kvy += (pushy << FP) / bm;

    /* The spin, about the contact - the closest point of B's box to A's
     * centre, halved, exactly as in gta_traffic_ram(). */
    {
        long bfx = gta_sin(b->face), bfy = -gta_cos(b->face);
        long along  = ((-dx) *  bfx + (-dy) *  bfy) >> 14;
        long across = ((-dx) * -bfy + (-dy) *  bfx) >> 14;
        if (along >  bhl) along =  bhl;
        if (along < -bhl) along = -bhl;
        if (across >  bhw) across =  bhw;
        if (across < -bhw) across = -bhw;
        rx = ((along * bfx - across * bfy) >> 14) / 2;
        ry = ((along * bfy + across * bfx) >> 14) / 2;
    }
    torque = (rx * pushy - ry * pushx) * 4;     /* half-scale world - see 89 */
    inertia = bi->moment;
    if (inertia < 1) inertia = 1;
    b->komega += ((torque * GTA_HIT_SPIN) / inertia) << 6;
    if (b->komega >  GTA_KNOCK_SPIN_MAX) b->komega =  GTA_KNOCK_SPIN_MAX;
    if (b->komega < -GTA_KNOCK_SPIN_MAX) b->komega = -GTA_KNOCK_SPIN_MAX;

    if (b->speed < GTA_SPEED_UNIT)
        b->speed = 4 * GTA_SPEED_UNIT;
    b->knock = GTA_KNOCK_TICKS;
    b->hit_latch = GTA_HIT_LATCH;       /* one response per contact */
    b->hold = GTA_HOLD_NONE;

    /* And the striker is halved, as the original halves the aggressor. */
    a->speed >>= 1;

    tr->stat_knocked++;
    tr->stat_fleet_hits++;
}

/* EVERY PAIR OF TRAFFIC CARS THAT IS ACTUALLY CLOSING.
 *
 * N is GTA_MAX_CARS = 20, so the 190 ordered pairs cost a subtract and a
 * compare each before anything else happens - cheaper than the occupancy
 * rebuild this tick already does, and it runs once per tick rather than per
 * car. A spatial index (which is what the original uses, a 128x128 bucket
 * grid) would matter at a few hundred cars; at twenty it would be slower.
 */
static void fleet_collisions(gta_traffic *tr)
{
    int i, j;

    for (i = 0; i < tr->n; i++) {
        gta_car *a = &tr->cars[i];
        const gta_car_info *ai;
        int ahl, ahw;

        if (a->done || a->abandoned)
            continue;
        ai = &tr->tiles->cars[a->model];
        ahl = gta_car_world_len(ai) / 2;
        ahw = gta_car_world_wid(ai) / 2;

        for (j = i + 1; j < tr->n; j++) {
            gta_car *b = &tr->cars[j];
            const gta_car_info *bi;
            long dx, dy, lim;

            if (b->done || b->abandoned || b->layer != a->layer)
                continue;
            bi = &tr->tiles->cars[b->model];
            dx = (b->x - a->x) >> FP;
            dy = (b->y - a->y) >> FP;
            lim = (long)(ahl + ahw
                       + gta_car_world_len(bi) / 2
                       + gta_car_world_wid(bi) / 2);
            if (dx > lim || dx < -lim || dy > lim || dy < -lim)
                continue;
            fleet_collide(tr, a, b);
            fleet_collide(tr, b, a);
        }
    }
}


void gta_traffic_tick(gta_traffic *tr, const gta_map *m, long cam_x, long cam_y)
{
    int i, camx, camy;

    unsigned long pt0 = 0;

    if (!tr->tiles)
        return;

    tr->tick++;
    if (tr->prof_clock)
        pt0 = tr->prof_clock();

    /* THE SIMPLE RELEASE, exactly as specified: a booked square is given
     * back when the owner's BODY has covered it and then completely left it.
     * Length-aware, because the body test is the oriented-box test the rest
     * of the file uses - a bus frees its squares later than a saloon. No
     * timer releases ground under a standing car. The countdown survives
     * only for a booking whose owner never reached it at all (rerouted,
     * reversed away) and for one whose owner is gone. */
    /* AN ORPHANED CONVOY FIRST: if a route's tail despawned while other
     * members still ride it, the owner-gone rule below would free the
     * ground under them. The REARMOST member (farthest from the landing)
     * becomes the new tail - it is the last to leave, so it releases. */
    for (i = 0; i < tr->n; i++) {
        gta_car *mem = &tr->cars[i];
        int k, alive = 0;
        gta_car *tail2 = 0;
        long best = -1;

        if (mem->done || mem->convoy == mem->serial)
            continue;
        for (k = 0; k < tr->n; k++)
            if (!tr->cars[k].done &&
                tr->cars[k].serial == mem->convoy) {
                alive = 1;
                break;
            }
        if (alive)
            continue;
        for (k = 0; k < tr->n; k++) {
            gta_car *o2 = &tr->cars[k];
            long d;

            if (o2->done || o2->convoy != mem->convoy || o2->book_lx < 0)
                continue;
            d = (o2->x >> FP) - ((long)o2->book_lx * 32 + 16);
            if (d < 0) d = -d;
            {
                long dy2 = (o2->y >> FP) - ((long)o2->book_ly * 32 + 16);
                if (dy2 < 0) dy2 = -dy2;
                d += dy2;
            }
            if (d > best) {
                best = d;
                tail2 = o2;
            }
        }
        if (!tail2)
            continue;
        for (k = 0; k < tr->claim_top; k++)
            if (tr->claim_ttl[k] > 0 && tr->claim_car[k] == mem->convoy) {
                tr->claim_car[k] = tail2->serial;
                tr->claim_seen[k] = 0;
            }
        for (k = 0; k < tr->n; k++)
            if (!tr->cars[k].done && tr->cars[k].convoy == mem->convoy)
                tr->cars[k].convoy = tail2->serial;
    }

    {
        int top2 = 0;
    for (i = 0; i < tr->claim_top; i++) {
        int k;
        const gta_car *o = 0;

        if (tr->claim_ttl[i] <= 0)
            continue;
        for (k = 0; k < tr->n; k++)
            if (tr->cars[k].serial == tr->claim_car[i]) {
                o = &tr->cars[k];
                break;
            }
        if (!o || o->done || o->layer != (int)tr->claim_z[i]) {
            tr->claim_ttl[i] = 0;       /* owner gone - give it back */
        } else if (car_on_block(tr, o, tr->claim_x[i], tr->claim_y[i])) {
            tr->claim_seen[i] = 1;      /* the body is on it */
        } else if (tr->claim_seen[i]) {
            tr->claim_ttl[i] = 0;       /* covered it, left it - free */
        } else if (!o->crossing && !convoy_riding(tr, o->serial)) {
            /* The countdown reaps only a booking whose owner is NOT still
             * committed to the crossing (rerouted, reversed away) AND whose
             * route no other car is still riding - a convoy member inside
             * the box would otherwise lose the ground under it ten seconds
             * after the tail bows out. A committed car queued at the line
             * keeps its booking however long the queue takes - it WILL
             * come, and losing the booking under it is what let another
             * arm in across its path. */
            if (--tr->claim_ttl[i] <= 0)
                tr->claim_ttl[i] = 0;   /* abandoned - dead booking */
        }
        if (tr->claim_ttl[i] > 0)
            top2 = i + 1;
    }
        /* THE CLAIM_TOP INVARIANT: every slot at or above claim_top is dead.
         * The allocator only ever raises it (first dead slot, possibly the
         * top itself); this sweep - the one place slots die in bulk - is
         * where it comes back down. Every live-table scan in this file runs
         * to claim_top instead of GTA_CLAIM_MAX, which matters because the
         * table is 240 entries and 20 cars keep about 60 alive; measured by
         * callgrind the full-table scans were 8M instructions of a 433M
         * run before this. */
        tr->claim_top = top2;
    }

    /* WHAT THE FLEET IS DOING, counted every tick so the game itself can say
     * it. Two screenshots of the emulator a minute apart came back
     * pixel-identical while every host test reported 87% of the fleet moving,
     * and the log had nothing to say between "interactive" and the end of the
     * session. A number the running game prints is the only thing that settles
     * that kind of disagreement. */
    camx = (int)(cam_x >> (FP + 5));
    camy = (int)(cam_y >> (FP + 5));
    tr->cam_bx = camx;
    tr->cam_by = camy;

    /* THE OCCUPANCY MATRIX, from where the cars actually are, before any of
     * them moves - see GTA_OCC_MAX. */
    if (tr->prof_clock) {
        unsigned long t = tr->prof_clock();
        tr->prof_us[0] += t - pt0;
        pt0 = t;
    }
    occ_rebuild(tr, m);
    if (tr->prof_clock) {
        unsigned long t = tr->prof_clock();
        tr->prof_us[1] += t - pt0;
        pt0 = t;
    }

    tr->stat_moving = 0;
    tr->stat_stopped = 0;
    for (i = 0; i < GTA_HOLD_COUNT; i++)
        tr->stat_hold[i] = 0;

    for (i = 0; i < tr->n; i++) {
        long px = tr->cars[i].x, py = tr->cars[i].y;
        long mx, my;

        /* The collision cool-downs, run down once a tick per car. */
        if (tr->cars[i].ram_cool)
            tr->cars[i].ram_cool--;
        if (tr->cars[i].hit_latch)
            tr->cars[i].hit_latch--;

        drive_one(tr, m, i);

        offroad_check(tr, m, i);
        if (tr->cars[i].offroad)
            recover_offroad(tr, m, i, camx, camy);

        mx = tr->cars[i].x - px;
        my = tr->cars[i].y - py;
        if (mx < 0) mx = -mx;
        if (my < 0) my = -my;
        tr->stat_moved += (mx + my) >> FP;
        tr->cars[i].odo += (mx + my) >> FP;

        if (tr->cars[i].turn != 0 && tr->cars[i].speed == 0)
            tr->stat_frozen_turn++;
        if (tr->cars[i].speed > 0) {
            tr->stat_moving++;
        } else {
            int h = tr->cars[i].hold;
            tr->stat_stopped++;
            if (h >= 0 && h < GTA_HOLD_COUNT)
                tr->stat_hold[h]++;
        }
    }

    /* IS ANY CROSSING LOCKED RIGHT NOW? See gta_traffic.stat_boxlock: this is
     * the deadlock counted directly rather than inferred from flow.
     *
     * A stopped car on a junction block is asked which crossing it is in, and
     * how many other stopped cars are in the same one. Three or more is a
     * lock. The scan is over stopped junction cars only - in healthy traffic
     * that is nearly nobody, and when it is not, that is the thing being
     * measured. */
    for (i = 0; i < tr->n; i++) {
        gta_car *c = &tr->cars[i];
        int rx, ry, k, n = 1;

        /* Moving, or off the crossing, clears the timer - otherwise a car that
         * queues briefly a dozen times accumulates a "deadlock" it never had. */
        if (c->done || c->speed > 0) {
            c->boxlock_ticks = 0;
            continue;
        }
        if (!is_junction(m, (int)(c->x >> (FP + 5)),
                            (int)(c->y >> (FP + 5)), c->layer)) {
            c->boxlock_ticks = 0;
            continue;
        }
        junction_root(m, (int)(c->x >> (FP + 5)),
                         (int)(c->y >> (FP + 5)), c->layer, &rx, &ry);

        for (k = 0; k < tr->n; k++) {
            const gta_car *o = &tr->cars[k];
            int ox, oy;
            if (k == i || o->done || o->speed > 0 || o->layer != c->layer)
                continue;
            if (!is_junction(m, (int)(o->x >> (FP + 5)),
                                (int)(o->y >> (FP + 5)), o->layer))
                continue;
            junction_root(m, (int)(o->x >> (FP + 5)),
                             (int)(o->y >> (FP + 5)), o->layer, &ox, &oy);
            if (ox == rx && oy == ry)
                n++;
        }
        /* AND IT HAS TO HAVE LASTED, OR IT IS A QUEUE AND NOT A DEADLOCK.
         *
         * This counted every tick on which three cars happened to be stopped in
         * the same crossing, and that is not the fault it was built for. When
         * the corner was lengthened to match the original (the notes, "THE
         * CORNER IS SET UP A BLOCK EARLY") cars spent 60% longer inside
         * junctions, so momentary overlaps rose from 27 to 1542 car-ticks over
         * the gate - while the cars that never got out at all HALVED, from 14
         * to 6. The instrument said the change was a disaster and the thing it
         * was supposed to measure said it was a success.
         *
         * A cycle does not clear. A queue does. So a car only counts once the
         * three of them have been stuck together for GTA_BOXLOCK_HOLD ticks,
         * and then every tick after that. `stat_boxlock_worst` still records
         * the largest group seen at any instant, because how MANY cars are in
         * there is a different question from how long they stay. */
        if (n >= 3) {
            if (c->boxlock_ticks < 30000) c->boxlock_ticks++;
            if (c->boxlock_ticks > GTA_BOXLOCK_HOLD) {
                tr->stat_boxlock++;
                if (n > tr->stat_boxlock_worst) {
                    tr->stat_boxlock_worst = n;
                    tr->stat_boxlock_x = rx;
                    tr->stat_boxlock_y = ry;
                }
            }
        } else {
            c->boxlock_ticks = 0;
        }
    }

    /* ONE ROUTE SEARCH A TICK FOR THE WHOLE FLEET.
     *
     * The original refuses a second request while one is outstanding (a single
     * global "a route is being found for driver N"), and the reason is the
     * same here: a breadth-first search over a few thousand blocks is fine
     * once a frame and ruinous twenty times. Cars waiting their turn keep
     * driving on the map's arrows, so nothing stops while it waits. */
    if (tr->prof_clock) {
        unsigned long t = tr->prof_clock();
        tr->prof_us[2] += t - pt0;
        pt0 = t;
    }
    route_tick(tr);
    if (tr->prof_clock) {
        unsigned long t = tr->prof_clock();
        tr->prof_us[3] += t - pt0;
        pt0 = t;
    }

    /* CARS HIT EACH OTHER. After the whole fleet has moved, so no car sees
     * the list change under it - the same reason the retirement sweep below
     * runs here rather than inside the drive loop. */
    fleet_collisions(tr);

    /* Retire what has finished or fallen too far behind, by compacting the
     * list. Done after the whole fleet has moved, not during, so no car sees
     * the list change under it. */
    camx = (int)(cam_x >> (FP + 5));
    camy = (int)(cam_y >> (FP + 5));
    {
        int keep = 0;
        /* OUT OF SIGHT IS THE ONLY REASON TO RETIRE A CAR. The radius follows
         * the zoom - at 8 pixels a block the screen is twenty blocks either
         * side of the camera, and the old fixed 14 removed cars the player was
         * looking straight at. A car that ran out of road is no longer deleted
         * where it stands either; it waits here like any other. */
        int r = gta_traffic_despawn_blocks(tr->view_blocks);

        for (i = 0; i < tr->n; i++) {
            gta_car *c = &tr->cars[i];
            int dx = (int)(c->x >> (FP + 5)) - camx;
            int dy = (int)(c->y >> (FP + 5)) - camy;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx > r || dy > r || c->done)
                continue;
            if (keep != i)
                tr->cars[keep] = *c;
            keep++;
        }
        tr->n = keep;
    }

    /* And top the fleet back up around wherever the camera is now, so the city
     * has traffic where the player is rather than only where he started. The
     * ring starts outside the screen: a car appearing in view out of nothing
     * is worse than no car at all. */
    /* NOT every tick. The band scan walks a few hundred map blocks and this
     * runs at 50 Hz; doing it every tick would cost more than driving the
     * whole fleet does. Twice a second is far faster than cars can leave the
     * area and is invisible. */
    if (tr->spawn_wait > 0) {
        tr->spawn_wait--;
    } else if (tr->n < GTA_MAX_CARS) {
        tr->spawn_wait = GTA_TRAFFIC_SPAWN_TICKS;
        park_band(tr, m, camx, camy,
                  gta_traffic_ring_lo(tr->view_blocks),
                  gta_traffic_ring_hi(tr->view_blocks), tr->fleet_cap);
    }
    if (tr->prof_clock)
        tr->prof_us[4] += tr->prof_clock() - pt0;
}

int gta_traffic_is_junction(const gta_map *m, int bx, int by, int z)
{
    return is_junction(m, bx, by, z);
}

void gta_traffic_junction_root(const gta_map *m, int bx, int by, int z,
                               int *rx, int *ry)
{
    junction_root(m, bx, by, z, rx, ry);
}

/* THE PLAYER HITS THE CITY - one impulse per overlapping car, per tick.
 *
 * TWO MASSES, TWO ANSWERS. The first version of this computed one impulse and
 * gave it to both sides, which is only right when the two vehicles weigh the
 * same - and that is exactly the shape of the fault the developer reported:
 * "long vehicles drive over small ones, long against long works". A bus at
 * 100 mass hitting a 10-mass saloon got the saloon's share of the exchange,
 * a tenth of what it should push with, so the saloon barely moved and the bus
 * kept coming. Two buses have equal mass, so the same bug is invisible there.
 *
 * The exchange is the textbook one. For a closing speed v along the contact
 * normal and restitution e:
 *
 *     player's change  = (1+e) * v * m_car    / (m_player + m_car)
 *     car's change     = (1+e) * v * m_player / (m_player + m_car)
 *
 * so the HEAVY side barely changes speed and the light side is thrown - which
 * is what a bus doing twenty pixels a tick into a parked saloon looks like.
 *
 * AND THE OVERLAP IS UNDONE, not just resisted. An impulse changes velocity;
 * it does nothing about the pixels the two bodies are already sharing this
 * tick. Positional correction splits the MTV depth between them by the same
 * inverse-mass ratio, so a bus pushes the saloon almost the whole way out and
 * a saloon bounces off the bus instead of burying itself in it. Without this
 * a fast heavy vehicle penetrates faster than the impulse can slow it and the
 * sprites simply pass through one another.
 *
 * The player's shares come back as deltas rather than being applied here: the
 * caller owns the vehicle, and the physics acts about a centre of mass this
 * function has no business knowing about. */
int gta_traffic_ram(gta_traffic *tr, long px, long py, int pface,
                    int phl, int phw, long pvx, long pvy, long pmass,
                    int layer, long *dvx, long *dvy, long *dyaw,
                    long *dpx, long *dpy)
{
    int i, hits = 0;
    long pm = pmass >> 16;
    /* The striker's speed, for the shove gate below - the same |vx| + |vy|
     * the rest of this file uses as a cheap magnitude. */
    long pspd = (pvx < 0 ? -pvx : pvx) + (pvy < 0 ? -pvy : pvy);

    *dvx = *dvy = *dyaw = 0;
    *dpx = *dpy = 0;
    if (pm < 1) pm = 1;

    for (i = 0; i < tr->n; i++) {
        gta_car *o = &tr->cars[i];
        const gta_car_info *oi;
        long dx, dy, nx, ny, depth, vrel, om, jp, jt, sep, mvx, mvy;
        long ovx, ovy;
        int ohl, ohw;

        if (o->done || o->layer != layer)
            continue;
        dx = (o->x - px) >> FP;
        dy = (o->y - py) >> FP;
        if (dx > 96 || dx < -96 || dy > 96 || dy < -96)
            continue;
        oi = &tr->tiles->cars[o->model];
        ohl = gta_car_world_len(oi) / 2;
        ohw = gta_car_world_wid(oi) / 2;

        /* The struck car's own velocity - it is on rails, so this is its
         * speed along its face. Needed BEFORE the contact test, because the
         * direction the pair is closing is what picks the contact axis. */
        ovx = ((long)gta_sin(o->face) >> 6) * (o->speed >> 8);
        ovy = ((long)-gta_cos(o->face) >> 6) * (o->speed >> 8);

        /* The contact normal and the depth, from the boxes themselves. */
        if (!box_mtv(px, py, pface, phl, phw,
                     o->x, o->y, o->face, ohl, ohw,
                     pvx - ovx, pvy - ovy, &nx, &ny, &depth))
            continue;

        om = oi->mass >> 16;
        if (om < 1) om = 1;

        /* POSITIONAL CORRECTION FIRST, and it happens whether the pair is
         * closing or not - two bodies that are already inside each other have
         * to come apart even if they are drifting apart on their own. The
         * split is by inverse mass, so the bus moves a tenth of what the
         * saloon does. `depth` is Q14 px; NRAM_MUL takes a 16.16 magnitude
         * against a Q14 unit axis and returns 16.16.
         *
         * HOW MUCH, AND WHO PAYS IT - three rules, each one measured in
         * (the hitcar REVERSALS counter is the developer's "teleportuje
         * w te i we wte" as a number):
         *
         * 1. HALF the overlap beyond a pixel of slop, per tick. It was 100
         *    percent with no slop, which is the textbook oscillator: undo
         *    everything, the AI closes the gap, undo everything again.
         * 2. EACH SIDE MOVES BY THE OTHER'S MASS SHARE. This was backwards
         *    - the victim moved by its OWN mass, so a bus ramming a saloon
         *    took 91 percent of the correction itself and was seen thrown
         *    back. fleet_collide() always had it the right way round.
         * 3. The EXCESS beyond GTA_SEP_DEEP goes to the VICTIM ALONE, in
         *    full. A striker stepping 16 px in one tick opens more overlap
         *    than a 50-percent split can clear (ramsweep read worst 13 px)
         *    - but paying any of that excess on the striker's side is a
         *    backwards jump of the player's own car, which is the jerk
         *    this whole exercise removes. Shoving the excess into the
         *    victim matches the original, whose response is a force on the
         *    victim and never a displacement of the striker. */
        sep = depth > GTA_SEP_SLOP ? depth - GTA_SEP_SLOP : 0;
        sep = RCHK(sep << 1, "sep depth") / (pm + om);
        mvx = NRAM_MUL(RCHK(sep * pm, "sep*pm"), nx);
        mvy = NRAM_MUL(sep * pm, ny);
        if (depth > GTA_SEP_DEEP) {
            long extra = (depth - GTA_SEP_DEEP) << 2;   /* Q14 -> 16.16 */
            mvx += NRAM_MUL(extra, nx);
            mvy += NRAM_MUL(extra, ny);
        }
        if (mvx > NRAM_MAXPUSH)  mvx = NRAM_MAXPUSH;
        if (mvx < -NRAM_MAXPUSH) mvx = -NRAM_MAXPUSH;
        if (mvy > NRAM_MAXPUSH)  mvy = NRAM_MAXPUSH;
        if (mvy < -NRAM_MAXPUSH) mvy = -NRAM_MAXPUSH;
        o->x += mvx;
        o->y += mvy;
        *dpx -= NRAM_MUL(RCHK(sep * om, "sep*om"), nx);
        *dpy -= NRAM_MUL(sep * om, ny);

        /* Closing speed along that normal - the player's velocity less the
         * car's own along its face. Only a CLOSING pair exchanges an impulse;
         * one already separating has had its overlap undone above and needs
         * nothing else. */
        vrel = RCHK((((pvx - ovx) >> 8) * nx) + (((pvy - ovy) >> 8) * ny),
                    "vrel") >> 6;
        if (vrel <= 0)
            continue;

        /* ONE RESPONSE PER CONTACT - the original's `car+0x230` latch
         * - a car whose response is latched refuses a second one until the
         * physics step re-arms it. While it is up the pair has been
         * separated above and pays nothing else: no impulse, no spin, no
         * shove, no halving of the striker. Without this every
         * tick the boxes still overlapped paid the whole response again -
         * hitcar read ONE bus-into-saloon contact as thirteen knocks, and
         * the striker's velocity, halved per tick, is the "my car
         * teleports" jerk. */
        if (o->hit_latch)
            continue;

        /* e ~ 0.25, and the two shares differ by the mass ratio. */
        jp = RCHK(((vrel + (vrel >> 2)) / (pm + om)) * om, "jp");
        jt = RCHK(((vrel + (vrel >> 2)) / (pm + om)) * pm, "jt");

        *dvx -= NRAM_MUL(jp, nx);
        *dvy -= NRAM_MUL(jp, ny);
        {
            long fx = gta_sin(pface), fy = -gta_cos(pface);
            long along = ((dx << 8) * (fx >> 6) + (dy << 8) * (fy >> 6)) >> 8;
            long side  = ((dx << 8) * (-fy >> 6) + (dy << 8) * (fx >> 6)) >> 8;
            /* hit ahead on the right shoves the nose left, and so on */
            if (along > 0) *dyaw -= (side > 0 ? jp : -jp) >> 4;
            else           *dyaw += (side > 0 ? jp : -jp) >> 4;
        }

        /* THE VICTIM'S SHARE IS A VELOCITY, NOT A NUDGE.
         *
         * It used to be paid as displacement - the distance that velocity
         * would have covered in one tick - and then the car's speed was cut to
         * a crawl. Both halves were wrong and they hid each other. Measured
         * before this change, with `gtadump hitcar`: a bus at 20 px/tick into a
         * stationary saloon moved it 14 px along, 0 px sideways, 0 degrees,
         * and its top speed for the next fifteen ticks was 0.41 px/tick. The
         * mass made no difference at all - a saloon and a 100-mass bus pushed
         * it the same distance - because the displacement was clamped to
         * NRAM_MAXPUSH long before the mass ratio could matter.
         *
         * A hit hard enough to be worth calling one now knocks the car LOOSE:
         * it gets kvx/kvy from the same impulse the player gets, a spin from
         * where along its body the hit landed, and GTA_KNOCK_TICKS of driving
         * itself before the AI takes it back. A gentle nudge still just shoves
         * it, because a car being leant on should not spin away.
         *
         * The spin: the moment arm is how far off centre the contact is,
         * across the victim's own body. A hit square on the nose gives zero
         * and the car is driven straight back; a corner gives the full arm and
         * the car slews. Divided by the body length because a long vehicle
         * turns less for the same shove - a cheap stand-in for a moment of
         * inertia, and honest about being one. */
        if (vrel > GTA_KNOCK_HARD) {
            /* THE ORIGINAL'S OWN FORMULA.
             *
             * There is no contact normal in GTA 1's car-vs-car path and no
             * restitution; the 0.625 that exists is for walls and for one
             * other class. What there is, is a FORCE applied for one tick,
             * pointing along "where I was going, plus away from me", scaled by
             * the striker's mass with a hard clamp, and divided down by 42.
             *
             *   m     = min(striker mass, 20)
             *   term  = striker's step this tick + the vector striker->victim
             *   push  = term * m / 42
             *
             * The victim then integrates it as any body would: v += F/mass,
             * omega += (r x F)/inertia, where r is from the victim's centre to
             * the contact point - which the original takes as the MIDPOINT of
             * the victim's centre and the penetrating corner.
             *
             * All three numbers - the 1/42, the clamp of 20.0 and the 0.5
             * midpoint - are the original's own constants. */
            long am = pm > GTA_HIT_MASS_CAP ? GTA_HIT_MASS_CAP : pm;
            long stepx = pvx >> FP, stepy = pvy >> FP;      /* px this tick */
            long termx = stepx + dx, termy = stepy + dy;    /* dx,dy: me->it */
            long pushx = (termx * am) / GTA_HIT_SCALE;      /* px of force   */
            long pushy = (termy * am) / GTA_HIT_SCALE;
            long rx, ry, torque, inertia;
            int olen = gta_car_world_len(oi);

            /* HALF OF WHAT IT WAS ALREADY DOING. The original's own line on
             * the car that was hit: `if (mass < 15) { v.x *= 0.5; v.y *= 0.5 }`
             * - so a car rammed from behind while driving keeps half of what it
             * had rather than starting again from rest. */
            if (!o->knock) {
                long sfx = gta_sin(o->face), sfy = -gta_cos(o->face);
                o->kvx = ((sfx >> 6) * (o->speed >> 8) >> 8);
                o->kvy = ((sfy >> 6) * (o->speed >> 8) >> 8);
                if (om < GTA_HIT_LIGHT) {
                    o->kvx = (o->kvx >> 8) * GTA_KNOCK_KEEP;
                    o->kvy = (o->kvy >> 8) * GTA_KNOCK_KEEP;
                }
            }

            /* v += F / mass. push is in whole px, so raise it to 16.16. */
            o->kvx += (pushx << FP) / om;
            o->kvy += (pushy << FP) / om;

            /* THE LEVER ARM, AND THIS IS WHERE THE SPIN COMES FROM.
             *
             * The original takes the contact as the midpoint of the victim's
             * centre and the PENETRATING CORNER - the corner of one box found
             * inside the other - and only falls back to the midpoint of the
             * two centres when no corner is inside. This port had only the
             * fallback, which puts `r` along the line of travel: `r x F` then
             * comes out near zero and the car is shoved bodily instead of
             * slewing. Reported as "the whole vehicle slides away; in the
             * original it turned when you hit it on a corner". */
            {
                /* WHERE THE TWO BODIES MEET: the closest point of the victim's
                 * box to the striker's centre. The striker's centre in the
                 * victim's frame, clamped to the victim's half-extents.
                 *
                 * Corner-inside-box - which is what the original tests - is
                 * unstable here: at shallow penetration whether one corner is
                 * in or two, and which, flips with a pixel of approach, and
                 * the torque jumped from +40 to -15 degrees across four pixels
                 * of aim. This is continuous: dead-centre gives a point on the
                 * axis and no torque, and the arm grows smoothly out to the
                 * corner as the hit moves out. */
                long ofx = gta_sin(o->face), ofy = -gta_cos(o->face);
                long along  = ((-dx) *  ofx + (-dy) *  ofy) >> 14;
                long across = ((-dx) * -ofy + (-dy) *  ofx) >> 14;

                if (along >  ohl) along =  ohl;
                if (along < -ohl) along = -ohl;
                if (across >  ohw) across =  ohw;
                if (across < -ohw) across = -ohw;

                /* Back to world, and halved - the original's contact is the
                 * MIDPOINT of the victim's centre and the contact. */
                rx = ((along * ofx - across * ofy) >> 14) / 2;
                ry = ((along * ofy + across * ofx) >> 14) / 2;
            }
            /* r x F, and TIMES FOUR for the half-scale world: this port
             * draws 32 world pixels to a block where the original draws 64,
             * so r and F are each halved and their product is quartered -
             * while the inertia below comes out of the car table in the
             * original's own units. The torque is the only quadratic term in
             * the whole collision, so it is the only one that needs this. */
            torque = (rx * pushy - ry * pushx) * 4;

            /* INERTIA COMES OUT OF THE CAR TABLE. The original loads the
             * rigid body's inertia from the car model and its mass from the
             * next field; this port's reader has `moment` and `mass` as the
             * same pair, same order, four bytes apart. So `moment`
             * is that number, 1360 for model 0 against a rod estimate of 800.
             *
             * GTA_HIT_SPIN converts the original's radians into this port's
             * 16.16 of a 256-step circle: 256/(2*pi) = 40.743 steps a radian
             * = 2670177 in 16.16, split as 41721 << 6 so the multiply stays
             * inside 32 bits on a 68020. */
            (void)olen;
            inertia = oi->moment;
            if (inertia < 1) inertia = 1;
            o->komega += ((torque * GTA_HIT_SPIN) / inertia) << 6;
            if (o->komega >  GTA_KNOCK_SPIN_MAX) o->komega =  GTA_KNOCK_SPIN_MAX;
            if (o->komega < -GTA_KNOCK_SPIN_MAX) o->komega = -GTA_KNOCK_SPIN_MAX;

            /* A STATIONARY VICTIM IS SHOVED TO SPEED 4 - but only when the
             * striker was really moving. The original gates this on
             * `(|self.speed| >> 1) > 6`, i.e. a striker doing more than
             * twelve of its own speed units, and this port had no gate at
             * all: every tap, from a motorbike or a tanker alike, set the
             * same flat 4. Since 4 units is 1.2 px/tick and a light car's
             * impulse is less than that, the flat value replaced the impulse
             * and every light vehicle shoved exactly as hard as every other.
             *
             * It also writes the RAILS speed, which is what the car drives at
             * once the AI has it back - not the velocity it is loose with.
             * knock_step() sets that from the velocity the car ends up with,
             * so this only has to stop a hard-nudged car sitting still. */
            if (o->speed < GTA_SPEED_UNIT &&
                pspd > GTA_HIT_SHOVE_MIN * GTA_SPEED_UNIT)
                o->speed = 4 * GTA_SPEED_UNIT;

            if (!o->knock)
                o->face16 = (long)o->face << 16;
            o->knock = GTA_KNOCK_TICKS;
            o->hit_latch = GTA_HIT_LATCH;   /* one response per contact */
            tr->stat_knocked++;

            /* AND THE AGGRESSOR IS HALVED - the original halves the striker's
             * own speed on the same tick, ONCE, behind the same latch.
             * Returned as a velocity delta here because the caller owns the
             * car. */
            *dvx -= pvx >> 1;
            *dvy -= pvy >> 1;
        } else {
            /* Not a crash - a lean. Push it out of the way and leave it be. */
            long ox_ = NRAM_MUL(jt, nx), oy_ = NRAM_MUL(jt, ny);
            if (ox_ > NRAM_MAXPUSH)  ox_ = NRAM_MAXPUSH;
            if (ox_ < -NRAM_MAXPUSH) ox_ = -NRAM_MAXPUSH;
            if (oy_ > NRAM_MAXPUSH)  oy_ = NRAM_MAXPUSH;
            if (oy_ < -NRAM_MAXPUSH) oy_ = -NRAM_MAXPUSH;
            o->x += ox_;
            o->y += oy_;
            if (o->speed > GTA_SPEED_UNIT)
                o->speed = GTA_SPEED_UNIT;
        }

        /* AND ONLY A REAL IMPACT COSTS BODYWORK.
         *
         * A player leaning on a parked car with the throttle down is a
         * CLOSING pair every single tick - the test above is satisfied
         * forever - so charging damage per tick made the counter climb
         * without limit: twelve points in twelve ticks, the car's engine
         * derated to nothing, from resting against a bumper. The original
         * does not have this because its collision fires ONCE, through a
         * latch - set to 2 and cleared when the impulse is applied - and
         * charges a flat 20 points.
         *
         * Two gates stand in for that latch here. The closing speed has to
         * be worth calling a crash, and the same pair cannot be charged
         * again for half a second. The HARD impulse has its own latch now
         * (hit_latch above, the original's 0x230); the LEAN is deliberately
         * ungated - a car being leant on is pushed every tick, which is what
         * stops the pair interpenetrating, and it costs no bodywork. */
        if (vrel > GTA_RAM_HARD && o->ram_cool == 0) {
            o->damage += (int)(jp >> 14) + 1;
            o->ram_cool = GTA_RAM_COOL;
            hits++;
            tr->stat_rams++;
        }
    }
    return hits;
}

int gta_traffic_abandon(gta_traffic *tr, int model, long x, long y, int face,
                        int layer, int remap, int damage)
{
    gta_car *c;
    int slot = -1;

    /* ROOM IS MADE, NOT HOPED FOR.
     *
     * The first version simply refused when the fleet was full, and the fleet
     * IS full: the spawner tops it back up the moment the player takes a car
     * out of it, so by the time he gets out again there is no slot left. The
     * log said "fleet full, car lost" and the car vanished exactly as before -
     * the same bug, one step further along.
     *
     * A car the player parked is worth more than an ambient one he has never
     * looked at, so if there is no free slot the FARTHEST ambient car gives
     * up its own. It is off-screen by construction (the fleet only reaches a
     * few blocks past the view) and the spawner will make another. Another
     * abandoned car is never taken - those are all the player's. */
    if (tr->n < GTA_MAX_CARS) {
        slot = tr->n++;
    } else {
        long worst = -1;
        int i;
        for (i = 0; i < tr->n; i++) {
            long dx, dy, d;
            if (tr->cars[i].abandoned)
                continue;
            dx = tr->cars[i].x - x; if (dx < 0) dx = -dx;
            dy = tr->cars[i].y - y; if (dy < 0) dy = -dy;
            d = dx > dy ? dx : dy;
            if (d > worst) { worst = d; slot = i; }
        }
        if (slot < 0)
            return 0;                   /* every slot is somebody's parked car */
    }
    c = &tr->cars[slot];
    memset(c, 0, sizeof *c);
    c->model = model;
    c->x = x;
    c->y = y;
    c->face = face;
    c->angle = face;
    c->layer = layer;
    c->speed = 0;
    c->remap = remap;
    c->damage = damage;
    c->abandoned = 1;
    c->done = 0;
    c->serial = ++tr->next_serial;
    return 1;
}

void gta_traffic_set_player(gta_traffic *tr, int active, long x, long y,
                            long speed, int face, int layer, int hl, int hw)
{
    tr->pl_active = active;
    tr->pl_x = x; tr->pl_y = y;
    tr->pl_speed = speed;
    tr->pl_face = face;
    tr->pl_layer = layer;
    tr->pl_hl = hl; tr->pl_hw = hw;
}

int gta_traffic_grab_car(gta_traffic *tr, long x, long y, int layer,
                         int range, int *model, long *cx, long *cy,
                         int *face, int *remap, int *damage, int *had_driver)
{
    int i, bi = -1;
    long bd = ((long)range << FP);

    for (i = 0; i < tr->n; i++) {
        gta_car *c = &tr->cars[i];
        long dx, dy, d;

        if (c->done || c->layer != layer)
            continue;
        dx = c->x - x; if (dx < 0) dx = -dx;
        dy = c->y - y; if (dy < 0) dy = -dy;
        d = dx > dy ? dx : dy;
        if (d < bd) { bd = d; bi = i; }
    }
    if (bi < 0)
        return 0;
    *model = tr->cars[bi].model;
    *cx = tr->cars[bi].x;
    *cy = tr->cars[bi].y;
    *face = tr->cars[bi].face;
    /* Carried across so the car the player gets out of is the same car he got
     * into - same paint, same dents. An abandoned car has nobody to throw out
     * of it; a moving one does, and that is the carjacking. */
    *remap = tr->cars[bi].remap;
    *damage = tr->cars[bi].damage;
    *had_driver = !tr->cars[bi].abandoned;
    /* Out of the fleet: the tick compacts it and the release sweep frees
     * every square it held - the same path a despawn takes. */
    tr->cars[bi].done = 1;
    return 1;
}

void gta_traffic_draw(gta_traffic *tr, gta_view *v)
{
    int i;
    for (i = 0; i < tr->n; i++) {
        const gta_car *c = &tr->cars[i];
        const gta_car_info *info = &tr->tiles->cars[c->model];
        if (info->sprite_index < 0)
            continue;
        /* THE CAR'S OWN COLOUR. `remap` is an index into this model's twelve
         * `remap8` bytes, which are themselves indices into the style file's
         * remap tables - so a saloon comes in as many colours as its record
         * lists, and -1 means the sprite's own paint. The field has existed
         * since traffic did; nothing could apply it until the renderer could
         * remap a sprite. */
        gta_render_add_sprite_r(v, c->x, c->y, c->layer, c->layer,
                              info->sprite_index, gta_car_draw_angle(c),
                              c->remap >= 0 && c->remap < GTA_CAR_REMAPS
                                  ? (int)info->remap8[c->remap] : 0);
    }
}
