/*
 * Native MorphOS (PowerPC) build only - tools/bin/build-morphos.sh.
 *
 * Stand-ins for the two 68k assembly c2p routines in c2p_glue.s, which
 * cannot be linked into a PowerPC program. They are never meant to run:
 * MorphOS has no AGA chipset, so amiga_gfx.c always takes the RTG path there
 * (the deep screen fed by WriteLUTPixelArray). If one were ever reached the
 * screen would simply stay unchanged instead of the program jumping into 68k
 * code. (As in AmiSC's amiga/mos_c2p_stubs.c.)
 *
 * Licence: MIT (ours).
 */
#include <exec/types.h>

struct C2PArgs;
struct C2P6Args;

void c2p_rect_asm(struct C2PArgs *a) { (void)a; }
void c2p6_bm_asm(struct C2P6Args *a) { (void)a; }
