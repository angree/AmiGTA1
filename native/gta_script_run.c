/* THE LEVEL SCRIPT'S LOGIC BLOCK AND ITS INTERPRETER.
 *
 * gta_script.c reads the declarations - the things a mission is played with.
 * This reads the other half, the commands, and runs them.
 *
 * THE SHAPE OF IT, as the original has it:
 *
 *   * a command line is `id NAME p1 p2 p3 p4 p5`, and `id` is the label
 *     other lines jump to;
 *   * 32 process slots, and ONE COMMAND PER ACTIVE PROCESS PER TICK. Not one
 *     per frame and not the whole block at once: SURVIVE, the side processes
 *     and every timer in the script depend on that rate;
 *   * `p2` is where to go on success, `p3` on failure. 0 means the next
 *     command in FILE ORDER, -1 means end the process, anything else is a
 *     line to jump to.
 *
 * WHAT IS NOT HERE YET. Only a handful of commands do their own work;
 * everything else takes the success path and is COUNTED. That is deliberate:
 * a script that stops at the first unimplemented command tells you nothing,
 * while one that walks its own logic prints, at the end of a run, exactly
 * which commands the game actually reaches and how often. The report is the
 * work list for the next step.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gta_script.h"

/* The commands this port acts on, by name. Everything else is carried and
 * counted - see the file header. */
static const struct { const char *name; int op; } cmd_names[] = {
    { "SURVIVE",        GTA_CMD_SURVIVE },
    { "DONOWT",         GTA_CMD_DONOWT },
    { "DISABLE",        GTA_CMD_DISABLE },
    { "ENABLE",         GTA_CMD_ENABLE },
    { "MISSION_END",    GTA_CMD_MISSION_END },
    { "STARTUP",        GTA_CMD_STARTUP },
    { "KICKSTART",      GTA_CMD_KICKSTART },
    { "KILL_SIDE_PROC", GTA_CMD_KILL_SIDE_PROC },
    { "MOBILE_BRIEF",   GTA_CMD_MOBILE_BRIEF },
    { "POWERUP_ON",     GTA_CMD_POWERUP_ON },
    { "POWERUP_OFF",    GTA_CMD_POWERUP_OFF },
    { "IS_POWERUP_DONE", GTA_CMD_IS_POWERUP_DONE },
    { "COMPARE",        GTA_CMD_COMPARE },
    { "INCCOUNT",       GTA_CMD_INCCOUNT },
    { "DECCOUNT",       GTA_CMD_DECCOUNT },
    { "ARROW",          GTA_CMD_ARROW },
    { "ARROW_OFF",      GTA_CMD_ARROW_OFF },
    { "MPHONE",         GTA_CMD_MPHONE },
    { "STEAL",          GTA_CMD_STEAL },
    { "CAR_ON",         GTA_CMD_CAR_ON },
    { "PARKED_ON",      GTA_CMD_PARKED_ON },
    { "CHECK_CAR",      GTA_CMD_CHECK_CAR },
    { "ARROWCAR",       GTA_CMD_ARROWCAR },
    { "IS_PED_IN_CAR",  GTA_CMD_IS_PED_IN_CAR },
    { "IS_GOAL_DEAD",   GTA_CMD_IS_GOAL_DEAD },
    { "BRIEF",          GTA_CMD_BRIEF },
    { "P_BRIEF",        GTA_CMD_P_BRIEF },
    { "P_BRIEF_TIMED",  GTA_CMD_P_BRIEF_TIMED },
    { "CANCEL_BRIEFING", GTA_CMD_CANCEL_BRIEFING },
    { "SPEECH_BRIEF",   GTA_CMD_SPEECH_BRIEF },
    { "MESSAGE_BRIEF",  GTA_CMD_MESSAGE_BRIEF },
    { "FRENZY_BRIEF",   GTA_CMD_FRENZY_BRIEF },
    { "PARK",           GTA_CMD_PARK },
    { "DOOR_ON",        GTA_CMD_DOOR_ON },
    { "DOOR_OFF",       GTA_CMD_DOOR_OFF },
    { "OPEN_DOOR",      GTA_CMD_OPEN_DOOR },
    { "CLOSE_DOOR",     GTA_CMD_CLOSE_DOOR },
    { "ANSWER",         GTA_CMD_ANSWER },
    { "RESET",          GTA_CMD_RESET },
    { "RESET_WITH_BRIEFS", GTA_CMD_RESET_BRIEFS },
    { "KEEP_THIS_PROC", GTA_CMD_KEEP_THIS_PROC },
    { "KILL_PROCESS",   GTA_CMD_KILL_PROCESS },
    { "KILL_SPEC_PROC", GTA_CMD_KILL_SPEC_PROC },
    { "PED_ON",         GTA_CMD_PED_ON },
    { "ARROWPED",       GTA_CMD_ARROWPED },
    { "PED_BACK",       GTA_CMD_PED_BACK },
    { "KILL_PED",       GTA_CMD_KILL_PED },
    { "IS_PED_ARRESTED", GTA_CMD_IS_PED_ARRESTED },
    { "DEAD_ARRESTED",  GTA_CMD_DEAD_ARRESTED },
    { 0, 0 }
};

static int cmd_op(const char *w, int len)
{
    int i;
    for (i = 0; cmd_names[i].name; i++)
        if ((int)strlen(cmd_names[i].name) == len &&
            strncmp(cmd_names[i].name, w, (size_t)len) == 0)
            return cmd_names[i].op;
    return GTA_CMD_OTHER;
}

/* The name table: every distinct command name in the section, so the report
 * can say what was executed without carrying a string per command. */
static int name_index(gta_script *s, const char *w, int len)
{
    int i;
    if (len > 15) len = 15;
    for (i = 0; i < s->n_cnames; i++)
        if ((int)strlen(s->cname[i]) == len &&
            strncmp(s->cname[i], w, (size_t)len) == 0)
            return i;
    if (s->n_cnames >= GTA_MAX_CNAMES)
        return 0;
    memcpy(s->cname[s->n_cnames], w, (size_t)len);
    s->cname[s->n_cnames][len] = 0;
    return s->n_cnames++;
}

/* `id NAME p1 p2 p3 p4 p5`. Returns 1 when the line is a command. */
static int parse_cmd(gta_script *s, const char *ln, gta_cmd *out)
{
    const char *p = ln, *w;
    long v[5];
    int wl, i;

    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9')
        return 0;
    out->line = (short)strtol(p, (char **)&p, 10);

    while (*p == ' ' || *p == '\t') p++;
    w = p;
    while ((*p >= 'A' && *p <= 'Z') || *p == '_' || (*p >= '0' && *p <= '9'))
        p++;
    wl = (int)(p - w);
    if (wl <= 0 || (*w >= '0' && *w <= '9'))
        return 0;                       /* not a name: not a command line */

    for (i = 0; i < 5; i++) {
        while (*p == ' ' || *p == '\t') p++;
        v[i] = (*p == '-' || (*p >= '0' && *p <= '9'))
               ? strtol(p, (char **)&p, 10) : 0;
    }
    out->op   = (unsigned char)cmd_op(w, wl);
    out->name = (unsigned char)name_index(s, w, wl);
    out->p1 = v[0]; out->p2 = v[1]; out->p3 = v[2];
    out->p4 = v[3]; out->p5 = v[4];
    return 1;
}

/* The logic block is what follows the declaration block's terminator - the
 * first line whose label is negative - and it ends at the next one. */
int gta_script_load_cmds(gta_script *s, const char *path, int level)
{
    FILE *f;
    char ln[256];
    int sec = -1, block = 0, i;

    s->n_cmds = 0;
    s->n_cnames = 0;
    for (i = 0; i < GTA_MAX_CNAMES; i++) s->hit[i] = 0;
    for (i = 0; i < GTA_MAX_PROC; i++) s->proc[i].cmd = -1;
    s->n_rearmed = s->n_disabled = s->n_enabled = 0;
    s->phone_ringing = 0;
    s->phone_cool = 0;
    s->n_answered = 0;

    /* ARM THE TRIGGERS, AND SET THE COUNTERS. A declaration that can start a
     * line begins armed; a counter begins at the first number on its line. */
    for (i = 0; i < s->n; i++) {
        s->d[i].state = (s->d[i].type == GTA_DECL_TRIGGER ||
                         s->d[i].type == GTA_DECL_MPHONES ||
                         s->d[i].type == GTA_DECL_SPRAY)
                        ? GTA_TRIG_ARMED : GTA_TRIG_NONE;
        s->d[i].handle = 0;
        if (s->d[i].type == GTA_DECL_DOOR)
            s->d[i].state = GTA_DOOR_SHUT;
        s->d[i].counter = (s->d[i].type == GTA_DECL_COUNTER ||
                           s->d[i].type == GTA_DECL_MISSION_COUNTER ||
                           s->d[i].type == GTA_DECL_SECRET_MISSION_COUNTER)
                          ? s->d[i].a : 0;
    }

    f = fopen(path, "r");
    if (!f) return 1;

    s->c = (gta_cmd *)malloc(GTA_MAX_CMDS * sizeof(gta_cmd));
    if (!s->c) { fclose(f); return 1; }

    while (fgets(ln, sizeof ln, f)) {
        if (ln[0] == '[') {
            if (sec == level) break;
            sec = atoi(ln + 1);
            continue;
        }
        if (sec != level) continue;
        if (ln[0] == '-') { block++; if (block > 1) break; continue; }
        if (block != 1) continue;
        if (s->n_cmds >= GTA_MAX_CMDS) break;
        if (parse_cmd(s, ln, &s->c[s->n_cmds]))
            s->n_cmds++;
    }
    fclose(f);
    return s->n_cmds > 0 ? 0 : 1;
}

/* Line label -> command index. Linear, like the declarations' lookup, and
 * for the same reason: it is asked a handful of times a tick. */
static int cmd_of_line(const gta_script *s, long line)
{
    int i;
    for (i = 0; i < s->n_cmds; i++)
        if (s->c[i].line == (short)line)
            return i;
    return -1;
}

static int proc_start(gta_script *s, long line, int trigger)
{
    int i, ci = cmd_of_line(s, line);
    if (ci < 0)
        return -1;
    for (i = 0; i < GTA_MAX_PROC; i++) {
        if (s->proc[i].cmd >= 0) continue;
        s->proc[i].cmd = (short)ci;
        s->proc[i].trigger = (short)trigger;
        s->proc[i].parent = -1;
        s->proc[i].keep = 0;
        s->proc[i].count = 0;
        s->proc[i].count2 = 0;
        s->proc[i].fresh = 1;
        s->n_started++;
        return i;
    }
    return -1;
}

/* The declaration a handle names, as something we may write to. */
static gta_decl *decl_of(gta_script *s, int line)
{
    int i;
    for (i = 0; i < s->n; i++)
        if (s->d[i].line == (short)line)
            return &s->d[i];
    return 0;
}

static void proc_end(gta_script *s, int i)
{
    s->proc[i].cmd = -1;
    s->proc[i].trigger = -1;
    s->n_ended++;
}

/* A PROCESS THAT ENDS ON `p2 == -1` RE-ARMS THE TRIGGER THAT STARTED IT
 * - the original ends the process and puts the trigger back in one step.
 * That is how a trigger-started mission becomes repeatable - walk back onto
 * the block and it runs again - and it is exactly what DISABLE exists to
 * prevent, by making the process forget its trigger first.
 *
 * The other two ways a process can stop here - running off the end of the
 * file, and jumping to a label that has no command - are the original's
 * ERROR path, which aborts the process and re-arms nothing. */
static void proc_end_rearm(gta_script *s, int i)
{
    if (s->proc[i].trigger >= 0) {
        gta_decl *d = decl_of(s, s->proc[i].trigger);
        if (d && d->state == GTA_TRIG_STARTED) {
            d->state = GTA_TRIG_ARMED;
            s->n_rearmed++;
            printf("gta: script - trigger line %d re-armed\n", d->line);
            fflush(stdout);
        }
    }
    proc_end(s, i);
}

/* THE GENERIC ADVANCE: 0 next in file order, -1
 * end the process, anything else a line to jump to. */
static void advance(gta_script *s, int i, long where)
{
    gta_proc *pr = &s->proc[i];
    pr->fresh = 1;
    pr->count = 0;
    pr->count2 = 0;
    if (where == -1) { proc_end_rearm(s, i); return; }
    if (where == 0) {
        if (pr->cmd + 1 >= s->n_cmds) { proc_end(s, i); return; }
        pr->cmd++;
        return;
    }
    {
        int ci = cmd_of_line(s, where);
        if (ci < 0) { proc_end(s, i); return; }   /* the original aborts too */
        pr->cmd = (short)ci;
    }
}

/* MPHONE alone continues at an INDEX rather than a line: its answer path is
 * `base + i` where `i` is which of the run of phones was picked, and `base`
 * is the command AFTER it when p2 is 0. The four handlers of Liberty City's
 * first job are lines 29, 30, 40 and 50 - consecutive commands, not
 * consecutive labels - so this cannot go through advance(). */
static void advance_to_index(gta_script *s, int i, int index)
{
    gta_proc *pr = &s->proc[i];
    pr->fresh = 1;
    pr->count = 0;
    pr->count2 = 0;
    if (index < 0 || index >= s->n_cmds) { proc_end(s, i); return; }
    pr->cmd = (short)index;
}

/* The placed telephone a declaration line names, or 0. */
static gta_placed *phone_of(gta_script *s, int line)
{
    int i;
    for (i = 0; i < s->n_phones; i++)
        if (s->phone[i].line == (short)line)
            return &s->phone[i];
    return 0;
}

static void phones_quiet(gta_script *s, long first, long count)
{
    long k;
    for (k = 0; k < count; k++) {
        gta_placed *ph = phone_of(s, (int)(first + k));
        if (ph) ph->ring = 0;
    }
}

void gta_script_set_brief(gta_script *s,
                          void (*fn)(void *ctx, int kind, int key, int arg),
                          void *ctx)
{
    s->brief_fn = fn;
    s->brief_ctx = ctx;
}

/* `p5` ON A SUCCESS PATH IS MONEY - the original pays it before it moves on.
 * The commands that do NOT pay are each a named exception - the whole BRIEF
 * family, END, MPHONE - and those keep plain advance(). */
static void advance_paid(gta_script *s, int i, long where, long p5)
{
    if (p5 > 0 && s->world && s->world->score)
        s->world->score(s->world_ctx, p5);
    advance(s, i, where);
}

/* One line of text, wherever it goes. Keys below 1000 are not text at all -
 * the original rejects them in every brief function. */
static void brief(gta_script *s, int kind, long key, long arg)
{
    if (s->brief_fn && key >= 1000)
        s->brief_fn(s->brief_ctx, kind, (int)key, (int)arg);
}

void gta_script_set_world(gta_script *s, const gta_script_world *w, void *ctx)
{
    s->world = w;
    s->world_ctx = ctx;
}

/* THE ORIGINAL'S 5-BASED COUNTDOWN, which is how
 * every "wait for this to happen, but not for ever" command times out: the
 * first tick loads `p4 + 5`, each tick that has not succeeded decrements it,
 * and the tick that brings it back to 5 takes the FAILURE path. `p4 < 1`
 * loads 0 instead, which never reaches 5 - that is "wait for ever", and it
 * is why the constant is 5 rather than 0. Returns 1 on the tick it runs out. */
static int countdown_out(gta_proc *pr, long p4)
{
    if (pr->fresh) {
        pr->count = p4 < 1 ? 0 : p4 + 5;
        pr->fresh = 0;
    }
    if (pr->count > 5 && --pr->count == 5)
        return 1;
    return 0;
}

/* A POWERUP declaration's block. The file writes POWERUPs in blocks (the
 * original multiplies by 0x40 and adds a half-block when it stores one), so
 * `x` and `y` are exactly what the world side wants. 0 when `line` is not a
 * POWERUP - a script bug rather than a port bug, but it must not crash. */
static int powerup_block(const gta_script *s, long line, int *bx, int *by,
                         int *kind, int *amount)
{
    const gta_decl *d = gta_script_by_line(s, (int)line);
    if (!d || d->type != GTA_DECL_POWERUP)
        return 0;
    *bx = d->x;
    *by = d->y;
    *kind = d->a;
    *amount = d->b;
    return 1;
}

static void step(gta_script *s, int i)
{
    gta_proc *pr = &s->proc[i];
    gta_cmd *c = &s->c[pr->cmd];

    s->hit[c->name]++;
    s->n_steps++;

    switch (c->op) {
    case GTA_CMD_DONOWT:
        /* The player's own process idles here for ever - there is no case
         * for it in the original either. */
        return;

    case GTA_CMD_SURVIVE:
        /* p4 ticks, counted in the process. */
        if (pr->fresh) { pr->count = c->p4; pr->fresh = 0; }
        if (--pr->count > 0)
            return;
        advance(s, i, c->p2);
        return;

    case GTA_CMD_ARROW:
        /* `ARROW object` - point at anything with a position, or at nothing
         * when p1 is -1 (MISSIONS.md 6.3). The GAME decides what that looks
         * like; the original hangs the arrow near the PLAYER in the target's
         * direction rather than over the target itself. */
        if (s->world && s->world->arrow) {
            long wx = 0, wy = 0;
            if (c->p1 >= 0 && gta_script_decl_pos(s, (int)c->p1, &wx, &wy))
                s->world->arrow(s->world_ctx, 1, wx, wy);
            else
                s->world->arrow(s->world_ctx, 0, 0, 0);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_ARROW_OFF:
        /* `1590 ARROW_OFF 0 0 0 0 2500` pays 2500 - the arrow going out at
         * the end of a job IS the pay-out in several missions. */
        if (s->world && s->world->arrow)
            s->world->arrow(s->world_ctx, 0, 0, 0);
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_MPHONE:
        /* `MPHONE first p2 count timer -`. It
         * rings `count` telephones whose LABELS run from `first`, waits for
         * the player to walk up to one of them ON FOOT, and then continues
         * at `base + i` - one handler per phone.
         *
         * It is also the one command RESET does not kill, because it is what
         * a level idles on between jobs. */
        if (s->phone_cool > 0)
            return;                     /* just answered one: nothing yet */
        if (pr->fresh) {
            pr->fresh = 0;
            if (!s->phone_ringing) {
                long k;
                s->phone_ringing = 1;
                pr->count = 25;
                for (k = 0; k < c->p3; k++) {
                    gta_placed *ph = phone_of(s, (int)(c->p1 + k));
                    if (ph && !ph->dead) ph->ring = 1;
                }
            }
        }
        pr->count--;
        {
            long fx = 0, fy = 0, k;
            int flip = (pr->count == 0 || pr->count == -10);
            /* OUT OF EARSHOT - the player's block more than fifteen from the
             * first phone. The phones stop and THE PROCESS STOPS, without
             * advancing and without re-arming: the trigger stays consumed
             * until a PHONE_TOGG puts it back. */
            if (gta_script_decl_pos(s, (int)c->p1, &fx, &fy)) {
                long adx = s->px - fx, ady = s->py - fy;
                if (adx < 0) adx = -adx;
                if (ady < 0) ady = -ady;
                if (adx > (15L << 21) || ady > (15L << 21)) {
                    phones_quiet(s, c->p1, c->p3);
                    s->phone_ringing = 0;
                    proc_end(s, i);
                    return;
                }
            }
            for (k = 0; k < c->p3; k++) {
                gta_placed *ph = phone_of(s, (int)(c->p1 + k));
                long adx, ady;
                if (!ph || ph->dead)
                    continue;
                if (flip && ph->ring)
                    ph->ring = (unsigned char)(ph->ring == 1 ? 2 : 1);
                if (!s->on_foot)
                    continue;
                adx = s->px - ph->x; if (adx < 0) adx = -adx;
                ady = s->py - ph->y; if (ady < 0) ady = -ady;
                if (adx >= (16L << 16) || ady >= (16L << 16))
                    continue;
                /* ANSWERED. */
                phones_quiet(s, c->p1, c->p3);
                s->phone_ringing = 0;
                s->phone_cool = 250;
                ph->dead = 1;
                s->n_answered++;
                printf("gta: script - phone %d answered, handler %ld + %ld\n",
                       ph->line, (long)pr->cmd + 1, k);
                fflush(stdout);
                if (c->p2 == -1) { proc_end(s, i); return; }
                advance_to_index(s, i,
                                 (c->p2 == 0 ? pr->cmd + 1
                                             : cmd_of_line(s, c->p2)) + (int)k);
                return;
            }
            if (pr->count == 0) pr->count = -1;
            else if (pr->count <= -10) pr->count = 25;
        }
        return;

    case GTA_CMD_PARKED_ON:
    case GTA_CMD_CAR_ON:
        /* `PARKED_ON object`: the declaration is a position and a model
         * until this runs (MISSIONS.md 6.6). A car that is already there is
         * not made twice. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (!d) { advance(s, i, c->p3); return; }
            if (!d->handle && s->world && s->world->car_on)
                d->handle = s->world->car_on(s->world_ctx, d->line, d->a,
                                             d->x, d->y,
                                             gta_script_angle(d->b));
            advance(s, i, d->handle ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_PED_ON:
        /* `274 PED_ON 252` - the job's own person, at the FUTUREPED line's
         * position. Its coordinates are PIXELS, which gta_script_decl_pos
         * already knows. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            long wx = 0, wy = 0;
            if (!d) { advance(s, i, c->p3); return; }
            if (!d->handle && s->world && s->world->ped_on &&
                gta_script_decl_pos(s, (int)c->p1, &wx, &wy))
                d->handle = s->world->ped_on(s->world_ctx, d->line, wx, wy,
                                             gta_script_angle(d->b));
            advance(s, i, d->handle ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_ARROWPED:
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->arrow_ped)
                s->world->arrow_ped(s->world_ctx, d->handle);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_PED_BACK:
        /* `333 PED_BACK 253 0 400 252` - p1 is the CAR and p4 the PERSON:
         * wait until he is in it. He gets in when the player brings the car
         * to him, which is the whole of the second half of mission 2. */
        {
            gta_decl *car = decl_of(s, (int)c->p1);
            gta_decl *ped = decl_of(s, (int)c->p4);
            if (car && ped && car->handle && ped->handle &&
                s->world && s->world->ped_into_car &&
                s->world->ped_into_car(s->world_ctx, ped->handle,
                                       car->handle)) {
                printf("gta: script - ped %d is in car %d\n",
                       ped->line, car->line);
                fflush(stdout);
                ped->handle = 0;
                advance_paid(s, i, c->p2, c->p5);
                return;
            }
            /* No timeout: the job waits for you to go and fetch him. */
        }
        return;

    case GTA_CMD_KILL_PED:
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->ped_kill) {
                s->world->ped_kill(s->world_ctx, d->handle);
                d->handle = 0;
            }
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_ARROWCAR:
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->arrow_car)
                s->world->arrow_car(s->world_ctx, d->handle);
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_STEAL:
        /* Wait until the player is at the wheel of THAT car. `p4` is the
         * usual 5-based timeout and `p5` the pay - 5000 for the car in
         * Liberty City's first job. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->player_car &&
                s->world->player_car(s->world_ctx) == d->handle) {
                printf("gta: script - STEAL: the player has car %d\n",
                       d->line);
                fflush(stdout);
                advance_paid(s, i, c->p2, c->p5);
                return;
            }
            if (countdown_out(pr, c->p4))
                advance(s, i, c->p3);
        }
        return;

    case GTA_CMD_CHECK_CAR:
        /* The same question asked once rather than waited on. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            int ok = d && d->handle && s->world && s->world->player_car &&
                     s->world->player_car(s->world_ctx) == d->handle;
            advance(s, i, ok ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_IS_PED_IN_CAR:
        /* `p1` is a ped - the PLAYER declaration in every use this port has
         * met - and `p4` a required model or -1. */
        {
            int ok = 0;
            if (s->world && s->world->player_car &&
                s->world->player_car(s->world_ctx)) {
                ok = 1;
                if (c->p4 >= 0 && s->world->player_car_model)
                    ok = s->world->player_car_model(s->world_ctx) == (int)c->p4;
            }
            advance(s, i, ok ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_IS_PED_ARRESTED:
        /* `235 IS_PED_ARRESTED 294 0 235 -1 0` - the job's "did he get
         * nicked" guard, looping on its own label until he does. It names
         * the PLAYER declaration in every use in section [1]. */
        {
            int yes = 0;
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->type == GTA_DECL_PLAYER &&
                s->world && s->world->player_arrested)
                yes = s->world->player_arrested(s->world_ctx);
            advance(s, i, yes ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_DEAD_ARRESTED:
        {
            int yes = 0;
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->type == GTA_DECL_PLAYER && s->world) {
                if (s->world->player_arrested)
                    yes = s->world->player_arrested(s->world_ctx);
                if (!yes && s->world->player_dead)
                    yes = s->world->player_dead(s->world_ctx);
            }
            advance(s, i, yes ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_IS_GOAL_DEAD:
        /* `150 IS_GOAL_DEAD 189 0 150 0 0` is a side process looping on its
         * own label until the car is wrecked - the "you broke it" half of
         * the job. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            int dead = 0;
            /* THE GOAL CAN BE THE PLAYER: `250 IS_GOAL_DEAD 294 0 250 -1 0`
             * is mission 2 waiting to see whether it has killed him. He has
             * no handle - he is not something the script created. */
            if (d && d->type == GTA_DECL_PLAYER) {
                dead = s->world && s->world->player_dead &&
                       s->world->player_dead(s->world_ctx);
                advance(s, i, dead ? c->p2 : c->p3);
                return;
            }
            if (d && d->handle && s->world) {
                /* A GOAL IS A CAR OR A PERSON and the command does not say
                 * which; the declaration does. */
                if (d->type == GTA_DECL_FUTUREPED || d->type == GTA_DECL_PED)
                    dead = s->world->ped_dead &&
                           s->world->ped_dead(s->world_ctx, d->handle);
                else
                    dead = s->world->car_dead &&
                           s->world->car_dead(s->world_ctx, d->handle);
            }
            advance(s, i, dead ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_DOOR_ON:
    case GTA_CMD_OPEN_DOOR:
        /* THE GARAGE OPENS. The port has no door graphic yet - the block's
         * own tiles do not change - so this is the state and the log; what
         * reads it is PARK. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (!d) { advance(s, i, c->p3); return; }
            if (d->state != GTA_DOOR_OPEN) {
                d->state = GTA_DOOR_OPEN;
                printf("gta: script - door %d opens at (%d,%d)\n",
                       d->line, d->x, d->y);
                fflush(stdout);
            }
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_DOOR_OFF:
    case GTA_CMD_CLOSE_DOOR:
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (!d) { advance(s, i, c->p3); return; }
            d->state = GTA_DOOR_SHUT;
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_PARK:
        /* `135 PARK 191 0 0 3 20000` - leave the car in the garage and get
         * paid. The original wants the root player IN a car, the car
         * STOPPED, and its body inside the door's block band; this asks for
         * the block itself, which is the same test to within half a car.
         * There is no timeout: a job waits for you to deliver. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            long wx = 0, wy = 0;
            int stopped = 0;
            if (d && s->world && s->world->player_car_at &&
                s->world->player_car_at(s->world_ctx, &wx, &wy, &stopped) &&
                stopped) {
                long dx = wx - gta_script_centre(d->x);
                long dy = wy - gta_script_centre(d->y);
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx <= (40L << 16) && dy <= (40L << 16)) {
                    printf("gta: script - PARKED at door %d\n", d->line);
                    fflush(stdout);
                    advance_paid(s, i, c->p2, c->p5);
                    return;
                }
            }
            /* AND SAY WHY NOT, once a second. A job that will not finish is
             * the hardest thing in this interpreter to diagnose from a
             * histogram: "PARK x174" says the command is running and
             * nothing about which of its three conditions is failing. */
            if (++pr->count >= 25) {
                pr->count = 0;
                if (!d) {
                    printf("gta: script - PARK: no door %ld\n", c->p1);
                } else {
                    long dx = wx - gta_script_centre(d->x);
                    long dy = wy - gta_script_centre(d->y);
                    printf("gta: script - PARK at door %d (%d,%d): in a car %d,"
                           " stopped %d, off by (%ld,%ld) px\n",
                           d->line, d->x, d->y, wx || wy, stopped,
                           dx >> 16, dy >> 16);
                }
                fflush(stdout);
            }
        }
        return;

    case GTA_CMD_ANSWER:
        /* `210 ANSWER 72 0 420 10 2500` - ONE phone rings and the job calls
         * you back on it. `p4` is how many fifty-tick cycles it will wait -
         * ten of them, twenty seconds - and `p3` is what happens when it
         * gives up (MISSIONS.md 6.4). `p5` is NOT a score here: the original
         * overwrites it with a working flag. */
        {
            gta_placed *ph = phone_of(s, (int)c->p1);
            if (pr->fresh) {
                pr->fresh = 0;
                pr->count = c->p4 + 10;
                pr->count2 = 0;
                if (ph && !ph->dead) {
                    ph->ring = 1;
                    s->phone_ringing = 1;
                    printf("gta: script - phone %d is ringing for you\n",
                           ph->line);
                    fflush(stdout);
                }
            }
            if (pr->count2 <= 0) {
                pr->count2 = 50;
                if (ph && ph->ring)
                    ph->ring = (unsigned char)(ph->ring == 1 ? 2 : 1);
                if (--pr->count <= 10) {
                    if (ph) ph->ring = 0;
                    s->phone_ringing = 0;
                    printf("gta: script - phone %ld rang out\n", c->p1);
                    fflush(stdout);
                    advance(s, i, c->p3);
                    return;
                }
            } else {
                pr->count2--;
            }
            if (s->on_foot && ph && !ph->dead) {
                long adx = s->px - ph->x, ady = s->py - ph->y;
                if (adx < 0) adx = -adx;
                if (ady < 0) ady = -ady;
                if (adx < (16L << 16) && ady < (16L << 16)) {
                    ph->ring = 0;
                    s->phone_ringing = 0;
                    s->n_answered++;
                    printf("gta: script - phone %d answered (ANSWER)\n",
                           ph->line);
                    fflush(stdout);
                    advance(s, i, c->p2);
                    return;
                }
            }
        }
        return;

    case GTA_CMD_MISSION_END:
        /* Pay, and put one on the multiplier for the rest of the level. */
        if (s->world && s->world->mission_done)
            s->world->mission_done(s->world_ctx);
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_RESET:
    case GTA_CMD_RESET_BRIEFS:
        /* WHAT TIDIES UP BETWEEN JOBS. It kills
         * every other process except the ones marked KEEP_THIS_PROC and the
         * ones sitting on MPHONE - which is the level idling between jobs
         * and must never be stopped - gives the mission's cars back to the
         * traffic, switches off every TEMPORARY trigger and door, and puts
         * the arrow out. A declaration is temporary when its line carries no
         * flag; Liberty City's phones and start trigger all carry `1`, which
         * is why they survive this. */
        {
            int k, killed = 0, cars = 0, trigs = 0;
            for (k = 0; k < GTA_MAX_PROC; k++) {
                if (k == i || s->proc[k].cmd < 0 || s->proc[k].keep)
                    continue;
                if (s->c[s->proc[k].cmd].op == GTA_CMD_MPHONE)
                    continue;
                proc_end(s, k);
                killed++;
            }
            for (k = 0; k < s->n; k++) {
                gta_decl *d = &s->d[k];
                if (d->flag)
                    continue;           /* permanent: RESET does not touch it */
                if (d->handle) {
                    if (s->world && s->world->car_release)
                        s->world->car_release(s->world_ctx, d->handle);
                    d->handle = 0;
                    cars++;
                }
                if (d->state == GTA_TRIG_ARMED ||
                    d->state == GTA_TRIG_STARTED) {
                    d->state = GTA_TRIG_DISABLED;
                    trigs++;
                }
                if (d->state == GTA_DOOR_OPEN)
                    d->state = GTA_DOOR_SHUT;
            }
            if (s->world && s->world->arrow)
                s->world->arrow(s->world_ctx, 0, 0, 0);
            printf("gta: script - line %d RESET: %d processes, %d cars, "
                   "%d triggers\n",
                   c->line, killed, cars, trigs);
            fflush(stdout);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_COMPARE:
        /* `COMPARE counter p2 p3 value p5`. It is how the
         * script skips work already done: `15 COMPARE 548 18 0 1 0` means
         * "if the first South Park phone's job is finished, go and look at
         * the second one". */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            advance(s, i, (d && (long)d->counter == c->p4) ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_INCCOUNT:
        /* `++counter; if (p4 < 0 || counter < p4) -> p3 else -> p2`. With
         * p4 = 0 it always succeeds and with p4 = -1 it always fails, which
         * is how the script uses it as a plain "add one and carry on". */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            long v = 0;
            if (d) v = ++d->counter;
            advance(s, i, (c->p4 < 0 || v < c->p4) ? c->p3 : c->p2);
        }
        return;

    case GTA_CMD_DECCOUNT:
        /* `--counter; if (counter < 1) -> p2 else -> p3` - the countdown of
         * "how many of these are left to do". */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            long v = 0;
            if (d) v = --d->counter;
            advance(s, i, v < 1 ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_DISABLE:
        /* The trigger is switched off AND THIS PROCESS FORGETS IT, so
         * ending will not put it back (MISSIONS.md 6.5). Both halves
         * matter: without the second, a mission that disables its own
         * start trigger runs again the moment it finishes. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->state != GTA_TRIG_NONE) {
                if (d->state != GTA_TRIG_DISABLED)
                    printf("gta: script - trigger line %d disabled\n",
                           d->line);
                d->state = GTA_TRIG_DISABLED;
                s->n_disabled++;
            }
        }
        pr->trigger = -1;
        advance(s, i, c->p2);
        return;

    case GTA_CMD_ENABLE:
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->state != GTA_TRIG_NONE) {
                if (d->state != GTA_TRIG_ARMED)
                    printf("gta: script - trigger line %d enabled\n",
                           d->line);
                d->state = GTA_TRIG_ARMED;
                s->n_enabled++;
            }
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_STARTUP:
        /* Once per game: mark the start trigger consumed and jump. The
         * process running this IS the one that trigger started, so it must
         * forget it too - otherwise the level restarts the moment the
         * opening block ends. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->state != GTA_TRIG_NONE)
                d->state = GTA_TRIG_STARTED;
        }
        pr->trigger = -1;
        advance(s, i, c->p2);
        return;

    case GTA_CMD_KICKSTART:
        /* A SIDE PROCESS at line p1, and this one carries on. The child
         * remembers WHICH KICKSTART made it, by line, because that is how
         * KILL_SIDE_PROC names it later. */
        {
            int k = proc_start(s, c->p1, -1);
            if (k >= 0)
                s->proc[k].parent = c->line;
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_KEEP_THIS_PROC:
        pr->keep = 1;
        advance_paid(s, i, c->p2, c->p5);
        return;

    /* THE TEXT COMMANDS. None of them pays: every one is listed in
     * one of the original's own exceptions: it moves on and pays nothing. */
    case GTA_CMD_MOBILE_BRIEF:
        brief(s, GTA_BRIEF_MOBILE, c->p5, 0);
        advance(s, i, c->p2);
        return;

    case GTA_CMD_BRIEF:
        brief(s, GTA_BRIEF_PHONE, c->p5, 0);
        advance(s, i, c->p2);
        return;

    case GTA_CMD_SPEECH_BRIEF:
        brief(s, GTA_BRIEF_SPEECH, c->p5, 0);
        advance(s, i, c->p2);
        return;

    case GTA_CMD_FRENZY_BRIEF:
        brief(s, GTA_BRIEF_FRENZY, c->p5, 0);
        advance(s, i, c->p2);
        return;

    case GTA_CMD_P_BRIEF:
        brief(s, GTA_BRIEF_PAGER, c->p5, 0);
        advance(s, i, c->p2);
        return;

    case GTA_CMD_P_BRIEF_TIMED:
        /* `205 P_BRIEF_TIMED 0 0 0 18 1500` - "Time left..." counting 18
         * seconds down on the pager. */
        brief(s, GTA_BRIEF_PAGER_T, c->p5, c->p4);
        advance(s, i, c->p2);
        return;

    case GTA_CMD_MESSAGE_BRIEF:
        /* The big centred card: [2500] MISSION COMPLETE!, [2501] FAILED! */
        brief(s, GTA_BRIEF_BIG, c->p5, 0);
        advance(s, i, c->p2);
        return;

    case GTA_CMD_CANCEL_BRIEFING:
        /* `p1` names the P_BRIEF_TIMED whose line is to be taken down; this
         * one DOES pay. */
        {
            const gta_cmd *t = 0;
            int k;
            for (k = 0; k < s->n_cmds; k++)
                if (s->c[k].line == (short)c->p1) { t = &s->c[k]; break; }
            brief(s, GTA_BRIEF_CANCEL, t ? t->p5 : 0, 0);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_POWERUP_ON:
        /* The crate itself. The original creates map object 0x54 here - the
         * declaration is only a position until this runs. */
        {
            int bx, by, kind, amount;
            if (s->world && s->world->powerup_on &&
                powerup_block(s, c->p1, &bx, &by, &kind, &amount))
                s->world->powerup_on(s->world_ctx, (int)c->p1, kind, amount,
                                     bx, by);
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_POWERUP_OFF:
        {
            int bx, by, kind, amount;
            if (s->world && s->world->powerup_off &&
                powerup_block(s, c->p1, &bx, &by, &kind, &amount))
                s->world->powerup_off(s->world_ctx, (int)c->p1, bx, by);
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_IS_POWERUP_DONE:
        /* Collected? Then the success path. Still standing there: wait, for
         * p4 ticks or for ever, and then fail. */
        {
            int bx, by, kind, amount;
            if (s->world && s->world->powerup_done &&
                powerup_block(s, c->p1, &bx, &by, &kind, &amount)) {
                if (s->world->powerup_done(s->world_ctx, (int)c->p1, bx, by)) {
                    advance(s, i, c->p2);
                    return;
                }
                if (countdown_out(pr, c->p4))
                    advance(s, i, c->p3);
                return;
            }
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_KILL_SIDE_PROC:
    case GTA_CMD_KILL_PROCESS:
    case GTA_CMD_KILL_SPEC_PROC:
        /* `60 KILL_SIDE_PROC 32001` stops whatever `32001 KICKSTART 13`
         * started - the arrow loop that was pointing at the phones, in the
         * one Liberty City uses it for. */
        {
            int k, n = 0;
            for (k = 0; k < GTA_MAX_PROC; k++)
                if (k != i && s->proc[k].cmd >= 0 &&
                    s->proc[k].parent == (short)c->p1) {
                    proc_end(s, k);
                    n++;
                }
            if (n) {
                printf("gta: script - killed %d side process%s of %ld\n",
                       n, n == 1 ? "" : "es", c->p1);
                fflush(stdout);
            }
        }
        advance(s, i, c->p2);
        return;

    default:
        /* Not written yet: take the success path so the script walks its own
         * logic, and let the report say how often this was reached. The
         * generic path pays, because that is what the original's generic
         * path does. */
        advance_paid(s, i, c->p2, c->p5);
        return;
    }
}

void gta_script_tick(gta_script *s, long px, long py, int on_foot)
{
    int i;

    if (s->n_cmds <= 0)
        return;
    s->px = px;
    s->py = py;
    s->on_foot = on_foot;
    /* HALF THE SIMULATION'S RATE. See gta_script.half. */
    s->half ^= 1;
    if (s->half)
        return;
    if (s->phone_cool > 0)
        s->phone_cool--;

    /* THE TRIGGERS. An ARMED trigger starts line `a` when the player is
     * within `b` blocks of it and becomes STARTED; a process that ends puts
     * it back to ARMED unless DISABLE took the link away first. */
    for (i = 0; i < s->n; i++) {
        gta_decl *d = &s->d[i];
        long dx, dy, r;
        if (d->state != GTA_TRIG_ARMED)
            continue;
        /* AN MPHONES TRIGGER HAS ITS OWN CONDITION on top of the distance:
         * no phone anywhere may be ringing and the player must not already
         * be on a phone job - the original's trigger type 0xc.
         * Without it two runs of phones can ring at once. */
        if (d->type == GTA_DECL_MPHONES && (s->phone_ringing || s->phone_cool))
            continue;
        /* A SPRAY SHOP IS A TRIGGER OF ITS OWN KIND - type 3, built into the
         * game rather than starting a line of the script - so the scan that
         * starts processes must leave it alone. It is handled below. */
        if (d->type == GTA_DECL_SPRAY)
            continue;
        dx = px - gta_script_centre(d->x);
        dy = py - gta_script_centre(d->y);
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        r = ((long)(d->b > 0 ? d->b : 1)) << 21;
        if (dx > r || dy > r) continue;
        d->state = GTA_TRIG_STARTED;
        if (proc_start(s, d->a, d->line) >= 0) {
            printf("gta: script - trigger line %d fired, running line %ld\n",
                   d->line, (long)d->a);
            fflush(stdout);
        }
    }

    /* THE SPRAY SHOPS. Drive in and the car changes colour and the police
     * lose interest; drive two blocks away and the shop is ready again. The
     * radius is the block itself, which is what a declaration with no second
     * number asks for. */
    for (i = 0; i < s->n; i++) {
        gta_decl *d = &s->d[i];
        long dx, dy;
        if (d->type != GTA_DECL_SPRAY)
            continue;
        dx = px - gta_script_centre(d->x); if (dx < 0) dx = -dx;
        dy = py - gta_script_centre(d->y); if (dy < 0) dy = -dy;
        if (d->state == GTA_TRIG_STARTED) {
            /* Used: it re-arms once he is clear of it. */
            if (dx > (2L << 21) || dy > (2L << 21))
                d->state = GTA_TRIG_ARMED;
            continue;
        }
        if (d->state != GTA_TRIG_ARMED)
            continue;
        if (dx > (1L << 20) || dy > (1L << 20))
            continue;                   /* half a block: inside the shop */
        if (s->world && s->world->respray &&
            s->world->respray(s->world_ctx, d->a)) {
            d->state = GTA_TRIG_STARTED;
            s->n_resprays++;
            printf("gta: script - spray shop %d: colour %d\n",
                   d->line, d->a);
            fflush(stdout);
        }
    }

    /* PHONE_TOGG - THE OTHER HALF OF THE PHONE LOOP. `203 (124,136) PHONE_TOGG
     * 192 5`: when the player is within FIVE blocks of that spot but no
     * longer within THREE, and trigger 192 has been started, put it back to
     * armed. That is how a run of phones starts ringing again after you walk
     * off and come back, and it is the only re-arming Liberty City's opening
     * actually uses - the original's trigger type 0xd. */
    for (i = 0; i < s->n; i++) {
        gta_decl *d = &s->d[i], *t;
        long dx, dy, r;
        if (d->type != GTA_DECL_PHONE_TOGG)
            continue;
        dx = px - gta_script_centre(d->x); if (dx < 0) dx = -dx;
        dy = py - gta_script_centre(d->y); if (dy < 0) dy = -dy;
        r = ((long)(d->b > 0 ? d->b : 1)) << 21;
        if (dx > r || dy > r) continue;              /* not in the ring */
        if (dx <= (3L << 21) && dy <= (3L << 21)) continue;  /* too close */
        t = decl_of(s, d->a);
        if (t && t->state == GTA_TRIG_STARTED) {
            t->state = GTA_TRIG_ARMED;
            s->n_rearmed++;
            printf("gta: script - PHONE_TOGG %d re-armed trigger %d\n",
                   d->line, t->line);
            fflush(stdout);
        }
    }

    for (i = 0; i < GTA_MAX_PROC; i++)
        if (s->proc[i].cmd >= 0)
            step(s, i);
}

/* THE ARMED TRIGGERS NEAR A POINT, printed once. A test of the re-arm has to
 * walk the player onto a trigger that is still live, and after the opening
 * block has switched twenty-odd of them off there is no way to know which
 * from the file alone. */
void gta_script_live_triggers(const gta_script *s, long px, long py, int blocks)
{
    long r = (long)blocks << 21;
    int i, n = 0;
    for (i = 0; i < s->n; i++) {
        const gta_decl *d = &s->d[i];
        long dx, dy;
        if (d->state != GTA_TRIG_ARMED)
            continue;
        dx = px - gta_script_centre(d->x); if (dx < 0) dx = -dx;
        dy = py - gta_script_centre(d->y); if (dy < 0) dy = -dy;
        if (dx > r || dy > r) continue;
        printf("gta: script - trigger %d armed at (%d,%d) r%d -> line %d\n",
               d->line, d->x, d->y, d->b, d->a);
        n++;
    }
    printf("gta: script - %d armed triggers within %d blocks\n", n, blocks);
    fflush(stdout);
}

void gta_script_report(const gta_script *s)
{
    int i, live = 0, shown = 0;

    if (s->n_cmds <= 0)
        return;
    for (i = 0; i < GTA_MAX_PROC; i++)
        if (s->proc[i].cmd >= 0) live++;

    printf("gta: script - %d processes running, %d started, %d ended, "
           "%d commands run; triggers %ld re-armed %ld disabled %ld enabled; "
           "phones %ld answered%s;",
           live, s->n_started, s->n_ended, s->n_steps,
           s->n_rearmed, s->n_disabled, s->n_enabled, s->n_answered,
           s->phone_ringing ? " (ringing)" : "");
    if (s->n_resprays)
        printf(" resprays %ld;", s->n_resprays);
    /* The five most-executed names: the work list for the next step. */
    for (shown = 0; shown < 5; shown++) {
        int best = -1;
        long bv = 0;
        for (i = 0; i < s->n_cnames; i++)
            if (s->hit[i] > bv) { bv = s->hit[i]; best = i; }
        if (best < 0) break;
        printf(" %s x%ld", s->cname[best], bv);
        ((gta_script *)s)->hit[best] = -bv;      /* mark, restored below */
    }
    for (i = 0; i < s->n_cnames; i++)
        if (s->hit[i] < 0) ((gta_script *)s)->hit[i] = -s->hit[i];
    printf("\n");
    fflush(stdout);
}
