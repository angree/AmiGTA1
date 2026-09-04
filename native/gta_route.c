/* The route finder. Read gta_route.h first - it says why traffic needs one at
 * all, and why this is a breadth-first search rather than anything cleverer.
 *
 * Licence: MIT (ours).
 */
#include <string.h>

#include "gta_route.h"

#define W GTA_ROUTE_WINDOW

static int g_allow_reverse;     /* see gta_route_find_chase */

/* The scratch, at file scope on purpose.
 *
 * 4 KB of visited flags and 8 KB of queue is more than belongs on the Amiga's
 * stack, which is a fixed allocation made in the startup code rather than
 * something that grows. It is also the same memory for every car, which is
 * only safe because one search runs at a time - see the header. */
static unsigned char came_from[W * W];   /* 0 = unvisited, else 1 + direction */
static unsigned short queue[W * W];

/* The four directions, in the map's bit order: N, S, W, E. */
static const signed char step_x[4] = {  0,  0, -1,  1 };
static const signed char step_y[4] = { -1,  1,  0,  0 };
static const unsigned char dir_bit[4] = { GTA_NAV_N, GTA_NAV_S,
                                          GTA_NAV_W, GTA_NAV_E };
/* ...and each one's opposite, in the same order. */
static const signed char opposite[4] = { 1, 0, 3, 2 };

/* Why the last search came back empty - see gta_route.h. One int, set on every
 * path out of the function, read by the caller straight afterwards; there is
 * one search per tick for the whole fleet so there is nothing to race with. */
static int g_last_fail = GTA_ROUTE_OK;

int gta_route_last_fail(void)
{
    return g_last_fail;
}

static int route_bfs(const gta_nav *nav, int sx, int sy, int z,
                     int tx, int ty, int ban, gta_route_node *out, int max,
                     int wander, int wmin, int wmax, unsigned long *seed)
{
    int ox = sx - W / 2, oy = sy - W / 2;      /* window origin */
    int head = 0, tail = 0, i, n;
    int cx, cy, gx, gy;
    int order[4], oi;               /* neighbour scan order, straight first */
    int back2;                      /* reverse of the parent arrival */
    const unsigned char *layer;
    int lox, loy, hix, hiy;                    /* the window, clipped */
    int ban_i = -1;                            /* the step index `ban` names */
    int wpx = -1, wpy = -1;                    /* the wandering pick, if any */
    long wseen = 0;                            /* how many were eligible */

    g_last_fail = GTA_ROUTE_NO_START;
    if (max <= 0 || nav->b == 0) return 0;
    if (gta_nav_dirs(gta_nav_at(nav, sx, sy, z)) == 0) return 0;
    g_last_fail = GTA_ROUTE_NO_TARGET;
    if (!wander) {
    if (gta_nav_dirs(gta_nav_at(nav, tx, ty, z)) == 0) return 0;
    g_last_fail = GTA_ROUTE_FAR;
    if (tx - ox < 0 || tx - ox >= W || ty - oy < 0 || ty - oy >= W) return 0;
    g_last_fail = GTA_ROUTE_SAME;
    if (sx == tx && sy == ty) return 0;
    }

    /* ONE POINTER AND NO BOUNDS CHECKS IN THE LOOP.
     *
     * The first version called gta_nav_at() four times per cell, and each call
     * checks three ranges before it indexes. On the 68020 that made a search
     * cost ELEVEN MILLISECONDS - half a frame - which the whole design cannot
     * afford however rarely it runs. The window is clipped to the map once,
     * here, and the loop then indexes the layer directly. */
    layer = nav->b + ((long)z << 16);
    lox = ox < 0 ? 0 : ox;
    loy = oy < 0 ? 0 : oy;
    hix = ox + W;  if (hix > GTA_MAP_DIM) hix = GTA_MAP_DIM;
    hiy = oy + W;  if (hiy > GTA_MAP_DIM) hiy = GTA_MAP_DIM;

    /* step_x/step_y are in the map's bit order N, S, W, E; `ban` is a heading
     * in our 256ths. Turned into an index here, once, rather than compared
     * inside the loop. */
    switch (ban) {
    case 0:   ban_i = 0; break;
    case 128: ban_i = 1; break;
    case 192: ban_i = 2; break;
    case 64:  ban_i = 3; break;
    default:  ban_i = -1; break;
    }

    memset(came_from, 0, sizeof came_from);
    came_from[(sy - oy) * W + (sx - ox)] = 0xff;    /* the start, no parent */
    /* The queue holds WINDOW coordinates packed as (y << 8) | x rather than a
     * cell index, because unpacking an index needs a divide and the 68020's
     * divide is forty-odd cycles - times two thousand cells. */
    queue[tail++] = (unsigned short)(((sy - oy) << 8) | (sx - ox));

    while (head < tail) {
        int cell = queue[head++];
        unsigned char here;

        /* AND A BUDGET. A search that has looked at this many blocks is
         * either going somewhere unreachable or somewhere too far, and both
         * are better answered with "no route" - the car keeps following the
         * arrows and asks again later - than with a frame-long pause. */
        if (head > GTA_ROUTE_BUDGET) {
            if (wander && wpx >= 0) { tx = wpx; ty = wpy; goto found; }
            g_last_fail = GTA_ROUTE_BUDGET_HIT;
            return 0;
        }

        cx = ox + (cell & 0xff);
        cy = oy + (cell >> 8);
        here = layer[(cy << 8) | cx];

        /* AND THE ROUTE MAY NOT DOUBLE BACK, which is a different rule from
         * the start-cell ban below and fixes a different bug.
         *
         * The developer's report was that a car "comes out of a side road,
         * turns left onto the main road and immediately turns left back", and
         * that GTA 1 never did this - a turn there is ninety degrees at most.
         * It was measured (`gtadump uturns`) at fourteen reversals a minute
         * around one junction, and nearly every one of them happened while the
         * car was FOLLOWING A ROUTE. The routes themselves contained the
         * U-turn:
         *
         *      (80,34) (80,35) (80,36) (80,37) (80,38) (81,38) (81,37) (81,36)
         *       south ------------------------> cross <----------------- north
         *
         * which is a perfectly legal path: Liberty City's avenues are two
         * lanes each way and each lane is its own block, so hopping across at
         * the end of the street and coming back up the other carriageway IS
         * the shortest way to a destination behind the car. The search was
         * right; the question was wrong.
         *
         * `ban` cannot catch it - it only refuses the FIRST step, and the
         * doubling back happens two or three blocks later. So the pattern
         * itself is refused: a step is rejected when it travels opposite to
         * the direction the path was travelling TWO steps ago, which is
         * exactly "direction D, one step across, direction not-D". Going right
         * round a block (D, across, across, not-D) still passes, because with
         * two blocks in between that is a detour and not a reversal.
         *
         * The cost is one extra array read per expanded cell - the arrival
         * direction of the parent, reached by stepping back from this cell -
         * and no extra memory. Making the search state (cell, direction)
         * instead would be the textbook answer and costs four times the
         * memory and four times the work, on a machine that has neither. */
        {
            /* Step back twice: once to the parent, once more to read the
             * direction the parent was ENTERED by. That direction's reverse is
             * what must not be taken from here.
             *
             * A THIRD STEP WAS TRIED AND COST MORE THAN IT BOUGHT, so it is
             * not here. Liberty City also has two-block crossovers, and a
             * route like
             *
             *      (248,124)..(248,127) south, (249,127) (250,127) across,
             *      then (250,126)..(250,119) north
             *
             * slips through a two-step rule because the way back up starts
             * three steps after the way down ended. Banning that as well does
             * catch it - reversals at that site fell from 11 to 2 - but it
             * makes the SEARCH FAIL far more often (139 good against 264
             * failed, where it had been 156 against 141), and a car with no
             * route falls back to following the arrows, which is the worse
             * driver. City-wide over 96 sites: overlaps 10 -> 55, flow
             * 88% -> 87%, longest stood 27.2 s -> 32.1 s, to remove 42
             * reversals out of 459. A local win and a general loss.
             *
             * AND NEITHER IS THE LANE CHANGE BANNED HERE, though it is the
             * same shape and the reasoning is seductive. The pattern is "D,
             * across, D" - go north, step one block sideways, carry on north -
             * which on a dual carriageway IS a lane change, and BFS likes it
             * because both lanes reach the same places so it takes whichever
             * it enqueued first. The junction-line instrument found cars doing
             * exactly that in the middle of crossings, and 243 of the 286 were
             * following a route.
             *
             * Refusing it here, scoped to sidesteps that leave a junction
             * block, works and is cheap: lane changes 286 -> 113 of ~5350
             * crossings, U-turns 3987 against 4023, and the worst merge corner
             * (132,36) fell from 55 overlaps to 10. It was still dropped,
             * because of what it does to the two thin parts of the graph. At
             * (64,64) - the flattest, most reliable site in the gate, 95% flow
             * and no car ever lost on any seed - it gave 89/94/87/90% and NINE
             * cars abandoned over four seeds. At (204,108) it stranded cars on
             * seeds that had been clean. Two softenings were measured and both
             * failed: retrying without the rule when the search fails fires on
             * two searches in three (that is the failure rate here) and puts
             * the lane changes straight back, 113 -> 206; allowing the jog
             * only where the junction could not have gone straight on changed
             * nothing at all.
             *
             * The reason it costs so much is worth keeping. A refused route is
             * not a detour, it is NO ROUTE - the car falls back to following
             * the arrows, which is the worse driver - and this search already
             * fails 62% of the time, so every extra ban lands on a graph that
             * is barely connected as it is. The lane change was fixed in the
             * DRIVER instead, where refusing it costs a car nothing: see
             * `turn_lock` in gta_traffic.c, which took it to 46 crossings out
             * of ~5100 with (64,64) left at 96%. Route rules are expensive;
             * driver rules are not. */
            int px = cx, py = cy, k;
            back2 = -1;
            for (k = 0; k < 2; k++) {
                int a = (int)came_from[(py - oy) * W + (px - ox)] - 1;
                if (a < 0 || a > 3) break;      /* the start cell, or no parent */
                px -= step_x[a];                /* step back to the parent */
                py -= step_y[a];
                if (px < lox || px >= hix || py < loy || py >= hiy) break;
                /* k == 0 is the step that entered THIS cell; going back that
                 * way lands on a cell already visited, so it needs no ban. */
                if (k == 1) back2 = opposite[a];
            }
        }

        /* STRAIGHT ON FIRST. BFS resolves equal-length ties by whichever
         * neighbour is enqueued first, and a fixed N,S,W,E order turns every
         * diagonal into a staircase - W,S,W,S,W,S - which makes a car turn at
         * EVERY block and is the junction wobble the developer kept reporting
         * (proved with `gtadump turntrace`: the wobbling car was on a route).
         * Trying the arrival direction first gathers the turns into corners:
         * the same length path, driven as two straights and one turn. */
        {
            int arrival = (int)came_from[(cy - oy) * W + (cx - ox)] - 1;
            int k;
            /* Rotating the three non-straight directions per search was tried
             * as a cure for the kerb-lane bias below - a dual carriageway
             * offers two paths of exactly equal length and BFS hands the tie
             * to whichever is enqueued first. It is not the cause: the split
             * moved from 24% to 22% in the left lane. Left in the record so
             * the next reader does not spend the afternoon on it. */
            if (arrival >= 0 && arrival < 4) {
                order[0] = arrival;
                for (i = 0, k = 1; i < 4; i++)
                    if (i != arrival) order[k++] = i;
            } else {
                for (i = 0; i < 4; i++) order[i] = i;
            }
        }

        for (oi = 0; oi < 4; oi++) {
            int nx, ny, ncell;
            i = order[oi];

            /* THE ARROWS ARE THE EDGES. A block's direction bits say which
             * ways traffic may LEAVE it, so this is a directed graph and a
             * route down a one-way street the wrong way is not reachable -
             * which is exactly the rule the map is written to express. */
            if ((gta_nav_dirs(here) & dir_bit[i]) == 0)
                continue;

            /* AND NOT STRAIGHT BACK OUT OF THE START BLOCK. See the header:
             * this is the whole of the U-turn fix, and it costs one comparison
             * on the first cell of a search. */
            if (i == ban_i && cx == sx && cy == sy)
                continue;

            /* AND NOT BACK THE WAY THE PATH WAS GOING TWO STEPS AGO - see the
             * note above the scan order: this is the whole of the U-turn fix
             * for routes that reverse across a dual carriageway. */
            /* THE CHASE MAY DOUBLE BACK. The rule above keeps traffic from
             * hopping across a dual carriageway and coming straight back,
             * which is right for a car going about its business and wrong
             * for a police car whose target is on the other carriageway: a
             * cop on the inner southbound lane could not route to the
             * northbound lane two blocks over at all ("exhausted"), while
             * one lane further out could, because its loop had two blocks
             * across. gta_route_find_chase() lifts the rule for one search. */
            if (i == back2 && !g_allow_reverse)
                continue;

            nx = cx + step_x[i];
            ny = cy + step_y[i];
            if (nx < lox || nx >= hix || ny < loy || ny >= hiy)
                continue;
            if ((layer[(ny << 8) | nx] & 0x0f) == 0)
                continue;

            ncell = (ny - oy) * W + (nx - ox);
            if (came_from[ncell] != 0)
                continue;
            came_from[ncell] = (unsigned char)(1 + i);
            queue[tail++] = (unsigned short)(((ny - oy) << 8) | (nx - ox));

            if (wander) {
                /* PICK THE TARGET OUT OF WHAT IS REACHABLE, rather than
                 * picking one first and discovering it is not. Reservoir
                 * sampling: every eligible cell has an equal chance and
                 * nothing has to be stored but the current pick. */
                int md = nx - sx, nd = ny - sy;
                if (md < 0) md = -md;
                if (nd < 0) nd = -nd;
                md += nd;
                if (md >= wmin && md <= wmax) {
                    wseen++;
                    *seed = *seed * 1664525UL + 1013904223UL;
                    if ((*seed >> 16) % (unsigned long)wseen == 0) {
                        wpx = nx;
                        wpy = ny;
                    }
                }
            } else if (nx == tx && ny == ty)
                goto found;
        }
    }
    if (wander && wpx >= 0) {
        /* THE SEARCH IS THE TARGET PICKER. Everything reachable has been seen
         * and one of the eligible cells was chosen as it went past, so this
         * cannot come back "no route" the way asking for a guessed target
         * could - and that was 17148 of 19448 failures. */
        tx = wpx;
        ty = wpy;
        goto found;
    }
    g_last_fail = GTA_ROUTE_EXHAUSTED;
    return 0;

found:
    g_last_fail = GTA_ROUTE_OK;
    /* Walk back to the start counting, then fill the array forwards - the
     * caller wants the route in the order it will be driven and a reverse pass
     * over a 40-entry array is cheaper than any cleverness. */
    n = 0;
    gx = tx; gy = ty;
    while (!(gx == sx && gy == sy)) {
        int d = came_from[(gy - oy) * W + (gx - ox)] - 1;
        if (d < 0 || d > 3) return 0;           /* cannot happen; not trusted */
        n++;
        gx -= step_x[d];
        gy -= step_y[d];
        if (n > W * W) return 0;
    }
    if (n > max) {
        /* Truncated: give the car the FIRST `max` blocks of the route, not the
         * last. It drives them and asks again. */
        int skip = n - max;
        n = max;
        gx = tx; gy = ty;
        while (skip-- > 0) {
            int d = came_from[(gy - oy) * W + (gx - ox)] - 1;
            gx -= step_x[d];
            gy -= step_y[d];
        }
    } else {
        gx = tx; gy = ty;
    }
    for (i = n - 1; i >= 0; i--) {
        int d;
        out[i].x = (unsigned char)gx;
        out[i].y = (unsigned char)gy;
        d = came_from[(gy - oy) * W + (gx - ox)] - 1;
        if (d < 0 || d > 3) break;
        gx -= step_x[d];
        gy -= step_y[d];
    }
    return n;
}

int gta_route_find(const gta_nav *nav, int sx, int sy, int z,
                   int tx, int ty, int ban, gta_route_node *out, int max)
{
    return route_bfs(nav, sx, sy, z, tx, ty, ban, out, max, 0, 0, 0, 0);
}

int gta_route_find_chase(const gta_nav *nav, int sx, int sy, int z,
                         int tx, int ty, int ban, gta_route_node *out, int max)
{
    int n;
    g_allow_reverse = 1;
    n = route_bfs(nav, sx, sy, z, tx, ty, ban, out, max, 0, 0, 0, 0);
    g_allow_reverse = 0;
    return n;
}

/* ASK THE SEARCH WHERE IT CAN GO, INSTEAD OF GUESSING AND BEING TOLD NO.
 *
 * `gta_route_pick_target` throws sixteen darts at a ring around the car and
 * accepts the first one that lands on a block with any direction bits set. It
 * has no idea whether that block can be REACHED from here: Liberty City is a
 * one-way system, the window is 48 blocks, and a target across a dual
 * carriageway or the far side of a river is road and is unreachable.
 *
 * That was measured rather than guessed, and it is the largest single number
 * in the traffic:
 *
 *      WHY SEARCHES FAIL - car not on a road block 2233;
 *                          ran out of budget 67;
 *                          explored all it could reach 17148
 *
 * 88% of failures are a search that did everything right and found that the
 * place it was sent to does not connect. (And the budget, which this was
 * blamed on for a week, is 67 of 19448 - tripling it to 2200 changed the total
 * by four.)
 *
 * So the target is chosen FROM the search rather than before it. One BFS,
 * reservoir-sampling every reachable cell in the right distance band as it
 * goes past, and the winner becomes the destination. It costs what a failed
 * search already cost, it cannot fail while anywhere is reachable, and the
 * car ends up going somewhere it can actually get to. */
int gta_route_wander(const gta_nav *nav, int sx, int sy, int z, int ban,
                     int min_d, int max_d, unsigned long *seed,
                     gta_route_node *out, int max)
{
    return route_bfs(nav, sx, sy, z, 0, 0, ban, out, max, 1, min_d, max_d, seed);
}

int gta_route_pick_target(const gta_nav *nav, int bx, int by, int z,
                          int min, int max, unsigned long *seed,
                          int *tx, int *ty)
{
    int tries;

    /* Sixteen darts at the board rather than a scan. A scan of the ring is
     * hundreds of blocks and this runs while cars are driving; the city is
     * mostly street in the places traffic lives, so a handful of tries finds
     * one, and failing is harmless - the car keeps its old route or waits a
     * tick and is asked again. */
    for (tries = 0; tries < 16; tries++) {
        int dx, dy, r, ang;

        *seed = *seed * 1664525UL + 1013904223UL;
        r   = min + (int)((*seed >> 16) % (unsigned long)(max - min + 1));
        *seed = *seed * 1664525UL + 1013904223UL;
        ang = (int)((*seed >> 16) & 3);

        /* Four quadrants rather than a circle: no trigonometry, and a target
         * off one of the axes is as good as any other. */
        *seed = *seed * 1664525UL + 1013904223UL;
        dx = (int)((*seed >> 16) % (unsigned long)(r + 1));
        dy = r - dx;
        if (ang & 1) dx = -dx;
        if (ang & 2) dy = -dy;

        if (gta_nav_dirs(gta_nav_at(nav, bx + dx, by + dy, z)) != 0) {
            *tx = bx + dx;
            *ty = by + dy;
            return 1;
        }
    }
    return 0;
}
