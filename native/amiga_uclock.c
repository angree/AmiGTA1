/*
 * amiga_uclock.c - microsecond clock for profiling.
 *
 * WHY: SDL_GetTicks() here is the DOS DateStamp clock, 20 ms granularity -
 * useless for timing a 0.3 ms sprite blit. timer.device's ReadEClock() reads
 * the CIA E-clock (709 379 Hz PAL, 715 909 NTSC) - cheap (one library call,
 * no task switch) and fine enough for per-blit accounting.
 *
 * The 32-bit low word wraps every ~100 minutes; a running 64-bit total is
 * kept so differences across the wrap are right.
 */
#include <exec/types.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include "amiga_uclock.h"

#ifdef __MORPHOS__
struct Library *TimerBase = NULL;    /* the MorphOS SDK's type */
#else
struct Device *TimerBase = NULL;
#endif

static struct timerequest s_tr;
static int   s_state = 0;          /* 0 untried, 1 open, -1 failed */
static ULONG s_freq = 709379UL;
static ULONG s_last_lo = 0;
static unsigned long long s_total = 0;

static int uclock_open(void)
{
	struct EClockVal ev;
	if (s_state != 0) return s_state > 0;
	if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_ECLOCK, (struct IORequest *)&s_tr, 0) != 0) {
		s_state = -1;
		return 0;
	}
	TimerBase = (void *)s_tr.tr_node.io_Device;
	s_freq = ReadEClock(&ev);
	if (s_freq == 0) s_freq = 709379UL;
	s_last_lo = ev.ev_lo;
	s_total = 0;
	s_state = 1;
	return 1;
}

unsigned long amiga_uclock_us(void)
{
	struct EClockVal ev;
	ULONG d;
	if (!uclock_open()) return 0;
	ReadEClock(&ev);
	d = ev.ev_lo - s_last_lo;     /* modulo 2^32: survives the wrap */
	s_last_lo = ev.ev_lo;
	s_total += d;
	return (unsigned long)((s_total * 1000000ULL) / (unsigned long long)s_freq);
}

unsigned long amiga_uclock_raw(void)
{
	struct EClockVal ev;
	if (!uclock_open()) return 0;
	ReadEClock(&ev);
	return (unsigned long)ev.ev_lo;
}

unsigned long amiga_uclock_freq(void)
{
	if (!uclock_open()) return 0;
	return (unsigned long)s_freq;
}

/* SLEEP INSTEAD OF SPINNING (40-MorphOS-zgodnosc-68k.md, item 5): the frame
 * cap busy-waited on the clock above, which under MorphOS and AROS is a
 * core at 100% for nothing. A second timer request on UNIT_MICROHZ, opened
 * on first use, waits in the OS. Wake-up is late by up to a scheduler
 * slice, so the caller sleeps short and spins only the last stretch. */
static struct MsgPort *s_sport = NULL;
static struct timerequest *s_sreq = NULL;
static int s_sstate = 0;            /* 0 untried, 1 open, -1 failed */

static int sleep_open(void)
{
	if (s_sstate != 0) return s_sstate > 0;
	s_sstate = -1;
	s_sport = CreateMsgPort();
	if (!s_sport) return 0;
	s_sreq = (struct timerequest *)CreateIORequest(s_sport, sizeof(struct timerequest));
	if (!s_sreq) { DeleteMsgPort(s_sport); s_sport = NULL; return 0; }
	if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
	               (struct IORequest *)s_sreq, 0) != 0) {
		DeleteIORequest((struct IORequest *)s_sreq); s_sreq = NULL;
		DeleteMsgPort(s_sport); s_sport = NULL;
		return 0;
	}
	s_sstate = 1;
	return 1;
}

void amiga_uclock_sleep_us(unsigned long us)
{
	if (us == 0 || !sleep_open()) return;
	s_sreq->tr_node.io_Command = TR_ADDREQUEST;
	s_sreq->tr_time.tv_secs = us / 1000000UL;
	s_sreq->tr_time.tv_micro = us % 1000000UL;
	DoIO((struct IORequest *)s_sreq);
}

void amiga_uclock_close(void)
{
	if (s_sstate > 0) {
		CloseDevice((struct IORequest *)s_sreq);
		DeleteIORequest((struct IORequest *)s_sreq);
		DeleteMsgPort(s_sport);
	}
	s_sreq = NULL; s_sport = NULL; s_sstate = 0;
}
