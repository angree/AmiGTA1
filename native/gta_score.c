/* The score - see gta_score.h for whose rules these are.
 *
 * Licence: MIT (ours).
 */
#include "gta_score.h"

#define SCORE_CAP  999999999L

void gta_score_init(gta_score *s)
{
    s->score = 0;
    s->multiplier = 1;
    s->streak_type = -1;
    s->streak_count = 0;
    s->streak_timer = 0;
    s->last_award = 0;
}

void gta_score_tick(gta_score *s)
{
    if (s->streak_timer > 0 && --s->streak_timer == 0) {
        s->streak_type = -1;
        s->streak_count = 0;
    }
}

/* The value of a type, from the original's table. Only the entries the port
 * can reach are here; everything else falls through to 100, which is what its
 * default branch awards. */
static long type_value(int type)
{
    switch (type) {
    case GTA_SCORE_TYPE_CIVILIAN: return 100;
    case GTA_SCORE_TYPE_CAR:      return 100;
    case GTA_SCORE_TYPE_RAM:      return 10;
    default:                      return 100;
    }
}

/* The base a reason is worth: 2 -> 3, 3/7/8 -> 10, 4 -> 7, 5/6 -> 2, else 1. */
static int reason_base(int reason)
{
    switch (reason) {
    case 2:            return 3;
    case 3: case 7: case 8: return 10;
    case 4:            return 7;
    case 5: case 6:    return 2;
    default:           return 1;
    }
}

long gta_score_add(gta_score *s, long value)
{
    long award = (long)s->multiplier * value;
    s->score += award;
    if (s->score > SCORE_CAP)
        s->score = SCORE_CAP;
    s->last_award = award;
    return award;
}

long gta_score_event(gta_score *s, int type, int reason)
{
    int base = reason_base(reason);
    long factor;

    /* THE STREAK: the same type again inside the window counts up, anything
     * else starts a new one. */
    if (s->streak_type == type && s->streak_timer > 0) {
        if (s->streak_count < 30)
            s->streak_count++;
    } else {
        s->streak_type = type;
        s->streak_count = 1;
    }
    s->streak_timer = GTA_SCORE_STREAK_TICKS;

    /* Type 1 multiplies by the COUNT; every other type doubles per repeat.
     * A base of 1 is the count on its own either way. */
    if (type == GTA_SCORE_TYPE_CIVILIAN) {
        factor = (long)s->streak_count * base;
    } else if (base > 1) {
        int k;
        factor = base;
        for (k = 1; k < s->streak_count && k < 20; k++)
            factor *= 2;
    } else {
        factor = s->streak_count;
    }

    return gta_score_add(s, factor * type_value(type));
}
