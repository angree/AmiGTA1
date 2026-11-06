/* The watchdog process - see amiga_watchdog.h for why it exists.
 *
 * Licence: MIT (ours).
 */
#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <exec/memory.h>
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
        /* PC, SR, then d0-a6: measured with `pcprof 2`, 2026-11-05. */
        sr  = ((unsigned long)sp[4] << 8) | sp[5];
        pc  = ((unsigned long)sp[0] << 24) | ((unsigned long)sp[1] << 16)
            | ((unsigned long)sp[2] << 8)  |  sp[3];
        fmt = 0;
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
        st = (const unsigned long *)(sp + 66);
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

/* The profiler - see amiga_watchdog.h. */
#define PCPROF_MAX 16384
static int s_prof_on;
static unsigned long *s_prof;
static unsigned long s_prof_n;

void amiga_watchdog_set_profile(int on) { s_prof_on = on; }

static void prof_sample(void)
{
    const unsigned long *sp;
    unsigned long pc, ra = 0;
    unsigned long lo = (unsigned long)(void *)amiga_watchdog_start - 0x100000UL;
    unsigned long hi = (unsigned long)(void *)amiga_watchdog_start + 0x100000UL;
    int i;

    if (!s_prof || s_prof_n >= PCPROF_MAX)
        return;
    /* READY only: then its registers are on its stack. WAIT is a sleep in the
     * OS - the frame cap, the disk - and is counted with pc 0. */
    if (s_main->tc_State != TS_READY) {
        s_prof[s_prof_n * 3] = 0;
        s_prof[s_prof_n * 3 + 1] = 0;
        s_prof[s_prof_n * 3 + 2] = (unsigned long)amiga_wd_phase;
        s_prof_n++;
        return;
    }
    sp = (const unsigned long *)s_main->tc_SPReg;
    if (!sp || ((unsigned long)sp & 1))
        return;
    /* `pcprof 2`: the raw top of the stack, to find the frame's layout. */
    if (s_prof_on == 2) {
        if (s_prof_n + 8 < PCPROF_MAX) {
            const unsigned short *w = (const unsigned short *)sp;
            for (i = 0; i < 48; i++)
                s_prof[s_prof_n * 3 + i / 2] = (i & 1)
                    ? (s_prof[s_prof_n * 3 + i / 2] << 16) | w[i]
                    : w[i];
            s_prof_n += 8;
        }
        return;
    }
    pc = sp[0];              /* the layout, found with `pcprof 2`: PC, SR, d0-a6 */
    /* A PC in the ROM or a library says little: the first return address
     * above the frame that lies in our own code says who called it. */
    sp = (const unsigned long *)((const char *)sp + 66);
    for (i = 0; i < 32; i++) {
        if (sp[i] >= lo && sp[i] < hi && !(sp[i] & 1)) {
            ra = sp[i];
            break;
        }
    }
    s_prof[s_prof_n * 3] = pc;
    s_prof[s_prof_n * 3 + 1] = ra;
    s_prof[s_prof_n * 3 + 2] = (unsigned long)amiga_wd_phase;
    s_prof_n++;
}

static void prof_write(void)
{
    BPTR fh;
    unsigned long head[3];
    if (!s_prof)
        return;
    head[0] = 0x50435046UL;
    head[1] = (unsigned long)(void *)amiga_watchdog_start;
    head[2] = s_prof_n;
    fh = Open((CONST_STRPTR)"pcprof.bin", MODE_NEWFILE);
    if (fh) {
        Write(fh, head, sizeof head);
        Write(fh, s_prof, s_prof_n * 12);
        Close(fh);
    }
    FreeMem(s_prof, PCPROF_MAX * 12);
    s_prof = NULL;
}

static void wd_entry(void)
{
    unsigned long last = amiga_wd_beat;
    int stale = 0;
    int reported = 0;
    int slice = 0;

    while (!s_stop) {
        if (s_prof) {
            /* The same one-second heartbeat, from fifty sleeps of one tick. */
            Delay(1);
            prof_sample();
            if (++slice < 50)
                continue;
            slice = 0;
        } else {
            Delay(50);                          /* one second */
        }
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
    prof_write();
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
    s_prof_n = 0;
    if (s_prof_on)
        s_prof = (unsigned long *)AllocMem(PCPROF_MAX * 12, MEMF_ANY);

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
    for (i = 0; i < 200 && !s_done; i++)   /* the profile file takes longer */
        Delay(5);
    s_proc = NULL;
}
