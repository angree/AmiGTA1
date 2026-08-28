/* Startup display chooser - Intuition side.
 *
 * One of the two files in this port allowed to include <proto/*> (the other
 * being amiga_gfx.c), for the reason documented in amiga_gfx.h: those headers
 * define function-style macros that collide with ordinary C++ identifiers, so
 * they must never reach the game.
 *
 * A system requester rather than a window of our own: it has to appear before
 * the game owns a display, it has to work on a machine where the RTG screen
 * we are asking about cannot be opened at all, and it must not need a single
 * pixel of our own drawing code to be working yet.
 */
#include <exec/types.h>
#include <intuition/intuition.h>
#include <proto/exec.h>
#include <proto/intuition.h>

#include "amiga_startup.h"

extern struct IntuitionBase *IntuitionBase;

/* Highest 68k family the running machine reports, as 20/30/40/60 (0 = 68000).
 * ExecBase->AttnFlags is set by the ROM and by any 060 library that patches
 * it, so this is what every other Amiga program uses for the same question. */
int amigastartup_cpu(void)
{
	UWORD f = SysBase->AttnFlags;
	if (f & (1 << 7)) return 60;      /* AFB_68060, set by the 060 libraries */
	if (f & AFF_68040) return 40;
	if (f & AFF_68030) return 30;
	if (f & AFF_68020) return 20;
	return 0;
}

void amigastartup_error(const char *text)
{
	struct EasyStruct es;

	if (IntuitionBase == 0 || text == 0) return;

	es.es_StructSize   = sizeof(es);
	es.es_Flags        = 0;
	es.es_Title        = (UBYTE *)"OpenXcom";
	es.es_TextFormat   = (UBYTE *)"%s";
	es.es_GadgetFormat = (UBYTE *)"OK";

	EasyRequestArgs(0, &es, 0, (APTR)&text);
}

int amigastartup_ask_backend(int rtg_available)
{
	struct EasyStruct es;
	LONG answer;

	if (IntuitionBase == 0) return AMIGA_STARTUP_AGA;

	es.es_StructSize   = sizeof(es);
	es.es_Flags        = 0;
	es.es_Title        = (UBYTE *)"OpenXcom";
	es.es_TextFormat   = (UBYTE *)"Which display should OpenXcom use?\n\n"
	                              "AGA opens a 320x200 8-bitplane screen and\n"
	                              "converts every frame chunky-to-planar.\n"
	                              "RTG opens an 8-bit CyberGraphX/Picasso96\n"
	                              "screen and needs no conversion at all.";
	es.es_GadgetFormat = rtg_available ? (UBYTE *)"AGA|RTG|Quit"
	                                   : (UBYTE *)"AGA|Quit";

	answer = EasyRequestArgs(0, &es, 0, 0);

	if (rtg_available) {
		/* 1 = AGA, 2 = RTG, 0 = the rightmost gadget, which is Quit. */
		if (answer == 1) return AMIGA_STARTUP_AGA;
		if (answer == 2) return AMIGA_STARTUP_RTG;
		return AMIGA_STARTUP_QUIT;
	}
	return (answer == 1) ? AMIGA_STARTUP_AGA : AMIGA_STARTUP_QUIT;
}
