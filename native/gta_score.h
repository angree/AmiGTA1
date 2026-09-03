/* The score, the multiplier and the streaks - Phase 5 item 5(b), the first
 * half: what the corner of the screen shows.
 *
 * THE RULES ARE THE ORIGINAL'S. It does not add a flat number per kill: a
 * score is a TYPE (what was killed) and a REASON (how), the reason picks a
 * base, doing the same type again within thirteen frames multiplies it, and
 * the whole thing is multiplied by the player's own multiplier before it is
 * added. Killing a civilian - type 1 - is `multiplier * streak * base * 100`,
 * with base 1 for a run-over and 2 for a shooting, so the second man run over
 * inside a quarter of a second is worth twice the first.
 *
 * The multiplier starts at 1, is raised only by the pickup and by a mission
 * reward, and is HALVED (never below 1) when the player is busted. None of
 * those three exist yet, so it sits at 1 and the scoring above is what moves.
 *
 * What is not here: the money side (respray, missions), the crime counters
 * that the pager prints on a bust, and the "GOURANGA!" bonus. They belong
 * with the wanted level and the missions.
 *
 * Portable C89, no floats, no Amiga headers. Licence: MIT (ours).
 */
#ifndef GTA_SCORE_H
#define GTA_SCORE_H

/* The score types that exist in the port so far. The original's table runs to
 * 0x2d; these are the entries our events can actually reach. */
#define GTA_SCORE_TYPE_CIVILIAN  1      /* a pedestrian, value 100 */
#define GTA_SCORE_TYPE_CAR      0x1c    /* a car taken, value 100 */
#define GTA_SCORE_TYPE_RAM      0x18    /* a car rammed */

/* ...and the reasons, which pick the base: a run-over is worth less than a
 * shooting, and both are worth less than an explosion. */
#define GTA_SCORE_REASON_RUNOVER 1      /* base 1 */
#define GTA_SCORE_REASON_SHOT    5      /* base 2 */
#define GTA_SCORE_REASON_BURNED  8      /* base 10 */
#define GTA_SCORE_REASON_BLOWN   2      /* base 3 */

/* Thirteen of the original's frames, in the port's 50 Hz ticks. */
#define GTA_SCORE_STREAK_TICKS  20

typedef struct {
    long score;             /* capped at 999999999, as the original is */
    int  multiplier;        /* 1 at the start of a level */
    int  streak_type;       /* the last type scored, -1 for none */
    int  streak_count;
    int  streak_timer;      /* ticks left in which the same type still counts */
    long last_award;        /* what the last event was worth - for the log */
} gta_score;

void gta_score_init(gta_score *s);

/* One tick: the streak window closes on its own. */
void gta_score_tick(gta_score *s);

/* Score one event and return what it was worth. `type` is a GTA_SCORE_TYPE_*,
 * `reason` a GTA_SCORE_REASON_* (0 when the type carries its own value). */
long gta_score_event(gta_score *s, int type, int reason);

/* A flat award, still multiplied - the original's popup call, which is what
 * shooting a car uses (+10). */
long gta_score_add(gta_score *s, long value);

#endif /* GTA_SCORE_H */
