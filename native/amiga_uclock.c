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

struct Device *TimerBase = NULL;

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
	TimerBase = s_tr.tr_node.io_Device;
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

unsigned long amiga_uclock_freq(void)
{
	if (!uclock_open()) return 0;
	return (unsigned long)s_freq;
}
