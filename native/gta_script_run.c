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
#include "gta_snd.h"        /* GTA_VOICE_* - which line the world says */

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
    { "GENERAL_ONSCREEN", GTA_CMD_GENERAL_ONSCREEN },
    { "CHANGE_PED_TYPE", GTA_CMD_CHANGE_PED_TYPE },
    { "HELL_ON",        GTA_CMD_HELL_ON },
    { "DROP_WANTED_LEVEL", GTA_CMD_DROP_WANTED },
    { "REMAP_PED",      GTA_CMD_REMAP_PED },
    { "PED_SENDTO",     GTA_CMD_PED_SENDTO },
    { "WAIT_FOR_PED",   GTA_CMD_WAIT_FOR_PED },
    { "DROP_ON",        GTA_CMD_DROP_ON },
    { "KILL_DROP",      GTA_CMD_KILL_DROP },
    { "GOTO_DROPOFF",   GTA_CMD_GOTO_DROPOFF },
    { "SCORE_CHECK",    GTA_CMD_SCORE_CHECK },
    { "DESTROY",        GTA_CMD_DESTROY },
    { "FRENZY_SET",     GTA_CMD_FRENZY_SET },
    { "BANK_ROBBERY",   GTA_CMD_BANK_ROBBERY },
    { "SETBOMB",        GTA_CMD_SETBOMB },
    { "CRANE",          GTA_CMD_CRANE },
    { "DO_GTA",         GTA_CMD_DO_GTA },
    { "RED_ARROW",      GTA_CMD_RED_ARROW },
    { "RED_ARROW_OFF",  GTA_CMD_RED_ARROW_OFF },
    { "END",            GTA_CMD_END },
    { "KILL_OBJ",       GTA_CMD_KILL_OBJ },
    { "KILL_CAR",       GTA_CMD_KILL_CAR },
    { "SET_PED_SPEED",  GTA_CMD_SET_PED_SPEED },
    { "ARMEDMESS",      GTA_CMD_ARMEDMESS },
    { "DISARMMESS",     GTA_CMD_DISARMMESS },
    { "FREEZE_TIMED",   GTA_CMD_FREEZE_TIMED },
    { "FREEZE_ENTER",   GTA_CMD_FREEZE_ENTER },
    { "UNFREEZE_ENTER", GTA_CMD_UNFREEZE_ENTER },
    { "BANK_ALARM_ON",  GTA_CMD_BANK_ALARM_ON },
    { "BANK_ALARM_OFF", GTA_CMD_BANK_ALARM_OFF },
    { "FRENZY_CHECK",   GTA_CMD_FRENZY_CHECK },
    { "STOP_FRENZY",    GTA_CMD_STOP_FRENZY },
    { "KF_PROCESS",     GTA_CMD_KF_PROCESS },
    { "RESET_KF",       GTA_CMD_RESET_KF },
    { "KF_BRIEF_TIMED", GTA_CMD_KF_BRIEF_TIMED },
    { "KF_CANCEL_BRIEFING", GTA_CMD_KF_CANCEL_BRIEFING },
    { "KF_BRIEF_GENERAL", GTA_CMD_KF_BRIEF_GENERAL },
    { "KF_CANCEL_GENERAL", GTA_CMD_KF_CANCEL_GENERAL },
    { "EXPLODE",        GTA_CMD_EXPLODE },
    { "PLAIN_EXPL_BUILDING", GTA_CMD_PLAIN_EXPL_BUILDING },
    { "EXPL_NO_FIRE",   GTA_CMD_EXPL_NO_FIRE },
    /* THE CHECKS THIS PORT CANNOT ANSWER YET. They are named here on
     * purpose: carried as GTA_CMD_OTHER they would take the SUCCESS path,
     * and a check that answers yes by default starts jobs that have not
     * been earned and ends ones that are running. Each of these is a
     * question about something the port does not have - a kill frenzy, a
     * second player, a train - so the honest answer is no. */
    { "MAKEOBJ",        GTA_CMD_MAKEOBJ },
    { "EXPLODE_CAR",    GTA_CMD_EXPLODE_CAR },
    { "CHANGE_BLOCK",   GTA_CMD_CHANGE_BLOCK },
    { "CHANGE_TYPE",    GTA_CMD_CHANGE_TYPE },
    { "EXPL_PED",       GTA_CMD_EXPL_PED },
    { "SET_NO_COLLIDE", GTA_CMD_SET_NO_COLLIDE },
    { "SET_COLLIDE",    GTA_CMD_SET_COLLIDE },
    { "PED_WEAPON",     GTA_CMD_PED_WEAPON },
    { "PED_POLICE",     GTA_CMD_PED_POLICE },
    { "ADD_A_LIFE",     GTA_CMD_ADD_A_LIFE },
    { "INC_HEADS",      GTA_CMD_INC_HEADS },
    { "IS_PED_STUNNED", GTA_CMD_IS_PED_STUNNED },
    { "DUMMYON",        GTA_CMD_DUMMYON },
    { "SENDTO",         GTA_CMD_SENDTO },
    { "GOTO",           GTA_CMD_GOTO },
    { "LOCK_DOOR",      GTA_CMD_LOCK_DOOR },
    { "UNLOCK_DOOR",    GTA_CMD_UNLOCK_DOOR },
    { "PED_OUT_OF_CAR", GTA_CMD_PED_OUT_OF_CAR },
    { "GET_DRIVER_INFO", GTA_CMD_GET_DRIVER_INFO },
    { "DUMMY_DRIVE_ON", GTA_CMD_DUMMY_DRIVE_ON },
    { "GET_CAR_INFO",   GTA_CMD_GET_CAR_INFO },
    { "LOCATE",         GTA_CMD_LOCATE },
    { "PIXEL_CAR_ON",   GTA_CMD_PIXEL_CAR_ON },
    { "PARKED_PIXELS_ON", GTA_CMD_PARKED_PIXELS_ON },
    { "HUNTON",         GTA_CMD_HUNTON },
    { "HUNTOFF",        GTA_CMD_HUNTOFF },
    { "OBTAIN",         GTA_CMD_OBTAIN },
    { "THROW",          GTA_CMD_THROW },
    { "SETUP_REPO",     GTA_CMD_SETUP_REPO },
    { "DO_REPO",        GTA_CMD_DO_REPO },
    { "MODEL_HUNT",     GTA_CMD_MODEL_HUNT },
    { "NEXT_KICK",      GTA_CMD_NEXT_KICK },
    { "SET_KILLTRIG",   GTA_CMD_SET_KILLTRIG },
    { "FREEUP_CAR",     GTA_CMD_FREEUP_CAR },
    { "START_MODEL",    GTA_CMD_START_MODEL },
    { "DO_MODEL",       GTA_CMD_DO_MODEL },
    { "RETURN_CONTROL", GTA_CMD_RETURN_CONTROL },
    { "PLAYER_ARE_BOTH_ONSCREEN", GTA_CMD_PLAYER_ARE_BOTH_ONSCREEN },
    { "IS_PLAYER_ON_TRAIN", GTA_CMD_IS_PLAYER_ON_TRAIN },
    { "WRECK_A_TRAIN", GTA_CMD_WRECK_A_TRAIN },
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
    out->line = strtol(p, (char **)&p, 10);

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
/* THE GATES (226): BARRIER and friends are doors with a condition. */
static int is_gate(int type)
{
    return type == GTA_DECL_BARRIER || type == GTA_DECL_MODEL_BARRIER ||
           type == GTA_DECL_SPECIFIC_BARR || type == GTA_DECL_SPECIFIC_DOOR ||
           type == GTA_DECL_BASIC_BARRIER;
}

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
                         s->d[i].type == GTA_DECL_SPRAY ||
                         s->d[i].type == GTA_DECL_BOMBSHOP)
                        ? GTA_TRIG_ARMED : GTA_TRIG_NONE;
        s->d[i].handle = 0;
        if (s->d[i].type == GTA_DECL_BOMBSHOP_COST)
            s->bomb_cost = s->d[i].a;
        if (s->d[i].type == GTA_DECL_CHOPPER_ENDPOINT) {
            s->chopper_x = ((long)s->d[i].x / 2) << 16;
            s->chopper_y = ((long)s->d[i].y / 2) << 16;
        }
        if (is_gate(s->d[i].type))
            s->d[i].state = GTA_DOOR_SHUT;
        s->d[i].kill_parent = 0;
        s->d[i].kill_slot = 0;
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
    /* A POINTER WALK, not s->c[i]: the indexed form returned -1 on the
     * Amiga for a label that was there (line 5705 of San Andreas, index
     * 1710), while the host found it - 221. */
    const gta_cmd *c = s->c;
    int i, n = s->n_cmds;
    for (i = 0; i < n; i++, c++) {
        long l = c->line;
        if (l == line)
            return i;
    }
    return -1;
}

/* THE CRANE'S INDEX: its place among the CRANE declarations, 0..3. */
static int crane_index(const gta_script *s, int line)
{
    int i, k = 0;
    for (i = 0; i < s->n; i++) {
        if (s->d[i].type != GTA_DECL_CRANE)
            continue;
        if (s->d[i].line == line)
            return k;
        k++;
    }
    return -1;
}

static int proc_start(gta_script *s, long line, int trigger);

int gta_script_debug_start(gta_script *s, int line)
{
    int r = proc_start(s, (long)line, -1);
    if (r < 0) {
        int k, busy = 0;
        for (k = 0; k < GTA_MAX_PROC; k++)
            if (s->proc[k].cmd >= 0) busy++;
        printf("gta: script - debug start of line %d failed: command index %d, "
               "%d of %d processes busy, %d commands, lines %ld %ld .. %ld, "
               "sizeof cmd %d\n", line,
               cmd_of_line(s, line), busy, GTA_MAX_PROC, s->n_cmds,
               s->n_cmds > 0 ? s->c[0].line : -1L,
               s->n_cmds > 1 ? s->c[1].line : -1L,
               s->n_cmds > 0 ? s->c[s->n_cmds - 1].line : -1L,
               (int)sizeof(gta_cmd));
        for (k = 0; k < s->n_cmds; k++)
            if (s->c[k].line >= line - 3 && s->c[k].line <= line + 3)
                printf("gta:   near: index %d line %ld op %d\n", k,
                       s->c[k].line, s->c[k].op);
        fflush(stdout);
    }
    return r;
}

static int proc_start(gta_script *s, long line, int trigger)
{
    int i, ci = cmd_of_line(s, line);
    if (ci < 0)
        return -1;
    for (i = 0; i < GTA_MAX_PROC; i++) {
        if (s->proc[i].cmd >= 0) continue;
        s->proc[i].cmd = (short)ci;
        s->proc[i].trigger = trigger;
        s->proc[i].parent = -1;
        s->proc[i].keep = 0;
        s->proc[i].kick_slot = 0;
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
        if (s->d[i].line == line)
            return &s->d[i];
    return 0;
}

/* WHERE A DECLARATION'S OBJECT ACTUALLY IS.
 *
 * gta_script_decl_pos answers where the FILE puts it, which is right until
 * something has been made from it and has moved. The original reads the
 * live record, so a check on a person who has walked away is a check on
 * where he walked to. Falls back to the declaration when nothing is alive. */
static int live_pos(gta_script *s, int line, long *wx, long *wy)
{
    gta_decl *d = decl_of(s, line);
    if (d && d->handle && s->world) {
        if (d->handle & GTA_IN_CAR) {
            if (s->world->car_pos &&
                s->world->car_pos(s->world_ctx, d->handle & ~GTA_IN_CAR,
                                  wx, wy))
                return 1;
        } else if (d->type == GTA_DECL_FUTUREPED || d->type == GTA_DECL_PED) {
            if (s->world->ped_pos &&
                s->world->ped_pos(s->world_ctx, d->handle, wx, wy))
                return 1;
        } else if (s->world->car_pos &&
                   s->world->car_pos(s->world_ctx, d->handle, wx, wy)) {
            return 1;
        }
    }
    return gta_script_decl_pos(s, line, wx, wy);
}

/* THE DROP-OFFS. One declaration, one object; DROP_ON puts it down and
 * KILL_DROP takes it away, and the same line twice is the same drop. */
static gta_placed *drop_of(gta_script *s, int line)
{
    int i;
    for (i = 0; i < s->n_drops; i++)
        if (s->drop[i].line == line)
            return &s->drop[i];
    return 0;
}

/* CAN THIS DECLARATION BE SWITCHED ON AND OFF? Not "has it got a state
 * already" - see the note at the head of the GUN_TRIG work. SPRAY is in the
 * list because DISABLE is how a level shuts a paint shop, even though the
 * scan that fires it is its own. */
static int switchable(int type)
{
    if (type == GTA_DECL_MOVING_TRIG || type == GTA_DECL_CORRECT_MOD_TRIG ||
        type == GTA_DECL_CORRECT_CAR_TRIG || type == GTA_DECL_MOVING_TRIG_HIRED ||
        type == GTA_DECL_CARBOMB_TRIG || type == GTA_DECL_DAMAGE_TRIG ||
        type == GTA_DECL_GUN_SCREEN_TRIG || type == GTA_DECL_CARDESTROY_TRIG ||
        type == GTA_DECL_CARWAIT_TRIG || type == GTA_DECL_PEDCAR_TRIG ||
        type == GTA_DECL_DUM_PED_BLOCK_TRIG || type == GTA_DECL_CARSTUCK_TRIG ||
        type == GTA_DECL_ALT_DAMAGE_TRIG)
        return 1;
    return type == GTA_DECL_TRIGGER || type == GTA_DECL_MPHONES ||
           type == GTA_DECL_CARTRIGGER ||
           type == GTA_DECL_SPRAY || type == GTA_DECL_GUN_TRIG ||
           type == GTA_DECL_DUM_MISSION_TRIG || type == GTA_DECL_PHONE_TOGG;
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
        if (s->phone[i].line == line)
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

/* SET_KILLTRIG's half of a trigger firing (224): end the process in slot
 * `kill_slot` of KICKSTART `kill_parent`. ALT_DAMAGE_TRIG never does - that
 * is the whole difference between it and DAMAGE_TRIG (0xf45f9). */
static void kill_on_fire(gta_script *s, const gta_decl *d)
{
    int k;
    if (!d->kill_parent || d->type == GTA_DECL_ALT_DAMAGE_TRIG)
        return;
    for (k = 0; k < GTA_MAX_PROC; k++)
        if (s->proc[k].cmd >= 0 && s->proc[k].parent == d->kill_parent &&
            s->proc[k].kick_slot == (unsigned char)d->kill_slot) {
            printf("gta: script - trigger %d kills process %d (slot %d of "
                   "%ld)\n", d->line, k, d->kill_slot, d->kill_parent);
            fflush(stdout);
            proc_end_rearm(s, k);
        }
}

/* ---- BATCH B helpers (222) ---- */

/* The car a line names: a car's own handle, or the car its driver sits in. */
static unsigned long car_handle(const gta_decl *d)
{
    if (!d || !d->handle) return 0;
    return d->handle & ~GTA_IN_CAR;
}

/* Every line that named `from` names `to` now - the driver got out (his
 * handle stops being the car's), or a ped got in (it starts being). */
static void rename_handle(gta_script *s, unsigned long from, unsigned long to)
{
    int k;
    for (k = 0; k < s->n; k++)
        if (s->d[k].handle == from) {
            s->d[k].handle = to;
            s->d[k].type = GTA_DECL_PED;
        }
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
            if (s->world && s->world->door) {
                s->world->door(s->world_ctx, d->line,
                               c->op == GTA_CMD_DOOR_ON ? GTA_DOOR_OP_ON
                                                        : GTA_DOOR_OP_OPEN);
            } else if (d->state != GTA_DOOR_OPEN) {
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
            if (s->world && s->world->door)
                s->world->door(s->world_ctx, d->line,
                               c->op == GTA_CMD_DOOR_OFF ? GTA_DOOR_OP_OFF
                                                         : GTA_DOOR_OP_CLOSE);
            else
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
            if (d && switchable(d->type)) {
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
            /* ENABLE SWITCHES A GATE ON for the types the original binds
             * (SPECIFIC_DOOR, SPECIFIC_BARR, BARRIER); MODEL_BARRIER and
             * BASIC_BARRIER take DOOR_ON instead (MISSIONS_DECLS_D.md 0.2).
             * With no door logic yet, "on" is open, as DOOR_ON makes it. */
            /* ...and a plain DOOR: MISSIONS_DECLS_D.md's table, "BARRIER
             * (and DOOR) - door on". Vice City's bomb shops are switched on
             * this way ([203] 30012 ENABLE 134) and stayed shut. */
            if (d && (d->type == GTA_DECL_SPECIFIC_DOOR ||
                      d->type == GTA_DECL_SPECIFIC_BARR ||
                      d->type == GTA_DECL_BARRIER ||
                      d->type == GTA_DECL_DOOR) &&
                d->state != GTA_DOOR_OPEN) {
                if (s->world && s->world->door)
                    s->world->door(s->world_ctx, d->line, GTA_DOOR_OP_ON);
                else
                    d->state = GTA_DOOR_OPEN;
                printf("gta: script - gate %d (%s) on at (%d,%d)\n", d->line,
                       gta_script_type_name(d->type), d->x, d->y);
                fflush(stdout);
            }
            if (d && switchable(d->type)) {
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
        /* AND IT INHERITS ITS PARENT'S PROTECTION - the original's case 0x59
         * copies the original's table[parent] into the child. Without that a kill
         * frenzy's FRENZY_CHECK loop, KICKSTARTed from a KF_PROCESS, is not
         * a frenzy process, RESET_KF does not end it, and it pays out
         * whenever the player's score next climbs by the target - long
         * after the frenzy was lost. */
        {
            int k = proc_start(s, c->p1, -1);
            if (k >= 0) {
                s->proc[k].parent = c->line;
                s->proc[k].keep = pr->keep;
                s->proc[k].kick_slot = 1;
            }
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
                if (s->c[k].line == c->p1) { t = &s->c[k]; break; }
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
                    s->proc[k].parent == c->p1) {
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

    case GTA_CMD_UNKNOWN_CHECK:
        advance(s, i, c->p3);
        return;

    case GTA_CMD_SCORE_CHECK:
        /* `8011 SCORE_CHECK 1000000 0 8010` - the gate on a job that opens
         * at a million points. `p1` is the threshold and the original also
         * marks the player as having passed it; nothing here reads that
         * mark yet. */
        advance(s, i, (s->world && s->world->score_now &&
                       s->world->score_now(s->world_ctx) >= c->p1)
                      ? c->p2 : c->p3);
        return;

    case GTA_CMD_DESTROY:
        /* IS_GOAL_DEAD without the "and this is who killed it" test, and
         * with the usual 5-based timeout on `p4`. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            int dead = 0;
            if (d && d->type == GTA_DECL_PLAYER)
                dead = s->world && s->world->player_dead &&
                       s->world->player_dead(s->world_ctx);
            else if (d && d->handle && s->world) {
                if (d->type == GTA_DECL_FUTUREPED || d->type == GTA_DECL_PED)
                    dead = s->world->ped_dead &&
                           s->world->ped_dead(s->world_ctx, d->handle);
                else
                    dead = s->world->car_dead &&
                           s->world->car_dead(s->world_ctx, d->handle);
            }
            if (dead) { advance_paid(s, i, c->p2, c->p5); return; }
            if (countdown_out(pr, c->p4)) advance(s, i, c->p3);
        }
        return;

    case GTA_CMD_GENERAL_ONSCREEN:
        /* IS THAT THING ON SCREEN? A WATCHER, and one whose default answer
         * has to be NO: it guards a KILL_PED, and "yes, the player can see
         * him" means "leave him alone" for ever. */
        {
            long wx = 0, wy = 0;
            int on = 0;
            if (s->world && s->world->onscreen && live_pos(s, (int)c->p1,
                                                           &wx, &wy))
                on = s->world->onscreen(s->world_ctx, wx, wy);
            advance(s, i, on ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_IS_PLAYER_ON_TRAIN:
        /* `6120 IS_PLAYER_ON_TRAIN 506 0 6120` - case 0x7d: aboard, the
         * DUMMY p1 takes the train's index, p5 is paid, on to p2; not
         * aboard, p3 (here itself: wait until he is). */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            int t = (s->world && s->world->player_train)
                  ? s->world->player_train(s->world_ctx) : -1;
            if (t >= 0) {
                if (d) d->handle = (unsigned long)t + 1;
                advance_paid(s, i, c->p2, c->p5);
            } else
                advance(s, i, c->p3);
        }
        return;

    case GTA_CMD_WRECK_A_TRAIN:
        /* `6160 WRECK_A_TRAIN 506 0 -1 -1 50000` - case 0x7e: wreck the
         * train in DUMMY p1, pay p5, on to p2 (no failure path). */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->wreck_train)
                s->world->wreck_train(s->world_ctx, (int)d->handle - 1);
            printf("gta: script - WRECK_A_TRAIN %ld\n", (long)c->p1);
            fflush(stdout);
            advance_paid(s, i, c->p2, c->p5);
        }
        return;

    case GTA_CMD_PLAYER_ARE_BOTH_ONSCREEN:
        /* `1180 PLAYER_ARE_BOTH_ONSCREEN 1000 0 1210` - the original's case
         * 0x54: the car (or the car the person p1 sits in) inside the
         * player's view rectangle (the original's routine) pays p5 and goes to p2;
         * out of it, p3. Vice City 2's first job blows the car up only
         * while the player can see it. */
        {
            long wx = 0, wy = 0;
            int on = 0;
            if (s->world && s->world->onscreen && live_pos(s, (int)c->p1,
                                                           &wx, &wy))
                on = s->world->onscreen(s->world_ctx, wx, wy);
            if (on) advance_paid(s, i, c->p2, c->p5);
            else    advance(s, i, c->p3);
        }
        return;

    case GTA_CMD_CHANGE_PED_TYPE:
        /* `520 CHANGE_PED_TYPE 299 0 -1 21 294` - person 299 becomes AI type
         * 21 with the PLAYER as his target. `p4` is the type and `p5` the
         * target's declaration; the original's attacking family is
         * 0x15..0x2e and they all take one. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->ped_type) {
                long tx = 0, ty = 0;
                int on_player = 0;
                if (c->p5 > 0) {
                    gta_decl *t = decl_of(s, (int)c->p5);
                    if (t && t->type == GTA_DECL_PLAYER)
                        on_player = 1;
                    live_pos(s, (int)c->p5, &tx, &ty);
                }
                {
                gta_decl *tc = c->p5 > 0 ? decl_of(s, (int)c->p5) : 0;
                if (c->p4 == 2 && tc && tc->handle &&
                    tc->type != GTA_DECL_PED && tc->type != GTA_DECL_FUTUREPED &&
                    tc->type != GTA_DECL_PLAYER && s->world->ped_sendto) {
                    /* TYPE 2 = get into car p5 (DUMMY_DRIVE_ON waits for
                     * it): he walks to the car, and the board takes him */
                    s->world->ped_sendto(s->world_ctx, d->handle, tx, ty);
                } else
                s->world->ped_type(s->world_ctx, d->handle, (int)c->p4,
                                   tx, ty, on_player);
                }
            }
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_REMAP_PED:
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->ped_remap)
                s->world->ped_remap(s->world_ctx, d->handle, (int)c->p4);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_PED_SENDTO:
        /* `778 PED_SENDTO 251 0 0 249` - p1 is WHERE and p4 is WHO. It is an
         * order, not a wait: the command advances at once. */
        {
            gta_decl *ped = decl_of(s, (int)c->p4);
            long wx = 0, wy = 0;
            if (ped && ped->handle && s->world && s->world->ped_sendto &&
                gta_script_decl_pos(s, (int)c->p1, &wx, &wy))
                s->world->ped_sendto(s->world_ctx, ped->handle, wx, wy);
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_WAIT_FOR_PED:
        /* ...and this is the wait. The original has no failure branch at
         * all: it stays on this line until he arrives. */
        {
            gta_decl *ped = decl_of(s, (int)c->p4);
            long wx = 0, wy = 0;
            if (!ped || !ped->handle) { advance(s, i, c->p2); return; }
            if (s->world && s->world->ped_at &&
                gta_script_decl_pos(s, (int)c->p1, &wx, &wy) &&
                s->world->ped_at(s->world_ctx, ped->handle, wx, wy)) {
                advance_paid(s, i, c->p2, c->p5);
                return;
            }
            /* ONCE A SECOND, WHERE HE IS AND WHERE HE SHOULD BE. A wait with
             * no timeout is invisible when it goes wrong - it looks exactly
             * like a job that is still in progress - so it says so. */
            if (++pr->count2 >= 25) {
                long hx = 0, hy = 0;
                int got = s->world && s->world->ped_pos &&
                          s->world->ped_pos(s->world_ctx, ped->handle,
                                            &hx, &hy);
                pr->count2 = 0;
                printf("gta: script - WAIT_FOR_PED %d: he is %s(%ld,%ld), "
                       "wanted (%ld,%ld)\n", ped->line,
                       got ? "" : "GONE ", hx >> 16, hy >> 16,
                       wx >> 16, wy >> 16);
                fflush(stdout);
            }
        }
        return;

    case GTA_CMD_HELL_ON:
        /* `545 HELL_ON 221` - the same placement PARKED_ON does, with two
         * differences that are the original's: the declared model is
         * IGNORED in favour of its own, and somebody is sitting in it. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (!d) { advance(s, i, c->p3); return; }
            if (!d->handle && s->world && s->world->car_on_driven)
                d->handle = s->world->car_on_driven(s->world_ctx, d->line,
                                                    GTA_HELL_MODEL,
                                                    d->x, d->y,
                                                    gta_script_angle(d->b));
            advance(s, i, d->handle ? c->p2 : c->p3);
        }
        return;

    case GTA_CMD_DROP_WANTED:
        if (s->world && s->world->drop_wanted)
            s->world->drop_wanted(s->world_ctx);
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_DROP_ON:
        /* `755 DROP_ON 240` - a map object of the declaration's own model,
         * at its block. The arrow points at it and GOTO_DROPOFF is getting
         * there. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            gta_placed *dr = d ? drop_of(s, d->line) : 0;
            if (d && !dr && s->n_drops < GTA_MAX_DROPS) {
                long wx = 0, wy = 0;
                int lz;
                gta_script_decl_pos(s, (int)c->p1, &wx, &wy);
                dr = &s->drop[s->n_drops++];
                dr->x = wx;
                dr->y = wy;
                dr->line = d->line;
                lz = gta_script_stand_layer(s->nav, (int)(wx >> 21),
                                            (int)(wy >> 21));
                dr->layer = (unsigned char)(lz < 0 ? 0 : lz);
                dr->angle = (unsigned char)gta_script_angle(d->b);
                dr->ring = 0;
                dr->dead = 0;
                dr->spr = (short)(s->tiles
                                  ? gta_tiles_object_sprite(s->tiles, d->a)
                                  : -1);
                printf("gta: script - drop-off %d at (%ld,%ld), model %d, "
                       "sprite %d\n", d->line, wx >> 16, wy >> 16, d->a,
                       dr->spr);
                fflush(stdout);
            } else if (dr) {
                dr->dead = 0;
            }
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    /* THE KILL FRENZY, read out of the interpreter's own cases (0x67, 0x68,
     * 0x86, 0x8d, 0x8e of the original's routine), not out of the names:
     *
     *   9001 KF_PROCESS            this process is a frenzy's: RESET leaves it
     *                              alone and RESET_KF is what ends it
     *   9015 FRENZY_SET 9030       write the player's SCORE, as it stands now,
     *                              into the `p4` of the command at line 9030
     *   9030 FRENZY_CHECK 1000 0 9030 0 20000
     *                              score - p4 < p1: not yet, go to p3 (itself);
     *                              otherwise the frenzy is won - p2, paying p5
     *   9042 STOP_FRENZY           the endless weapon goes, the old ones return
     *   9044 RESET_KF              every OTHER process marked by KF_PROCESS is
     *                              killed (and the frenzy's cars are unlocked,
     *                              which this port does not lock yet)
     *
     * So a frenzy is "earn p1 points before the SURVIVE beside it runs out",
     * and the weapon is simply the crate's own: `271 POWERUP 2 500` is a
     * machine gun for 400 ticks, which PROGRESS 169 already does. */
    /* THE BANK JOB (the original's routine cases 0x4c, 0x50, 0x51).
     *
     *   278 BANK_ALARM_ON 0 0 -1 241 0   the bell is the DUMMY in p4 - not
     *                                    p1 - and it moves on without paying
     *   350 BANK_ROBBERY 253 0 0 241 0   heat +1000 (cap 2000) and the wanted
     *                                    level straight to 4; p1 is the
     *                                    getaway car, which the original
     *                                    also gives a sound event (9) - not
     *                                    ported, nothing here consumes it
     *   365 BANK_ALARM_OFF 241 0 0 0 0   the bell of the DUMMY in p1 stops;
     *                                    the generic success path, pays p5 */
    /* THE BOMB JOB (cases 0x18, 0x22, 0x23, 0x5c, 0x5d, 0x5e):
     *
     *   3516 FREEZE_ENTER            he cannot get out of the car
     *   3540 SETBOMB 297 0 0 6 0     a speed bomb in car 297, already armed
     *   3550 ARMEDMESS               "bomb_on" on the card
     *   3570 SETBOMB 297 0 0 0 20000 disarmed - and it pays
     *   3580 UNFREEZE_ENTER / 3590 DISARMMESS
     *
     * All of them ACTIONS on the generic success path. */
    /* FOUR SMALL ONES (cases 10, 0x2b, 0x32, 0x7c).
     *
     *   8180 END 1 0 0 1 50000   the process stops. The original also writes
     *                            p1 into a per-process slot that nothing
     *                            outside the interpreter ever reads.
     *   816 KILL_OBJ 140         the object goes (its record becomes type
     *                            0x65); here the FUTURE objects are the
     *                            placed ones, so it is KILL_DROP's work
     *   5271 KILL_CAR 485        the car is taken out of the world
     *   1251 SET_PED_SPEED 237 0 0 3   the pace PED_SENDTO walks him at */
    case GTA_CMD_END:
        /* NOT a value nobody reads, as 187 said: the interpreter's own
         * end-of-tick code (MISSIONS.md 4.5) takes it once the player has no
         * process left and ends the LEVEL with it - 1 passed, 2 failed. The
         * process chain that reached END is the level's last, so the level
         * ends here. */
        printf("gta: script - END %ld at line %d - the level is over\n",
               c->p1, (int)c->line);
        proc_end(s, i);
        if (s->world && s->world->level_end)
            s->world->level_end(s->world_ctx, (int)c->p1);
        return;

    case GTA_CMD_KILL_OBJ:
        {
            gta_placed *dr = drop_of(s, (int)c->p1);
            if (dr) dr->dead = 1;
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    /* BATCH A - the other levels' actions and one check, read out of
     * the original's routine (the research notes). Every one pays p5 and
     * goes on to p2 in the same tick, bar IS_PED_STUNNED. */
    case GTA_CMD_MAKEOBJ:
        /* a map object of the FUTURE's type, at its pixel position, turned
         * by p4; from now on the line names that object (ARROW, LOCATE,
         * KILL_OBJ) - the port's placed objects are DROP_ON's */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            gta_placed *dr = d ? drop_of(s, d->line) : 0;
            if (d && !dr && s->n_drops < GTA_MAX_DROPS) {
                long wx = 0, wy = 0;
                int lz;
                gta_script_decl_pos(s, (int)c->p1, &wx, &wy);
                dr = &s->drop[s->n_drops++];
                dr->x = wx;
                dr->y = wy;
                dr->line = d->line;
                lz = gta_script_stand_layer(s->nav, (int)(wx >> 21),
                                            (int)(wy >> 21));
                dr->layer = (unsigned char)(lz < 0 ? 0 : lz);
                dr->angle = (unsigned char)gta_script_angle((int)c->p4);
                dr->ring = 0;
                dr->dead = 0;
                dr->spr = (short)(s->tiles
                                  ? gta_tiles_object_sprite(s->tiles, d->a)
                                  : -1);
                printf("gta: script - MAKEOBJ %d at (%ld,%ld), type %d, "
                       "sprite %d\n", d->line, wx >> 16, wy >> 16, d->a,
                       dr->spr);
                fflush(stdout);
            } else if (dr) {
                dr->dead = 0;
            }
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_EXPLODE_CAR:
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->car_explode)
                s->world->car_explode(s->world_ctx, d->handle);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_CHANGE_BLOCK:
    case GTA_CMD_CHANGE_TYPE:
        /* a BLOCK_INFO in block coordinates: the type word a (CHANGE_BLOCK),
         * or face a := tile b (CHANGE_TYPE: 0 W, 1 E, 2 N, 3 S, 4 lid) */
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            if (d && s->world) {
                if (c->op == GTA_CMD_CHANGE_BLOCK && s->world->map_type)
                    s->world->map_type(s->world_ctx, d->x, d->y, d->z,
                                       (long)(unsigned short)d->a);
                else if (c->op == GTA_CMD_CHANGE_TYPE && s->world->map_face)
                    s->world->map_face(s->world_ctx, d->x, d->y, d->z,
                                       d->a, d->b & 0xff);
            }
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_EXPL_PED:
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            if (d && s->world && s->world->ped_explode) {
                if (d->type == GTA_DECL_PLAYER)
                    s->world->ped_explode(s->world_ctx, 0);
                else if (d->handle)
                    s->world->ped_explode(s->world_ctx, d->handle);
            }
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_SET_NO_COLLIDE:
    case GTA_CMD_SET_COLLIDE:
        /* "cannot be damaged or shoved", not "no collision" */
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->car_invuln)
                s->world->car_invuln(s->world_ctx, d->handle,
                                     c->op == GTA_CMD_SET_NO_COLLIDE);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_PED_WEAPON:
    case GTA_CMD_PED_POLICE:
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            if (d && d->handle && d->type != GTA_DECL_PLAYER && s->world) {
                if (c->op == GTA_CMD_PED_WEAPON && s->world->ped_weapon)
                    s->world->ped_weapon(s->world_ctx, d->handle, (int)c->p4);
                else if (c->op == GTA_CMD_PED_POLICE && s->world->ped_police)
                    s->world->ped_police(s->world_ctx, d->handle);
            }
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_ADD_A_LIFE:
        if (s->world && s->world->add_life)
            s->world->add_life(s->world_ctx);
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_INC_HEADS:
        if (s->world && s->world->inc_heads)
            s->world->inc_heads(s->world_ctx, (int)c->p1);
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_IS_PED_STUNNED:
        /* the one check: lying on the ground alive -> p2 (paid), else p3,
         * which the scripts point back at the line itself to wait */
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            int down = d && d->handle && d->type != GTA_DECL_PLAYER &&
                       s->world && s->world->ped_stunned &&
                       s->world->ped_stunned(s->world_ctx, d->handle);
            if (down) advance_paid(s, i, c->p2, c->p5);
            else      advance(s, i, c->p3);
        }
        return;

    /* BATCH B - positions, dummy cars, drivers (the research notes) */
    case GTA_CMD_LOCATE:
        /* the player ON FOOT within p5 of the original's pixels (0 = 16) of
         * p1; p4 the usual 5-based timeout. p5 is a radius, not a score. */
        {
            long wx = 0, wy = 0, r, dx, dy;
            int out = countdown_out(pr, c->p4);
            r = (c->p5 > 0 ? c->p5 : 16) / 2;       /* 64 a block -> 32 */
            if (r < 10) r = 10;
            if (s->on_foot && live_pos(s, (int)c->p1, &wx, &wy)) {
                dx = s->px - wx; if (dx < 0) dx = -dx;
                dy = s->py - wy; if (dy < 0) dy = -dy;
                if (dx < (r << 16) && dy < (r << 16)) {
                    printf("gta: script - LOCATE %ld reached\n", c->p1);
                    fflush(stdout);
                    advance(s, i, c->p2);
                    return;
                }
            }
            if (out) advance(s, i, c->p3);
        }
        return;

    case GTA_CMD_GOTO:
        /* the player - in a car or not - in p1's BLOCK, x and y only */
        {
            long wx = 0, wy = 0;
            int out = countdown_out(pr, c->p4);
            if (live_pos(s, (int)c->p1, &wx, &wy) &&
                (s->px >> 21) == (wx >> 21) && (s->py >> 21) == (wy >> 21)) {
                printf("gta: script - GOTO %ld reached\n", c->p1);
                fflush(stdout);
                advance_paid(s, i, c->p2, c->p5);
                return;
            }
            if (out) advance(s, i, c->p3);
        }
        return;

    case GTA_CMD_LOCK_DOOR:
    case GTA_CMD_UNLOCK_DOOR:
        {
            unsigned long h = car_handle(decl_of(s, (int)c->p1));
            if (h && s->world && s->world->car_lock)
                s->world->car_lock(s->world_ctx, h,
                                   c->op == GTA_CMD_LOCK_DOOR);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_DUMMYON:
        {
            unsigned long h = car_handle(decl_of(s, (int)c->p1));
            if (h && s->world && s->world->car_wake)
                s->world->car_wake(s->world_ctx, h);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_SENDTO:
        /* car p1 drives itself by road to p4's block; the arrival is the
         * DUM_MISSION_TRIG's business, not this command's */
        {
            unsigned long h = car_handle(decl_of(s, (int)c->p1));
            long wx = 0, wy = 0;
            if (h && s->world && s->world->car_sendto &&
                live_pos(s, (int)c->p4, &wx, &wy)) {
                s->world->car_sendto(s->world_ctx, h, wx, wy);
                printf("gta: script - SENDTO car %ld -> line %ld at (%ld,%ld)\n",
                       c->p1, c->p4, wx >> 21, wy >> 21);
                fflush(stdout);
            }
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_GET_DRIVER_INFO:
        /* line p4 := the driver of car p1, or p3 when nobody is in it */
        {
            unsigned long h = car_handle(decl_of(s, (int)c->p1));
            gta_decl *dst = decl_of(s, (int)c->p4);
            if (h && dst && s->world && s->world->car_has_driver &&
                s->world->car_has_driver(s->world_ctx, h)) {
                dst->handle = h | GTA_IN_CAR;
                dst->type = GTA_DECL_PED;
                advance_paid(s, i, c->p2, c->p5);
            } else {
                advance(s, i, c->p3);
            }
        }
        return;

    case GTA_CMD_PED_OUT_OF_CAR:
        /* p1 a car, or a ped GET_DRIVER_INFO named: the driver gets out and
         * every line that named him in the car names him on foot */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            int is_ped = d && (d->type == GTA_DECL_PED ||
                               d->type == GTA_DECL_FUTUREPED);
            unsigned long h;
            long r;
            if (!d || !d->handle) return;            /* stays, as the original */
            if (is_ped && !(d->handle & GTA_IN_CAR)) {
                advance_paid(s, i, c->p2, c->p5);    /* not in a car: done */
                return;
            }
            h = car_handle(d);
            r = (s->world && s->world->driver_out)
                    ? s->world->driver_out(s->world_ctx, h) : -1;
            if (r == 0) return;                      /* not yet */
            if (r > 0) {
                rename_handle(s, h | GTA_IN_CAR, (unsigned long)r);
                printf("gta: script - PED_OUT_OF_CAR %ld: ped %ld out\n",
                       c->p1, r);
                fflush(stdout);
                advance_paid(s, i, c->p2, c->p5);
            } else if (is_ped) {
                advance_paid(s, i, c->p2, c->p5);
            } else {
                advance(s, i, c->p3);
            }
        }
        return;

    case GTA_CMD_DUMMY_DRIVE_ON:
        /* waits, for ever, until a ped has climbed into car p1 - then it
         * drives off */
        {
            unsigned long h = car_handle(decl_of(s, (int)c->p1));
            long r = (h && s->world && s->world->car_board)
                         ? s->world->car_board(s->world_ctx, h) : 0;
            if (r == 0) return;
            if (r > 0)
                rename_handle(s, (unsigned long)r, h | GTA_IN_CAR);
            printf("gta: script - DUMMY_DRIVE_ON %ld: driving off\n", c->p1);
            fflush(stdout);
            advance_paid(s, i, c->p2, c->p5);
        }
        return;

    case GTA_CMD_GET_CAR_INFO:
        /* line p4 := the car ped p1 (the player, in every use) is in */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            gta_decl *dst = decl_of(s, (int)c->p4);
            unsigned long h = 0;
            if (d && d->type == GTA_DECL_PLAYER) {
                if (s->world && s->world->player_car)
                    h = s->world->player_car(s->world_ctx);
            } else if (d && (d->handle & GTA_IN_CAR)) {
                h = car_handle(d);
            }
            if (h && dst) {
                dst->handle = h;
                if (dst->type == GTA_DECL_PED || dst->type == GTA_DECL_DUMMY)
                    dst->type = GTA_DECL_FUTURECAR;
                advance_paid(s, i, c->p2, c->p5);
            } else {
                advance(s, i, c->p3);
            }
        }
        return;

    case GTA_CMD_PIXEL_CAR_ON:
    case GTA_CMD_PARKED_PIXELS_ON:
        /* a car of model a, turned b, at p1's PIXEL position, paint p4 -
         * with a driver sitting in it for PIXEL_CAR_ON */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            long wx = 0, wy = 0;
            if (d && !d->handle && s->world && s->world->car_on_px &&
                gta_script_decl_pos(s, (int)c->p1, &wx, &wy))
                d->handle = s->world->car_on_px(s->world_ctx, d->line, d->a,
                                                wx, wy, gta_script_angle(d->b),
                                                (int)c->p4,
                                                c->op == GTA_CMD_PIXEL_CAR_ON);
            if (d && d->handle) advance_paid(s, i, c->p2, c->p5);
            else if (c->op == GTA_CMD_PIXEL_CAR_ON) advance(s, i, c->p3);
        }
        return;

    /* BATCH C, part 1 (the research notes) */
    case GTA_CMD_NEXT_KICK:
        /* another child for KICKSTART p4, at line p1 (0 = the next command),
         * so one KILL_SIDE_PROC p4 stops them all */
        {
            long start = c->p1 ? c->p1
                       : (pr->cmd + 1 < s->n_cmds ? s->c[pr->cmd + 1].line : -1);
            int k, slot = 1, j;
            for (j = 0; j < GTA_MAX_PROC; j++)
                if (s->proc[j].cmd >= 0 && s->proc[j].parent == c->p4 &&
                    s->proc[j].kick_slot >= slot)
                    slot = s->proc[j].kick_slot + 1;
            k = start > 0 ? proc_start(s, start, -1) : -1;
            if (k >= 0) {
                s->proc[k].parent = c->p4;
                s->proc[k].keep = pr->keep;
                s->proc[k].kick_slot = (unsigned char)slot;
            }
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_SET_KILLTRIG:
        /* p1 the trigger, p4 a KICKSTART, p5 the SLOT (not a score): 1..4,
         * and 5 - a slot nothing fills - clears it */
        {
            gta_decl *t = decl_of(s, (int)c->p1);
            if (t) {
                t->kill_parent = (c->p5 >= 1 && c->p5 <= 4) ? c->p4 : 0;
                t->kill_slot = (signed char)c->p5;
            }
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_FREEUP_CAR:
        /* the mission lets go of it: ordinary traffic again */
        {
            unsigned long h = car_handle(decl_of(s, (int)c->p1));
            if (h && s->world && s->world->car_release)
                s->world->car_release(s->world_ctx, h);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_HUNTON:
    case GTA_CMD_HUNTOFF:
        /* p3 is the MODE, not a failure line; the target is always the
         * player in the shipped script */
        {
            unsigned long h = car_handle(decl_of(s, (int)c->p1));
            if (h && s->world && s->world->car_hunt)
                s->world->car_hunt(s->world_ctx, h,
                                   c->op == GTA_CMD_HUNTON
                                       ? (c->p3 > 0 ? (int)c->p3 : 1) : 0);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_MODEL_HUNT:
        /* p4 bomb cars (model 47) with drivers at FUTURECAR lines p1..,
         * hunting the player, kamikaze. This process then stays here for
         * good, turning wrecked hunters off; with p2 = 0 the script goes on
         * in a child at the next command. */
        if (pr->count2 != 5) {
            long k;
            for (k = 0; k < c->p4; k++) {
                gta_decl *dd = decl_of(s, (int)(c->p1 + k));
                if (!dd || dd->handle || !s->world || !s->world->car_on)
                    continue;
                dd->handle = s->world->car_on(s->world_ctx, dd->line, 47,
                                              dd->x, dd->y, 0);
                if (dd->handle) {
                    if (s->world->setbomb)
                        s->world->setbomb(s->world_ctx, dd->handle, 3);
                    if (s->world->car_hunt)
                        s->world->car_hunt(s->world_ctx, dd->handle, 3);
                }
            }
            if (c->p2 != -1 && pr->cmd + 1 < s->n_cmds) {
                int ch = proc_start(s, s->c[pr->cmd + 1].line, -1);
                if (ch >= 0) s->proc[ch].keep = pr->keep;
            }
            pr->count2 = 5;
            pr->fresh = 0;
            return;
        }
        {
            long k;
            for (k = 0; k < c->p4; k++) {
                gta_decl *dd = decl_of(s, (int)(c->p1 + k));
                if (dd && dd->handle && s->world && s->world->car_dead &&
                    s->world->car_dead(s->world_ctx, dd->handle) &&
                    s->world->car_hunt)
                    s->world->car_hunt(s->world_ctx, dd->handle, 0);
            }
        }
        return;

    case GTA_CMD_SETUP_REPO:
        /* the car p1 remembers its owner, FUTUREPED p4 (in the rotation
         * field, as the original does - PARKED_ON has read it already) */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d) d->b = (short)c->p4;
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_DO_REPO:
        /* waits for the player to be driving car p1 (pays p5). After p4
         * ticks - counted down IN THE COMMAND, as the original does - the
         * owner walks back, gets in and drives it away; the player can still
         * steal it back. */
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            unsigned long h = car_handle(d);
            gta_decl *own = d ? decl_of(s, d->b) : 0;
            long wx = 0, wy = 0;
            if (h && s->world && s->world->player_car &&
                s->world->player_car(s->world_ctx) == h) {
                advance_paid(s, i, c->p2, c->p5);
                return;
            }
            if (!h || !s->world) return;
            if (c->p4 > 0 && --c->p4 == 0) {
                if (own && !own->handle && s->world->ped_on &&
                    gta_script_decl_pos(s, own->line, &wx, &wy))
                    own->handle = s->world->ped_on(s->world_ctx, own->line,
                                                   wx, wy, 0);
                if (own && own->handle && s->world->ped_sendto &&
                    s->world->car_pos &&
                    s->world->car_pos(s->world_ctx, h, &wx, &wy))
                    s->world->ped_sendto(s->world_ctx, own->handle, wx, wy);
                printf("gta: script - DO_REPO %ld: the owner comes back\n",
                       c->p1);
                fflush(stdout);
                pr->count2 = 2;
                return;
            }
            if (pr->count2 == 2 && own && own->handle &&
                !(own->handle & GTA_IN_CAR) && s->world->ped_board &&
                s->world->ped_board(s->world_ctx, own->handle, h)) {
                rename_handle(s, own->handle, h | GTA_IN_CAR);
                if (s->world->car_wake)
                    s->world->car_wake(s->world_ctx, h);
                pr->count2 = 3;
            }
        }
        return;

    case GTA_CMD_OBTAIN:
    case GTA_CMD_THROW:
        /* the money bag the player holds and throws at the boat: the pay
         * and the flow are the original's, the bag itself is not drawn */
        advance_paid(s, i, c->p2, c->p5);
        return;

    /* BATCH C, part 2 - THE MODEL CAR (the research notes) */
    case GTA_CMD_START_MODEL:
        /* waits until he is driving, then swaps him into a bomb car. p5 is
         * how many model cars are left - spent here, not paid */
        if (c->p5 == 0) { advance(s, i, c->p3); return; }
        if (s->world && s->world->model_start &&
            s->world->model_start(s->world_ctx)) {
            c->p5--;
            printf("gta: script - START_MODEL %ld: %ld model cars left\n",
                   c->line, c->p5);
            fflush(stdout);
            advance(s, i, c->p2);
        }
        return;

    case GTA_CMD_DO_MODEL:
        /* the session's supervisor: p2 some 100 ticks after target p4 is
         * wrecked, p3 when he gets out or the last model car is spent; a
         * spent one is replaced after 100 ticks while START_MODEL p1 has
         * any left. No score on any path. */
        {
            int sm = cmd_of_line(s, c->p1), st;
            unsigned long th = c->p4 >= 0 ? car_handle(decl_of(s, (int)c->p4)) : 0;
            if (!s->world || !s->world->model_state) { advance(s, i, c->p3); return; }
            if ((s->world->player_dead && s->world->player_dead(s->world_ctx)) ||
                (s->world->player_arrested &&
                 s->world->player_arrested(s->world_ctx))) {
                if (s->world->model_return)
                    s->world->model_return(s->world_ctx);
                proc_end_rearm(s, i);
                return;
            }
            if (th && s->world->car_dead &&
                s->world->car_dead(s->world_ctx, th)) {
                if (pr->count2++ > 99)
                    advance(s, i, c->p2);
                return;
            }
            st = s->world->model_state(s->world_ctx);
            if (st == GTA_RC_CANCEL) { advance(s, i, c->p3); return; }
            if (st == GTA_RC_DEAD) {
                if (pr->count++ <= 99) return;
                pr->count = 0;
                if (sm >= 0 && s->c[sm].p5 > 0 && s->world->model_start &&
                    s->world->model_start(s->world_ctx)) {
                    s->c[sm].p5--;
                    printf("gta: script - DO_MODEL: another model car, %ld "
                           "left\n", s->c[sm].p5);
                    fflush(stdout);
                    return;
                }
                advance(s, i, c->p3);
            }
        }
        return;

    case GTA_CMD_RETURN_CONTROL:
        if (s->world && s->world->model_return)
            s->world->model_return(s->world_ctx);
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_KILL_CAR:
        {
            gta_decl *d = decl_of(s, (int)c->p1);
            if (d && d->handle) {
                if (s->world && s->world->car_kill)
                    s->world->car_kill(s->world_ctx, d->handle);
                d->handle = 0;
            }
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_SET_PED_SPEED:
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->ped_speed)
                s->world->ped_speed(s->world_ctx, d->handle, (int)c->p4);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    /* THE SECOND ARROW (the original's routine / the original's routine / the original's routine). The
     * same sprite as the first, remapped with the table the original gives
     * player 2 (the original's table[2] = 3), and it always points at a POINT: the
     * object's position when the command runs - where a ped IS, not where
     * he was declared. `RED_ARROW -1` does not touch the red one at all: it
     * puts the ordinary arrow out (the original calls the original's routine there). */
    case GTA_CMD_RED_ARROW:
        if (c->p1 < 0) {
            if (s->world && s->world->arrow)
                s->world->arrow(s->world_ctx, 0, 0, 0);
        } else {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            long wx = 0, wy = 0;
            int ok = 0;
            if (d && d->handle && s->world && s->world->ped_pos &&
                (d->type == GTA_DECL_PED || d->type == GTA_DECL_FUTUREPED))
                ok = s->world->ped_pos(s->world_ctx, d->handle, &wx, &wy);
            if (!ok)
                ok = gta_script_decl_pos(s, (int)c->p1, &wx, &wy);
            if (ok && s->world && s->world->red_arrow)
                s->world->red_arrow(s->world_ctx, 1, wx, wy);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_RED_ARROW_OFF:
        if (s->world && s->world->red_arrow)
            s->world->red_arrow(s->world_ctx, 0, 0, 0);
        advance_paid(s, i, c->p2, c->p5);
        return;

    /* THE DOCK CRANES (the original's routine, the original's routine, the original's routine, and the
     * crane state machine in the tail of the original's routine).
     *
     *   1 SURVIVE 0 0 30 30 0
     *   2 CRANE 285 -1 -1 286 0     crane 285, the car must stand on the block
     *                               of TRIGGER 286; p2 done, p3 refused
     *   1509 DO_GTA 270 0 -1 0 20000   GTA_DEMAND 270 = "0 22 -1 2": crane 0
     *                               wants two cars of model 22, any colour;
     *                               the job waits until it has had them
     *
     * CRANE is two-phase: the first tick OFFERS the car (and the refusals
     * are texts of their own), then the command stays until the crane has
     * lifted it and paid - which is the world's business. */
    case GTA_CMD_CRANE:
        {
            int k = crane_index(s, (int)c->p1);
            const gta_decl *t = gta_script_by_line(s, (int)c->p4);
            if (k < 0 || !t || !s->world || !s->world->crane_offer) {
                advance(s, i, c->p3);
                return;
            }
            if (pr->count == 0) {
                int r = s->world->crane_offer(s->world_ctx, k, t->x, t->y);
                if (r == 0) { pr->count = 1; return; }     /* taken: wait */
                advance(s, i, c->p3);
                return;
            }
            {
                int r = s->world->crane_poll(s->world_ctx, k);
                if (r == 0) return;
                if (r == 1) advance_paid(s, i, c->p2, c->p5);
                else        advance(s, i, c->p3);
            }
        }
        return;

    case GTA_CMD_DO_GTA:
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            if (!d || !s->world || !s->world->crane_demand) {
                advance(s, i, c->p3);
                return;
            }
            if (pr->count == 0) {
                s->world->crane_demand(s->world_ctx, d->a, d->b, d->c, d->d);
                pr->count = 1;
                return;
            }
            if (s->world->crane_demand(s->world_ctx, d->a, 0, 0, 0))
                advance_paid(s, i, c->p2, c->p5);
        }
        return;

    case GTA_CMD_SETBOMB:
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            if (d && d->handle && s->world && s->world->setbomb)
                s->world->setbomb(s->world_ctx, d->handle, (int)c->p4);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_ARMEDMESS:
    case GTA_CMD_DISARMMESS:
        if (s->world && s->world->named_text)
            s->world->named_text(s->world_ctx,
                                 c->op == GTA_CMD_ARMEDMESS ? "bomb_on" : "bomb_off");
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_FREEZE_ENTER:
    case GTA_CMD_UNFREEZE_ENTER:
    case GTA_CMD_FREEZE_TIMED:
        if (s->world && s->world->freeze)
            s->world->freeze(s->world_ctx, c->op == GTA_CMD_FREEZE_ENTER,
                             c->op == GTA_CMD_FREEZE_TIMED ? (int)c->p1 : 0);
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_BANK_ALARM_ON:
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p4);
            if (d && s->world && s->world->alarm)
                s->world->alarm(s->world_ctx, d->line, 1, d->x, d->y);
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_BANK_ALARM_OFF:
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            if (d && s->world && s->world->alarm)
                s->world->alarm(s->world_ctx, d->line, 0, d->x, d->y);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_BANK_ROBBERY:
        if (s->world && s->world->robbery)
            s->world->robbery(s->world_ctx);
        advance_paid(s, i, c->p2, c->p5);
        return;

    /* THE FRENZY'S CLOCKS. KF_BRIEF_TIMED (the original's routine case 0x89) starts
     * the frenzy's countdown, p4 seconds at 25 ticks a second, and says
     * "Kill frenzy!" (the original's routine(3)); the other three set or clear one of
     * the two clocks (MISSIONS.md, the table in section [1]). None of them
     * decides anything - the SURVIVE beside the frenzy does the timing - they
     * are what the player SEES and HEARS of it. */
    case GTA_CMD_KF_BRIEF_TIMED:
    case GTA_CMD_KF_CANCEL_BRIEFING:
    case GTA_CMD_KF_BRIEF_GENERAL:
    case GTA_CMD_KF_CANCEL_GENERAL:
        if (s->world && s->world->kf_timer) {
            int timed = c->op == GTA_CMD_KF_BRIEF_TIMED
                     || c->op == GTA_CMD_KF_CANCEL_BRIEFING;
            int on = c->op == GTA_CMD_KF_BRIEF_TIMED
                  || c->op == GTA_CMD_KF_BRIEF_GENERAL;
            s->world->kf_timer(s->world_ctx, timed ? 0 : 1,
                               on ? c->p4 * 25 : -1);
        }
        if (c->op == GTA_CMD_KF_BRIEF_TIMED && s->world && s->world->say)
            s->world->say(s->world_ctx, GTA_VOICE_FRENZY);
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_KF_PROCESS:
        pr->keep = 2;
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_FRENZY_SET:
        {
            int ci = cmd_of_line(s, c->p1);
            long now = (s->world && s->world->score_now)
                     ? s->world->score_now(s->world_ctx) : 0;
            if (ci >= 0)
                s->c[ci].p4 = now;
            printf("gta: script - FRENZY_SET: score %ld written into line %ld\n",
                   now, c->p1);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_FRENZY_CHECK:
        {
            long now = (s->world && s->world->score_now)
                     ? s->world->score_now(s->world_ctx) : 0;
            if (now - c->p4 < c->p1) { advance(s, i, c->p3); return; }
            printf("gta: script - FRENZY_CHECK: %ld points since the start, "
                   "%ld asked - the frenzy is WON\n", now - c->p4, c->p1);
        }
        /* the original's routine case 0x68, the won branch: the original's routine(4) */
        if (s->world && s->world->say)
            s->world->say(s->world_ctx, GTA_VOICE_FRENZY_PASSED);
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_STOP_FRENZY:
        if (s->world && s->world->frenzy_stop)
            s->world->frenzy_stop(s->world_ctx);
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_RESET_KF:
        {
            int k, killed = 0;
            for (k = 0; k < GTA_MAX_PROC; k++) {
                if (k == i || s->proc[k].cmd < 0 || s->proc[k].keep != 2)
                    continue;
                proc_end(s, k);
                killed++;
            }
            printf("gta: script - RESET_KF: %d frenzy process(es) ended\n",
                   killed);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_EXPLODE:
    case GTA_CMD_PLAIN_EXPL_BUILDING:
    case GTA_CMD_EXPL_NO_FIRE:
        /* `805 EXPLODE 129 810 0 0 0` / `1077 PLAIN_EXPL_BUILDING 322 0 0 2 0`
         * - a blast on one face of the block a DUMMY stands on. `p1` is the
         * object, `p4` the face (0 west, 1 east, 2 north, 3 south), `p5` the
         * score; an ACTION, so it always goes on to `p2` (MISSIONS.md:
         * the original's routine / the original's routine). A level's bank job is forty of the
         * second kind in a row - the whole block going up. */
        {
            const gta_decl *d = gta_script_by_line(s, (int)c->p1);
            if (d && s->world && s->world->explode)
                s->world->explode(s->world_ctx, d->line, d->x, d->y,
                                  (int)c->p4 & 3,
                                  c->op == GTA_CMD_EXPLODE);
        }
        advance_paid(s, i, c->p2, c->p5);
        return;

    case GTA_CMD_KILL_DROP:
        {
            gta_placed *dr = drop_of(s, (int)c->p1);
            if (!dr) { advance(s, i, c->p3); return; }
            dr->dead = 1;
        }
        advance(s, i, c->p2);
        return;

    case GTA_CMD_GOTO_DROPOFF:
        /* `761 GOTO_DROPOFF 240 0 810 56 50000` - get to the drop within the
         * time and the job pays; run out and it fails. The original tests it
         * once every eighth tick and takes one off `p4` each time, so `p4`
         * is not ticks: 56 of them is about eighteen seconds. The reach is
         * its own 16 pixels, and it is the PLAYER that has to be there -
         * the car is not looked at. */
        {
            long wx = 0, wy = 0, dx, dy;
            if (pr->fresh) {
                pr->fresh = 0;
                pr->count = c->p4 > 0 ? c->p4 * 8 : 0;
                pr->count2 = 0;
            }
            if (pr->count > 0 && --pr->count == 0) {
                printf("gta: script - drop-off %ld not reached in time\n",
                       c->p1);
                fflush(stdout);
                advance(s, i, c->p3);
                return;
            }
            if (++pr->count2 < 8)
                return;
            pr->count2 = 0;
            if (!gta_script_decl_pos(s, (int)c->p1, &wx, &wy))
                return;
            dx = s->px - wx; if (dx < 0) dx = -dx;
            dy = s->py - wy; if (dy < 0) dy = -dy;
            if (dx <= (16L << 16) && dy <= (16L << 16)) {
                printf("gta: script - drop-off %ld reached\n", c->p1);
                fflush(stdout);
                advance_paid(s, i, c->p2, c->p5);
            }
        }
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

/* ---- THE WATCH TRIGGERS (222, the research notes) ------------
 *
 * The original binds each to a car or ped handle at ENABLE; here the
 * watched line's handle is read live, which is the same thing once the car
 * exists and nothing before. All are Chebyshev squares in blocks, z ignored,
 * and the original's own 25 Hz counts (this tick is 25 Hz too). */

static int in_square(long wx, long wy, int bx, int by, int r)
{
    long dx = (wx >> 21) - bx, dy = (wy >> 21) - by;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return dx <= r && dy <= r;
}

/* Start `line`. A one-shot is never re-armed by its process (CARBOMB,
 * PEDCAR, DAMAGE): it goes to DISABLED and forgets it ever ran. */
static void watch_fire(gta_script *s, gta_decl *d, long line, int oneshot)
{
    kill_on_fire(s, d);
    d->state = oneshot ? GTA_TRIG_DISABLED : GTA_TRIG_STARTED;
    if (proc_start(s, line, oneshot ? -1 : d->line) >= 0) {
        printf("gta: script - %s %d fired, running line %ld\n",
               gta_script_type_name(d->type), d->line, line);
        fflush(stdout);
    }
}

/* 1 when `d` is a watch trigger (handled here, whatever happened). */
static int watch_tick(gta_script *s, gta_decl *d)
{
    const gta_script_world *w = s->world;
    unsigned long pc;
    long wx = 0, wy = 0;

    if (!w) return 0;
    pc = w->player_car ? w->player_car(s->world_ctx) : 0;
    switch (d->type) {
    case GTA_DECL_CORRECT_CAR_TRIG: {        /* L r car: the player in car X here */
        unsigned long h = car_handle(decl_of(s, d->c));
        if (h && pc == h && in_square(s->px, s->py, d->x, d->y, d->b))
            watch_fire(s, d, d->a, 0);
        return 1;
    }
    case GTA_DECL_CORRECT_MOD_TRIG: {        /* L r model; a wrong model here
                                              * keeps it watching anywhere */
        int in;
        if (!pc) return 1;
        in = in_square(s->px, s->py, d->x, d->y, d->b);
        if (in || d->counter) {
            if (w->player_car_model &&
                w->player_car_model(s->world_ctx) == d->c) {
                d->counter = 0;
                watch_fire(s, d, d->a, 0);
            } else if (in) {
                d->counter = 1;
            }
        }
        return 1;
    }
    case GTA_DECL_CARDESTROY_TRIG: {         /* L r car: car X a wreck here */
        unsigned long h = car_handle(decl_of(s, d->c));
        if (!h || pc == h || !w->car_pos || !w->car_dead) return 1;
        if (w->car_pos(s->world_ctx, h, &wx, &wy) &&
            in_square(wx, wy, d->x, d->y, d->b) &&
            w->car_dead(s->world_ctx, h))
            watch_fire(s, d, d->a, 0);
        return 1;
    }
    case GTA_DECL_CARWAIT_TRIG:              /* L car: stood still on screen */
    case GTA_DECL_CARSTUCK_TRIG: {           /* ...and off the road */
        unsigned long h = car_handle(decl_of(s, d->b));
        int st;
        if (!h || pc == h || !w->car_still) { d->counter = 0; return 1; }
        st = w->car_still(s->world_ctx, h);
        if (d->type == GTA_DECL_CARWAIT_TRIG ? st >= 1 : st == 2) {
            if (++d->counter > 64) {
                d->counter = 0;
                watch_fire(s, d, d->a, 0);
            }
        } else {
            d->counter = 0;
        }
        return 1;
    }
    case GTA_DECL_GUN_SCREEN_TRIG: {         /* L ped: shots fired, ped X sees */
        if (!s->on_foot || !w->player_firing || !w->onscreen ||
            !w->player_firing(s->world_ctx))
            return 1;
        {
            gta_decl *p = decl_of(s, d->b);
            if (p && p->handle && live_pos(s, d->b, &wx, &wy) &&
                w->onscreen(s->world_ctx, wx, wy))
                watch_fire(s, d, d->a, 0);
        }
        return 1;
    }
    case GTA_DECL_DUM_PED_BLOCK_TRIG: {      /* L r ped: ped X walked here */
        gta_decl *p = decl_of(s, d->c);
        if (p && p->handle && live_pos(s, d->c, &wx, &wy) &&
            in_square(wx, wy, d->x, d->y, d->b))
            watch_fire(s, d, d->a, 0);
        return 1;
    }
    case GTA_DECL_PEDCAR_TRIG: {             /* L ped car: ped X got into car Y */
        gta_decl *p = decl_of(s, d->b);
        unsigned long hc = car_handle(decl_of(s, d->c));
        unsigned long hp = p ? p->handle : 0;
        if (!hc || !hp) return 1;
        if (hp == (hc | GTA_IN_CAR)) {
            watch_fire(s, d, d->a, 1);
        } else if (!(hp & GTA_IN_CAR) && w->ped_board &&
                   w->ped_board(s->world_ctx, hp, hc)) {
            rename_handle(s, hp, hc | GTA_IN_CAR);
            watch_fire(s, d, d->a, 1);
        }
        return 1;
    }
    case GTA_DECL_DAMAGE_TRIG:               /* L r: the car brought here, */
    case GTA_DECL_ALT_DAMAGE_TRIG:           /* latched, then wrecked here */
        if (!d->handle) {
            if (pc && in_square(s->px, s->py, d->x, d->y, d->b))
                d->handle = pc;
            return 1;
        }
        if (!w->car_pos || !w->car_pos(s->world_ctx, d->handle, &wx, &wy) ||
            !in_square(wx, wy, d->x, d->y, d->b)) {
            d->handle = 0;
            return 1;
        }
        if (w->car_dead && w->car_dead(s->world_ctx, d->handle)) {
            d->handle = 0;
            watch_fire(s, d, d->a, 1);
        }
        return 1;
    case GTA_DECL_MOVING_TRIG_HIRED: {       /* car r: near car X, it bolts */
        unsigned long h = car_handle(decl_of(s, d->a));
        if (!h || !w->car_pos || !w->car_sendto ||
            !w->car_pos(s->world_ctx, h, &wx, &wy))
            return 1;
        if (in_square(s->px, s->py, (int)(wx >> 21), (int)(wy >> 21), d->b)) {
            w->car_sendto(s->world_ctx, h, gta_script_centre(d->x),
                          gta_script_centre(d->y));
            d->state = GTA_TRIG_STARTED;     /* no process: only ENABLE re-arms */
            printf("gta: script - MOVING_TRIG_HIRED %d: car %d drives to "
                   "(%d,%d)\n", d->line, d->a, d->x, d->y);
            fflush(stdout);
        }
        return 1;
    }
    case GTA_DECL_CARBOMB_TRIG: {            /* car L: in car X, a bomb fitted */
        unsigned long h = car_handle(decl_of(s, d->a));
        if (h && pc == h && w->car_bomb && w->car_bomb(s->world_ctx, h))
            watch_fire(s, d, d->b, 1);
        return 1;
    }
    case GTA_DECL_MOVING_TRIG:
        /* all eight in the shipped script watch FUTURECARs that are never
         * ENABLE'd, so none can fire (MISSIONS_DECLS_D.md) */
        return 1;
    }
    return 0;
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
        if (d->type == GTA_DECL_SPRAY || d->type == GTA_DECL_BOMBSHOP)
            continue;
        if (watch_tick(s, d))
            continue;
        if (d->type == GTA_DECL_CARTRIGGER) {
            /* `140 (154,15,4) CARTRIGGER 31220 139`: trigger type 9 fires
             * when the player is in the car declared on line `b` - the
             * number sits in the original's RADIUS slot, so the block test
             * spans the map and the car is the whole condition
             * (MISSIONS.md, trigger types). Never declared armed: ENABLE
             * arms it once the car exists. */
            gta_decl *car = decl_of(s, d->b);
            if (!car || !car->handle || !s->world || !s->world->player_car ||
                s->world->player_car(s->world_ctx) != car->handle)
                continue;
            d->state = GTA_TRIG_STARTED;
            kill_on_fire(s, d);
            if (proc_start(s, d->a, d->line) >= 0) {
                printf("gta: script - CARTRIGGER %d fired (in car %d),"
                       " running line %ld\n", d->line, car->line, (long)d->a);
                fflush(stdout);
            }
            continue;
        }
        if (d->type == GTA_DECL_DUM_MISSION_TRIG) {
            /* `618 (23,148,3) DUM_MISSION_TRIG 7700 3 319`: fires when CAR
             * 319 - the one a SENDTO drives there - comes within 3 blocks.
             * The player has nothing to do with it (trigger type 0xe). */
            long cx = 0, cy = 0;
            gta_decl *car = decl_of(s, d->c);
            if (!car || !car->handle || !s->world || !s->world->car_pos ||
                !s->world->car_pos(s->world_ctx, car_handle(car), &cx, &cy))
                continue;
            /* ...and not while the player is driving it (MISSIONS_DECLS_D) */
            if (s->world->player_car &&
                s->world->player_car(s->world_ctx) == car_handle(car))
                continue;
            dx = cx - gta_script_centre(d->x);
            dy = cy - gta_script_centre(d->y);
        } else {
            dx = px - gta_script_centre(d->x);
            dy = py - gta_script_centre(d->y);
        }
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        r = ((long)(d->b > 0 ? d->b : 1)) << 21;
        if (dx > r || dy > r) continue;
        d->state = GTA_TRIG_STARTED;
        kill_on_fire(s, d);
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

    /* THE BOMB SHOPS (228, the research notes) - the spray shop's twin:
     * drive in, pay BOMBSHOP_COST, and the car carries a bomb that FIRE
     * arms. The original re-runs every tick and a second sale is stopped
     * only by the bomb already fitted; here one visit is one try, and it
     * re-arms two blocks away, so "no money" is said once, not every tick. */
    for (i = 0; i < s->n; i++) {
        gta_decl *d = &s->d[i];
        long dx, dy;
        int r;
        if (d->type != GTA_DECL_BOMBSHOP)
            continue;
        dx = px - gta_script_centre(d->x); if (dx < 0) dx = -dx;
        dy = py - gta_script_centre(d->y); if (dy < 0) dy = -dy;
        if (d->state == GTA_TRIG_STARTED) {
            if (dx > (2L << 21) || dy > (2L << 21))
                d->state = GTA_TRIG_ARMED;
            continue;
        }
        /* THE SHOP'S BLOCK, as in the original: a garage, field ground under
         * a roof, which a car drives into (collision here is ground-based) */
        if (d->state != GTA_TRIG_ARMED || dx > (1L << 20) || dy > (1L << 20))
            continue;
        if (!s->world || !s->world->bombshop)
            continue;
        r = s->world->bombshop(s->world_ctx, s->bomb_cost);
        if (r >= 0) {
            d->state = GTA_TRIG_STARTED;
            printf("gta: script - bomb shop %d: %s\n", d->line,
                   r ? "bomb fitted" : "refused");
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
