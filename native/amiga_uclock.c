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
#ifdef __MORPHOS__
#include <stdlib.h>          /* atexit, for the sleep timer's teardown */
#endif

#include "amiga_uclock.h"

/* The base the ReadEClock inline indexes off. It is the same object on both
 * targets - timer.device's base, taken from the opened IORequest below - but
 * AmigaOS 68k's <proto/timer.h> declares it `struct Device *` and MorphOS's
 * declares it `struct Library *`, and a disagreement with the system header is
 * a hard error rather than a warning. So it is spelled the way the platform
 * spells it. */
#ifdef __MORPHOS__
struct Library *TimerBase = NULL;
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
#ifdef __MORPHOS__
	TimerBase = (struct Library *)s_tr.tr_node.io_Device;
#else
	TimerBase = s_tr.tr_node.io_Device;
#endif
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

#ifdef __MORPHOS__
/* Hand the CPU back for us microseconds - see the note in amiga_uclock.h for
 * why this exists on MorphOS and not on the 68k.
 *
 * A SECOND timer.device unit, opened separately from the E-clock one above.
 * UNIT_ECLOCK exists only to be read; issuing a TR_ADDREQUEST on it is not
 * what it is for, and the two uses want different IORequests anyway - this one
 * is sent and waited on, that one is never sent at all. They cost one
 * IORequest each and nothing else.
 *
 * The request is synchronous (DoIO, not SendIO): the caller is asking to be
 * blocked, so there is no state to carry between calls and nothing to abort on
 * the way out. */
static struct MsgPort   *s_sleep_mp;
static struct timerequest *s_sleep_tr;
static int                s_sleep_state = 0;   /* 0 untried, 1 open, -1 failed */

/* GIVE THE DEVICE BACK, and do it from atexit rather than from a cleanup
 * function somebody has to remember to call.
 *
 * Exec allocations are NOT reclaimed when a process ends - that is the whole
 * difference from a hosted OS - so a MsgPort, an IORequest and an open count on
 * timer.device would survive every run of the game until the machine was
 * rebooted. Registering the teardown at the same moment the resource is taken
 * is the only arrangement that cannot drift out of step with its caller, and it
 * needs no #ifdef in gta_main.c.
 *
 * Safe to run at exit: the request is always issued with DoIO, so nothing is
 * ever outstanding and timer.device can never reply into a port that is gone. */
static void sleep_close(void)
{
	if (s_sleep_state != 1) return;
	s_sleep_state = -1;                 /* no further sleeps after this */
	CloseDevice((struct IORequest *)s_sleep_tr);
	DeleteIORequest((struct IORequest *)s_sleep_tr); s_sleep_tr = NULL;
	DeleteMsgPort(s_sleep_mp);                       s_sleep_mp = NULL;
}

static int sleep_open(void)
{
	if (s_sleep_state != 0) return s_sleep_state > 0;
	s_sleep_state = -1;

	s_sleep_mp = CreateMsgPort();
	if (s_sleep_mp == NULL) return 0;

	s_sleep_tr = (struct timerequest *)
	             CreateIORequest(s_sleep_mp, sizeof(struct timerequest));
	if (s_sleep_tr == NULL) {
		DeleteMsgPort(s_sleep_mp); s_sleep_mp = NULL;
		return 0;
	}

	if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
	               (struct IORequest *)s_sleep_tr, 0) != 0) {
		DeleteIORequest((struct IORequest *)s_sleep_tr); s_sleep_tr = NULL;
		DeleteMsgPort(s_sleep_mp); s_sleep_mp = NULL;
		return 0;
	}

	s_sleep_state = 1;
	/* If atexit refuses there is nothing useful to do about it - the sleep is
	 * still correct, it just leaks on the way out - so the return is ignored
	 * rather than turned into a failure the caller cannot act on. */
	(void)atexit(sleep_close);
	return 1;
}

void amiga_uclock_sleep_us(unsigned long us)
{
	if (us == 0) return;
	/* Not an error path - a caller that cannot sleep simply runs uncapped,
	 * which is exactly what it did before this function existed. */
	if (!sleep_open()) return;

	s_sleep_tr->tr_node.io_Command = TR_ADDREQUEST;
	s_sleep_tr->tr_time.tv_secs    = (ULONG)(us / 1000000UL);
	s_sleep_tr->tr_time.tv_micro   = (ULONG)(us % 1000000UL);
	DoIO((struct IORequest *)s_sleep_tr);
}
#endif /* __MORPHOS__ */
