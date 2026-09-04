/* The watchdog process - see amiga_watchdog.h for why it exists.
 *
 * Licence: MIT (ours).
 */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#include "amiga_watchdog.h"

volatile unsigned long amiga_wd_beat;
volatile int amiga_wd_phase;

static struct Task *s_main;
static volatile int s_stop;
static volatile int s_done;
static struct Process *s_proc;

/* The names, for the report; index = AMIGA_WD_PHASE_*. */
static const char *const phase_name[10] = {
    "none", "input", "sim", "traffic", "peds", "weapons", "player",
    "render", "present", "cap"
};

/* It is a Process, not a Task, so that dos.library may be called from it:
 * the report is a file. It has no stdio of its own - libnix's stdout belongs
 * to the main process and is exactly what a stuck main task may be holding
 * - so the report is Write()n through dos.library directly. */
static void wd_report(int stale)
{
    static char buf[2048];
    BPTR fh;
    int n = 0;
    const unsigned char *sp = (const unsigned char *)s_main->tc_SPReg;
    unsigned long pc = 0, sr = 0, fmt = 0;
    int phase = amiga_wd_phase;
    int i;

    if (phase < 0 || phase > 9)
        phase = 0;

    /* The saved registers at tc_SPReg: d0-d7/a0-a6 (60 bytes), then the
     * interrupt frame Exec's dispatcher left there - SR, PC, format. */
    if (sp && ((unsigned long)sp & 1) == 0) {
        sr  = ((unsigned long)sp[60] << 8) | sp[61];
        pc  = ((unsigned long)sp[62] << 24) | ((unsigned long)sp[63] << 16)
            | ((unsigned long)sp[64] << 8)  |  sp[65];
        fmt = ((unsigned long)sp[66] << 8) | sp[67];
    }

    n += snprintf(buf + n, sizeof buf - n,
        "WATCHDOG: no heartbeat for %d s - beat %lu, last phase %d (%s),"
        " main task state %d (%s)\n",
        stale, amiga_wd_beat, phase, phase_name[phase],
        (int)s_main->tc_State,
        s_main->tc_State == TS_WAIT ? "WAITING - blocked in the OS, not spinning"
        : s_main->tc_State == TS_READY ? "READY - spinning in our own code"
        : s_main->tc_State == TS_RUN ? "RUN" : "?");
    /* The same first line a Guru gets, so the same mapping works on it. */
    n += snprintf(buf + n, sizeof buf - n,
        "CPU TRAP 99 (WATCHDOG) at PC 0x%08lx SR 0x%04lx frame 0x%04lx%s\n",
        pc, sr, fmt, (sr & 0x2000) ? " [SUPERVISOR]" : "");
    n += snprintf(buf + n, sizeof buf - n,
        "  textbase: amiga_watchdog_start is at 0x%08lx (nm it to map the PC)\n",
        (unsigned long)(void *)amiga_watchdog_start);
    if (sp && ((unsigned long)sp & 1) == 0) {
        const unsigned long *st = (const unsigned long *)sp;
        n += snprintf(buf + n, sizeof buf - n, "  raw tc_SPReg 0x%08lx:",
                      (unsigned long)sp);
        for (i = 0; i < 20 && n < (int)sizeof buf - 12; i++)
            n += snprintf(buf + n, sizeof buf - n, " %08lx", st[i]);
        n += snprintf(buf + n, sizeof buf - n, "\n");
        /* The stack proper, above the frame: return addresses. */
        st = (const unsigned long *)(sp + 68);
        for (i = 0; i < 48 && n < (int)sizeof buf - 12; i++) {
            if ((i & 7) == 0)
                n += snprintf(buf + n, sizeof buf - n, "%s  usp+%03x:",
                              i ? "\n" : "", i * 4);
            n += snprintf(buf + n, sizeof buf - n, " %08lx", st[i]);
        }
        n += snprintf(buf + n, sizeof buf - n, "\n");
    }
    if ((pc & 1) == 0 && pc >= 0x1000UL && pc < 0x20000000UL) {
        const unsigned short *w = (const unsigned short *)pc;
        n += snprintf(buf + n, sizeof buf - n, "  opcode words:");
        for (i = 0; i < 8 && n < (int)sizeof buf - 8; i++)
            n += snprintf(buf + n, sizeof buf - n, " %04x", (unsigned)w[i]);
        n += snprintf(buf + n, sizeof buf - n, "\n");
    }

    fh = Open((CONST_STRPTR)"watchdog.log", MODE_NEWFILE);
    if (fh) {
        Write(fh, buf, n);
        Close(fh);
    }
}

static void wd_entry(void)
{
    unsigned long last = amiga_wd_beat;
    int stale = 0;
    int reported = 0;

    while (!s_stop) {
        Delay(50);                              /* one second */
        if (amiga_wd_beat != last) {
            last = amiga_wd_beat;
            stale = 0;
            reported = 0;
            continue;
        }
        stale++;
        /* Once at the threshold, and again every thirty seconds after it,
         * so a report is never lost to a later, different state. */
        if (stale == AMIGA_WD_STALE_SECS ||
            (stale > AMIGA_WD_STALE_SECS && (stale % 30) == 0)) {
            wd_report(stale);
            reported = 1;
        }
    }
    (void)reported;
    s_done = 1;
}

int amiga_watchdog_start(void)
{
    BPTR dir;

    s_main = FindTask(NULL);
    s_stop = 0;
    s_done = 0;
    amiga_wd_beat = 0;
    amiga_wd_phase = 0;

    /* The report goes beside the game, whatever the watchdog's own current
     * directory would have been. NP_CurrentDir takes the lock. */
    dir = Lock((CONST_STRPTR)"PROGDIR:", ACCESS_READ);

    s_proc = CreateNewProcTags(NP_Entry,      (ULONG)wd_entry,
                               NP_Name,       (ULONG)"AmiGTA watchdog",
                               NP_StackSize,  8192,
                               NP_Priority,   5,
                               NP_CurrentDir, (ULONG)dir,
                               TAG_END);
    if (!s_proc) {
        if (dir) UnLock(dir);
        printf("gta: watchdog - could not start\n");
        fflush(stdout);
        return 0;
    }
    printf("gta: watchdog armed - a %d s hang is reported in watchdog.log\n",
           AMIGA_WD_STALE_SECS);
    fflush(stdout);
    return 1;
}

void amiga_watchdog_stop(void)
{
    int i;
    if (!s_proc)
        return;
    s_stop = 1;
    /* It wakes once a second; give it two. */
    for (i = 0; i < 40 && !s_done; i++)
        Delay(5);
    s_proc = NULL;
}
