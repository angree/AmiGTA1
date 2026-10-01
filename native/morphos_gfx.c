/* MorphOS/PowerPC display and input - the RTG path, natively.
 *
 * This is the MorphOS counterpart of native/amiga_gfx.c and it implements the
 * SAME amiga_gfx.h contract, so native/gta_main.c and the rest of the engine
 * are byte-for-byte the code the 68k build uses. Only this file changes.
 *
 * WHY A SEPARATE FILE RATHER THAN #ifdefs THROUGH amiga_gfx.c.
 *
 * amiga_gfx.c carries four display backends - AGA, EHB, RTG and a Workbench
 * window - and three of them are 68k to the bone: contiguous Chip RAM
 * bitplanes, Mikael Kalms' chunky-to-planar in 68020 assembler, EHB's
 * hardware half-brights, WritePixelArray8. None of that exists on MorphOS and
 * none of it can be made to. Bracketing it would have meant threading
 * conditionals through roughly a thousand lines that the PowerPC build can
 * never execute, in a file whose 68k build CANNOT BE CHECKED FROM HERE - the
 * bebbo toolchain is not installed on this machine - so a mistake would have
 * broken the Amiga release silently. The whole of this file is instead under
 * one #ifdef __MORPHOS__, which is the same rule stated once at the top
 * instead of thirty times in the middle of somebody else's code.
 *
 * The RTG backend is the one that survives the move, and it is the one this
 * file is: on MorphOS every screen is an RTG screen. There is no chunky-to-
 * planar step, because the chunky buffer the renderer writes into is already
 * the display's own format - which is exactly the reason the RTG variant was
 * chosen as the base for this port.
 *
 * MIT, like the rest of the port.
 */

#ifdef __MORPHOS__

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <cybergraphx/cybergraphics.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/cybergraphics.h>

#include <string.h>
#include <stdio.h>

#include "amiga_gfx.h"

/* Declared extern by <proto/cybergraphics.h>; ours to define and to fill.
 * cybergraphics.library is resident on every MorphOS machine, but it is still
 * opened rather than assumed - the failure is then one log line instead of a
 * null-pointer jump through the inline's library base. */
struct Library *CyberGfxBase;

/* The stack this program runs on.
 *
 * ONE MEGABYTE, and it is the same number for the same reason as everywhere
 * else in this family of ports: the Amiga build's `run` script says
 * `Stack 1000000` because the map and sprite loaders recurse, and the OpenTTD
 * and OpenXcom ports both landed on the same figure independently. A default
 * shell stack is a few kilobytes and a crash in a loader looks nothing like a
 * stack overflow when you read the report.
 *
 * Setting it HERE rather than in a startup script is the difference that
 * matters on MorphOS: a program launched from Ambient by double-click never
 * runs the script, and the failure then depends on how the user started the
 * game. The C runtime's own __stack is weak, so this definition wins. */
unsigned long __stack = 1024 * 1024;

/* The standard AmigaOS/MorphOS version cookie, so `version AmiGTA` answers.
 * USED_VAR-style: referenced by nothing, which is exactly why it needs the
 * `used` attribute or the linker throws it away at -O2. */
static const char *const g_version __attribute__((used)) =
	"$VER: AmiGTA 0.0.2 (MorphOS/PPC)";

/* ------------------------------------------------------------------ log --- */

void amigagfx_log(const char *msg)
{
	fprintf(stdout, "morphos: %s\n", msg);
	fflush(stdout);
}

static int g_verbose;

void amigagfx_set_verbose(int verbose)
{
	g_verbose = verbose;
}

/* ---------------------------------------------------------------- state --- */

/* HOW A RECTANGLE REACHES THE SCREEN. Three routes, and the screen that
 * opened decides which one - the choice is never guessed from the mode we
 * asked for, it is made from what the display actually turned out to be.
 *
 *   BLIT_LOCK  8-bit LUT8 screen, and LockBitMapTagList hands back a base
 *              address and a bytes-per-row we can write to directly. The
 *              chunky buffer IS the display format there, so a rectangle is a
 *              memcpy per row and nothing is converted at all. Fastest, and
 *              the reason the RTG path was worth porting.
 *
 *   BLIT_PENS  8-bit screen where the lock was refused or reported something
 *              other than LUT8. WritePixelArray with RECTFMT_LUT8 writes our
 *              bytes as PEN NUMBERS, unremapped - correct, because on an
 *              8-bit screen of our own those pens hold our palette.
 *
 *   BLIT_WLUT  the screen is deeper than 8 bits, which is the common case -
 *              not because the hardware cannot do 8, but because an 8-bit mode
 *              at a given size has to EXIST in the machine's screenmode
 *              database and a stock install may not carry one. There are no pens to
 *              share, so WriteLUTPixelArray converts our chunky bytes through
 *              the CTABFMT_XRGB8 table. It is the only correct route on a
 *              truecolour screen and it is what makes this port run on machines
 *              that cannot open an 8-bit screen in the first place.
 *
 * LOCKING DISCIPLINE, obeyed everywhere BLIT_LOCK appears - getting this wrong
 * on MorphOS wedges the display, so it is stated once:
 *   1. Between LockBitMapTagList() and UnLockBitMap() this file calls NOTHING.
 *      No OS function, no logging, no allocation - only memcpy over memory
 *      whose bounds were computed BEFORE the lock was taken.
 *   2. Every path out of a locked region passes through exactly one
 *      UnLockBitMap(). The format check inside the lock does not return early;
 *      it sets a flag, falls out, unlocks, and only then acts on it.
 *   3. Base address and bytes-per-row are valid ONLY inside the lock that
 *      produced them. Re-read on every lock, never cached across one.
 *   4. The lock is held for one rectangle, never across a frame or the event
 *      loop.
 */
#define BLIT_LOCK 0
#define BLIT_PENS 1
#define BLIT_WLUT 2
/*   BLIT_RAW   the same truecolour case as BLIT_WLUT, done the other way
 *              round: the palette lookup happens HERE, into a scratch buffer
 *              in ordinary cached memory, and WritePixelArray then moves
 *              pixels that are already in the screen's format.
 *
 *              WHY THAT MIGHT BE FASTER. The MorphOS autodoc annotates every
 *              accelerated call - "Uses hardware acceleration on dedicated
 *              hardware", "Uses Altivec enabled code if..." - and
 *              WriteLUTPixelArray carries no such note, while the same
 *              document is explicit that only "many" calls are accelerated.
 *              The one readable implementation of it anywhere (AROS's) is a
 *              scalar dst[x] = lut[src[x]] loop, and no GPU can expand a
 *              palette out of a RAM-side array in any case. Moving
 *              already-native pixels is the case that CAN be accelerated, and
 *              is measured at ~215 MB/s on a MorphOS G4.
 *
 *              It is also what E-UAE does on this platform, and what the
 *              scaled path in this file has been doing all along.
 *
 *              NOT THE DEFAULT, because none of that is a measurement. See
 *              blit_bench(). */
#define BLIT_RAW  3

static struct Screen *g_screen;
static struct Window *g_window;

static UBYTE *g_chunky;          /* the 8bpp buffer the renderer draws into */
static int    g_width, g_height; /* the GAME AREA the ENGINE renders, always
                                  * exactly what amigagfx_open was asked for */
static int    g_pitch;           /* bytes per chunky row; == g_width here   */
static int    g_xoff, g_yoff;    /* where the game area sits on the screen  */
static int    g_bar;             /* height of the Intuition title bar, 0 = none */

/* HOW MUCH OF THE GAME AREA THE SCREEN CAN ACTUALLY SHOW, which is not the
 * same thing as how much the engine draws, and conflating the two overruns the
 * heap.
 *
 * The engine renders a FIXED size. gta_main.c hands gta_render_target() its
 * compile-time SCREEN_W/SCREEN_H and never asks this file how big the game
 * area came out - it does not call amigagfx_game_width() at all. So if
 * g_width/g_pitch were clipped down to a screen that turned out smaller than
 * requested, the renderer would go on writing full-width rows into a buffer
 * allocated for the clipped ones, and every row would run into the next.
 *
 * Therefore: g_width/g_height/g_pitch are ALWAYS what was asked for and the
 * chunky buffer is always that size, and a screen too small to show all of it
 * costs a cropped picture rather than a corrupted allocator. The clip lives
 * here, and it is applied in exactly one place - amigagfx_blit. */
static int    g_vis_w, g_vis_h;

/* SCALING: stretch the picture to the screen instead of centring it.
 *
 * g_scale is the setting; g_dst_* is where a frame actually lands, which is
 * the whole screen when scaling and the centred game area when not. Keeping
 * the destination rectangle as state rather than recomputing it per blit means
 * the blit does not have to know which mode it is in - it is handed a source
 * rectangle and a destination rectangle either way. */
static int    g_scale;
static int    g_dst_x, g_dst_y, g_dst_w, g_dst_h;

/* ---- DOUBLE BUFFERING --------------------------------------------------- *
 *
 * Two screen bitmaps: the game draws into the one that is not being shown and
 * ChangeScreenBuffer swaps them. It does NOT make the frame cheaper - the same
 * pixels are still converted and written - it stops the blit racing the
 * scanout, which is the difference between a picture and a picture with a tear
 * across it.
 *
 * WHY IT IS SAFE TO ADD BLIND. Everything here fails soft: if
 * AllocScreenBuffer refuses, g_dbuf stays 0 and every path below is the
 * single-buffer one that was already there. The only new risk is the Wait()
 * for the safe-to-draw message, so there is a way out that does not need a
 * rebuild - see the nodbuf note in dbuf_open().
 *
 * THE OTHER DOCUMENTED WAY, recorded because it is the one a MorphOS
 * developer recommends: "the usual method to achieve multibuffering is to use
 * ScrollVPort() on a double height screen", with "do not allocate a custom
 * bitmap... it may be misaligned, using the wrong format or not located in
 * video ram". That advice is aimed at code poking a bitmap pointer into a
 * screen's own RastPort, which the same developer calls a hack that "was never
 * supported by RTG" - this uses AllocScreenBuffer/ChangeScreenBuffer, which is
 * the supported API for the same thing and is what the CGX drivers use. If the
 * flip ever misbehaves on real hardware, the double-height-screen + ScrollVPort
 * arrangement is the first thing to try instead.
 *
 * A DOUBLE-BUFFERED FRAME IS ALWAYS WHOLE. With two buffers a dirty rectangle
 * is not enough: what was drawn last frame went to the OTHER bitmap, so
 * anything not redrawn this frame shows the frame before last. Rather than
 * track damage across two buffers, a flipped frame presents the lot - which
 * costs nothing in practice, because gta_main.c blits full-frame anyway.
 */
static struct ScreenBuffer *g_sb[2];
static struct MsgPort      *g_sb_safe;
static struct MsgPort      *g_sb_disp;
static struct RastPort      g_sb_rp;      /* points at the BACK buffer */
static int g_dbuf;        /* 1 when the two buffers are up                */
static int g_sb_cur;      /* which one the game is drawing into           */
static int g_sb_pending;  /* a flip is in flight; its messages are owed   */

/* Where the library blit calls should draw. The back buffer while double
 * buffered, the screen itself otherwise - so every blit below says
 * `target_rp()` and none of them has to know which mode is running. */
static struct RastPort *target_rp(void)
{
	return g_dbuf ? &g_sb_rp : &g_screen->RastPort;
}

/* Scratch for the scaled truecolour path - see blit_scaled_wlut. Allocated on
 * first use at the destination size and kept, because a frame is every frame
 * and allocating one per frame would be the most expensive thing in the loop. */
static ULONG *g_scratch;
static long   g_scratch_px;

void amigagfx_set_scale(int on)
{
    g_scale = on ? 1 : 0;
}

static int    g_depth;           /* bits per pixel of the screen we opened  */
static int    g_blit;            /* BLIT_*                                  */
static int    g_demoted;         /* log a LOCK->PENS demotion exactly once  */
static ULONG  g_want_modeid;

static ULONG  g_ctable[256];     /* CTABFMT_XRGB8 mirror of the palette     */
static ULONG  g_epoch;
static unsigned long g_blits;

static char   g_screen_title[64] = "AmiGTA";

/* SA_DetailPen and SA_BlockPen are passed SEPARATELY to OpenScreenTags and
 * they OVERRIDE what SA_Pens says for exactly the two the title bar uses, so
 * all three have to be kept in step - setting SA_Pens alone changes the
 * screen's gadgetry and leaves the bar itself untouched. */
static ULONG  g_detail_pen = 15;
static ULONG  g_block_pen  = 17;

/* The full DRIPEN table Intuition wants, terminated by ~0. It is a
 * thirteen-entry array and not a shorter one because SA_Pens reads until the
 * terminator: a four-entry version left Intuition reading whatever followed it
 * in memory for SHINEPEN onwards, which is a wrong bevel at best. Defaults
 * match the 68k file's; amigagfx_set_bar_pens() replaces them with indices
 * picked out of the palette that is actually loaded. */
static UWORD  g_screen_pens[] = {
	15,         /* DETAILPEN        - bar text, old (1.3) look   */
	17,         /* BLOCKPEN         - bar fill, old (1.3) look   */
	15,         /* TEXTPEN          - text on BACKGROUNDPEN      */
	15,         /* SHINEPEN         - bevel light edge           */
	0,          /* SHADOWPEN        - bevel dark edge            */
	19,         /* FILLPEN          - selected gadget fill       */
	15,         /* FILLTEXTPEN                                   */
	17,         /* BACKGROUNDPEN                                 */
	15,         /* HIGHLIGHTTEXTPEN                              */
	15,         /* BARDETAILPEN     - screen title text (3.x)    */
	17,         /* BARBLOCKPEN      - screen title bar fill (3.x)*/
	0,          /* BARTRIMPEN       - line under the bar         */
	(UWORD)~0
};

static int    g_hide_pointer;
static int    g_pointer_suspended;

/* ------------------------------------------------------------- memory ----- */

unsigned long AmigaLargestFastMem(void)
{
	unsigned long v;
	Forbid();
	v = AvailMem(MEMF_FAST | MEMF_LARGEST);
	Permit();
	return v;
}

unsigned long AmigaFreeFastMem(void)
{
	unsigned long v;
	Forbid();
	v = AvailMem(MEMF_FAST);
	Permit();
	return v;
}

/* One line per call into PROGDIR:morphos_mem.log. Same shape as the 68k
 * probe, and the same cap so it can never flood; the numbers mean less on a
 * machine with virtual memory and 512 MB of it, but a startup that eats
 * hundreds of megabytes still shows up here. */
#define MEM_LOG      "PROGDIR:morphos_mem.log"
#define MEM_MAXLINES 100

void AmigaMemProbe(const char *label)
{
	static int lines = 0;
	FILE *f;

	if (lines >= MEM_MAXLINES) return;
	f = fopen(MEM_LOG, lines == 0 ? "w" : "a");
	if (f == NULL) return;
	fprintf(f, "%3d %-28s fast %8lu KB  largest %8lu KB\n",
	        ++lines, label != NULL ? label : "?",
	        AmigaFreeFastMem() >> 10, AmigaLargestFastMem() >> 10);
	fclose(f);
}

/* ------------------------------------------------------------- pointer ---- */

/* HIDING THE POINTER, AND WHY THERE IS NO SPRITE HERE ANY MORE.
 *
 * The Amiga way is SetPointer() with a sprite made of zeros, because Intuition
 * has no "no pointer" call. On MorphOS that is the wrong mechanism twice over.
 * A MorphOS developer, on the subject of sprites under RTG: "Modern (har har)
 * graphic chips only support one 'sprite' and it's used for the mouse pointer.
 * As a matter of fact, RTG implies not using sprites at all." And MorphOS does
 * have the call the Amiga lacks - SA_ShowPointer on a custom screen, which is
 * passed at OpenScreen and is why the pointer never appears at all rather than
 * appearing once and being covered up.
 *
 * So the sprite is gone, with its Chip RAM allocation. What is left is the
 * flag, which the screen is opened from: the game sets this once at startup
 * and never toggles it, so nothing is lost. Were a caller ever to toggle it
 * mid-session, it would need the screen reopening - which is what the
 * amigagfx_open path already does for the title bar. */
/* The screen carries the setting; a window-level ClearPointer is still the way
 * to put the pointer BACK, which is what the loading splash wants. */
static void pointer_apply(void)
{
	if (g_window == NULL) return;
	if (!g_hide_pointer || g_pointer_suspended)
		ClearPointer(g_window);
}

void amigagfx_set_hide_system_pointer(int on)
{
	g_hide_pointer = on ? 1 : 0;
	pointer_apply();
}

void amigagfx_pointer_suspend(int on)
{
	g_pointer_suspended = on ? 1 : 0;
	pointer_apply();
}

static void pointer_free(void)
{
	if (g_window != NULL) ClearPointer(g_window);
}

/* --------------------------------------------------------- title bar ------ */

void amigagfx_set_screen_title(const char *t)
{
	if (t == NULL) return;
	strncpy(g_screen_title, t, sizeof g_screen_title - 1);
	g_screen_title[sizeof g_screen_title - 1] = '\0';
	/* Guarded on the WINDOW, not the screen: this is the only way Intuition
	 * offers to change a screen's title after it is open, and it needs a
	 * window on that screen to say it through. (STRPTR)~0 means "leave the
	 * window's own title alone". Before the window exists the stored string is
	 * enough - it goes in as SA_Title when the screen opens. */
	if (g_window != NULL)
		SetWindowTitles(g_window, (STRPTR)~0, (STRPTR)g_screen_title);
}

/* A screen pen is an INDEX, not a colour, so these have to come out of the
 * palette that is actually loaded - see choose_bar_pens() in gta_main.c. Must
 * be called BEFORE amigagfx_open(); afterwards the screen already has its
 * pens. Only an 8-bit screen has pens at all; on a truecolour one Intuition
 * draws the bar from the system's own colours and these are ignored. */
void amigagfx_set_bar_pens(int text, int fill, int trim)
{
	if (text < 0 || text > 255 || fill < 0 || fill > 255 ||
	    trim < 0 || trim > 255)
		return;

	g_detail_pen = (ULONG)text;
	g_block_pen  = (ULONG)fill;

	g_screen_pens[0]  = (UWORD)text;   /* DETAILPEN          */
	g_screen_pens[1]  = (UWORD)fill;   /* BLOCKPEN           */
	g_screen_pens[2]  = (UWORD)text;   /* TEXTPEN            */
	g_screen_pens[3]  = (UWORD)text;   /* SHINEPEN           */
	g_screen_pens[4]  = (UWORD)trim;   /* SHADOWPEN          */
	g_screen_pens[5]  = (UWORD)fill;   /* FILLPEN            */
	g_screen_pens[6]  = (UWORD)text;   /* FILLTEXTPEN        */
	g_screen_pens[7]  = (UWORD)fill;   /* BACKGROUNDPEN      */
	g_screen_pens[8]  = (UWORD)text;   /* HIGHLIGHTTEXTPEN   */
	g_screen_pens[9]  = (UWORD)text;   /* BARDETAILPEN       */
	g_screen_pens[10] = (UWORD)fill;   /* BARBLOCKPEN        */
	g_screen_pens[11] = (UWORD)trim;   /* BARTRIMPEN         */
}

/* Make Intuition draw the screen title bar again.
 *
 * THE BAR IS PIXELS IN THE SCREEN'S OWN BITMAP, and Intuition redraws it when
 * something makes it - a click on the depth gadget, say - and not before. So
 * anything that paints over those rows erases it until the player happens to
 * touch the screen, which reads as "the bar never opened" and is not that at
 * all. The one thing here that paints there is the full-screen SetRast in
 * amigagfx_open, which is deliberate: it blacks the border around a game area
 * that does not fill the screen.
 *
 * Toggling ShowTitle off and on is what forces the redraw; Intuition has no
 * "refresh the bar" call. Harmless when there is no bar. */
void amigagfx_refresh_titlebar(void)
{
	/* g_bar is 0 when the screen was opened without one, and toggling ShowTitle
	 * on such a screen would not refresh a bar - it would CREATE one the caller
	 * asked not to have. */
	if (g_screen == NULL || g_bar == 0) return;
	ShowTitle(g_screen, FALSE);
	ShowTitle(g_screen, TRUE);
}

/* ------------------------------------------------------ no planes here ---- */

/* AGA-only diagnostics. There are no bitplanes on MorphOS and there is no Chip
 * RAM to measure, so these answer honestly rather than inventing something:
 * gta_main.c's memory readout prints "n/a" for a NULL. */
unsigned char *amigagfx_planes(void)  { return NULL; }
long           amigagfx_planes_bytes(void) { return 0; }

/* ------------------------------------------------------------- library ---- */

static int cgx_open(void)
{
	if (CyberGfxBase != NULL) return 1;
	CyberGfxBase = OpenLibrary((CONST_STRPTR)CYBERGFXNAME, 41L);
	if (CyberGfxBase == NULL) {
		amigagfx_log("cybergraphics.library v41 not available - cannot open a screen");
		return 0;
	}
	return 1;
}

static void cgx_close(void)
{
	if (CyberGfxBase != NULL) { CloseLibrary(CyberGfxBase); CyberGfxBase = NULL; }
}

/* --------------------------------------------------------------- modes ---- */

/* Ask CGX for the best mode of at least w x h at this depth, then verify what
 * came back. BestCModeIDTagList never fails outright - when nothing matches it
 * returns a mode id anyway - so the answer is checked before it is believed.
 * Returns INVALID_ID when there is no such mode. */
static ULONG best_mode(int w, int h, int depth)
{
	ULONG id;
	struct TagItem tags[4];

	if (!cgx_open()) return (ULONG)INVALID_ID;

	tags[0].ti_Tag = CYBRBIDTG_NominalWidth;  tags[0].ti_Data = (ULONG)w;
	tags[1].ti_Tag = CYBRBIDTG_NominalHeight; tags[1].ti_Data = (ULONG)h;
	tags[2].ti_Tag = CYBRBIDTG_Depth;         tags[2].ti_Data = (ULONG)depth;
	tags[3].ti_Tag = TAG_END;                 tags[3].ti_Data = 0UL;

	id = BestCModeIDTagList(tags);

	if (id == (ULONG)INVALID_ID) return (ULONG)INVALID_ID;
	if (!IsCyberModeID(id))      return (ULONG)INVALID_ID;
	if (GetCyberIDAttr(CYBRIDATTR_DEPTH, id) != (ULONG)depth) return (ULONG)INVALID_ID;

	return id;
}

/* Every MorphOS screen is an RTG screen, so this is always true when the
 * library is there. It exists because amiga_gfx.h declares it and because a
 * caller building a resolution list must get the same answer on both targets. */
int amigagfx_rtg_has_mode(int w, int h)
{
	int ok;
	(void)w; (void)h;
	ok = cgx_open();
	/* The probe can run long before any screen exists. Leave the library open
	 * only if it is already carrying one; otherwise hand it straight back. */
	if (g_screen == NULL) cgx_close();
	return ok;
}

int amigagfx_backend(void) { return AMIGAGFX_BACKEND_RTG; }

/* Window mode is an AGA-era backend that shares the Workbench palette; it has
 * no purpose here and is not implemented. Answering 0 makes the shared code
 * take the screen path, which is the only one this file has. */
int amigagfx_wb_available(void) { return 0; }

int amigagfx_wb_colours(int *granted)
{
	if (granted != NULL) *granted = 256;
	return AMIGAGFX_WBCOL_OWN;
}

/* EHB does not exist on PowerPC hardware; the palette is accepted and dropped
 * so the shared caller needs no #ifdef. */
void amigagfx_set_ehb_palette(const unsigned char *rgb64)
{
	(void)rgb64;
}

/* ---------------------------------------------------------------- clock --- */

static ULONG raw_ticks(void)
{
	struct DateStamp ds;
	DateStamp(&ds);
	return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

unsigned long amigagfx_millis(void)
{
	return (unsigned long)((raw_ticks() - g_epoch) * 20UL);
}

/* -------------------------------------------------------- pointer warp ---- */

/* The 68k file feeds input.device an IECLASS_NEWPOINTERPOS event for this.
 * Nothing in AmiGTA uses the mouse at all - the game hides the pointer at
 * startup and never reads it - so rather than carry a device open and an
 * IORequest for a call that is never made, this reports honestly that it did
 * not move the pointer. A caller that grows a use for it will see the 0. */
int amigagfx_warp_pointer(int x, int y)
{
	(void)x; (void)y;
	return 0;
}

/* ---------------------------------------------------------- blit probe ---- */

/* Take the lock once, look at what we actually got, release it, and decide how
 * this screen will be blitted. Deliberately a real lock rather than an
 * inspection of the mode: the mode says what was requested, the lock says what
 * LockBitMapTagList will hand the blit every frame. */
static void probe_blit_method(void)
{
	APTR  handle;
	APTR  base   = NULL;
	ULONG bpr    = 0;
	ULONG depth  = 0;
	ULONG pixfmt = (ULONG)~0;
	struct TagItem tags[5];

	g_demoted = 0;

	/* Deeper than 8 bits: there are no pens and nothing to lock into a byte
	 * per pixel. WriteLUTPixelArray is the only correct route and it is not a
	 * fallback here - it is the right call. */
	if (g_depth > 8) {
		g_blit = BLIT_WLUT;
		amigagfx_log("blit: WriteLUTPixelArray (truecolour screen, CTABFMT_XRGB8)");
		return;
	}

	g_blit = BLIT_PENS;

	tags[0].ti_Tag = LBMI_BASEADDRESS; tags[0].ti_Data = (ULONG)&base;
	tags[1].ti_Tag = LBMI_BYTESPERROW; tags[1].ti_Data = (ULONG)&bpr;
	tags[2].ti_Tag = LBMI_DEPTH;       tags[2].ti_Data = (ULONG)&depth;
	tags[3].ti_Tag = LBMI_PIXFMT;      tags[3].ti_Data = (ULONG)&pixfmt;
	tags[4].ti_Tag = TAG_DONE;         tags[4].ti_Data = 0;

	/* DIRECT ACCESS NEEDS LINEAR DISPLAY MEMORY, and not every card has it.
	 * CGX documents UnLockBitMapTagList(UBMI_UPDATERECTS) as the way to tell a
	 * non-linear card that something was written - without which the lock
	 * "succeeds" and nothing appears. Rather than carry that path, the lock
	 * route is simply refused on such a card and the library route used, which
	 * is correct everywhere and is what a non-linear card gets anyway. */
	if (!GetCyberMapAttr(g_screen->RastPort.BitMap, CYBRMATTR_ISLINEARMEM)) {
		amigagfx_log("blit: display memory is not linear - "
		             "using WritePixelArray rather than a direct lock");
		return;
	}

	handle = LockBitMapTagList((APTR)g_screen->RastPort.BitMap, tags);
	if (handle != NULL) UnLockBitMap(handle);   /* nothing at all in between */

	if (handle == NULL) {
		amigagfx_log("blit: LockBitMap refused - using WritePixelArray(RECTFMT_LUT8)");
		return;
	}
	if (base == NULL || depth != 8UL || pixfmt != PIXFMT_LUT8 ||
	    bpr < (ULONG)(g_xoff + g_vis_w)) {
		/* THE LOCK IS THE LAST WORD ON THE DEPTH. g_depth is read back from
		 * the opened screen, but if the lock disagrees with it the lock is
		 * what the pixels actually are, so that is what decides the route.
		 *
		 * (An earlier version of this comment claimed RECTFMT_LUT8 into a
		 * deeper rastport "would paint garbage". That is wrong on MorphOS: the
		 * documentation says it uses the colormap attached to the bitmap, and
		 * for a screen bitmap that is the viewport colormap LoadRGB32 has
		 * already filled. It is a legitimate second route and worth measuring
		 * against WriteLUTPixelArray one day. WriteLUTPixelArray is kept for
		 * now because it is the one the CGX drivers in the wild agree on.) */
		if (depth > 8UL) g_blit = BLIT_WLUT;
		fprintf(stdout, "morphos: blit: lock gave depth=%lu pixfmt=%lu bpr=%lu base=%p"
		                " - not a usable LUT8 surface, using %s\n",
		        (unsigned long)depth, (unsigned long)pixfmt,
		        (unsigned long)bpr, base,
		        g_blit == BLIT_WLUT ? "WriteLUTPixelArray" : "WritePixelArray");
		fflush(stdout);
		return;
	}

	g_blit = BLIT_LOCK;
	fprintf(stdout, "morphos: blit: direct LUT8 bitmap access, bpr %lu"
	                " (chunky pitch %d) - a memcpy per row and no conversion\n",
	        (unsigned long)bpr, g_pitch);
	fflush(stdout);
}

/* ---- double buffering --------------------------------------------------- */

static void dbuf_close(void)
{
	int i;
	/* The flip in flight owns its messages until they arrive. Leaving without
	 * collecting them frees ports that intuition still holds a pointer to. */
	if (g_sb_pending) {
		if (g_sb_safe != NULL) while (GetMsg(g_sb_safe) == NULL) WaitPort(g_sb_safe);
		if (g_sb_disp != NULL) while (GetMsg(g_sb_disp) == NULL) WaitPort(g_sb_disp);
		g_sb_pending = 0;
	}
	for (i = 0; i < 2; i++) {
		if (g_sb[i] != NULL) { FreeScreenBuffer(g_screen, g_sb[i]); g_sb[i] = NULL; }
	}
	if (g_sb_safe != NULL) { DeleteMsgPort(g_sb_safe); g_sb_safe = NULL; }
	if (g_sb_disp != NULL) { DeleteMsgPort(g_sb_disp); g_sb_disp = NULL; }
	g_dbuf = 0;
	g_sb_cur = 0;
}

static int dbuf_open(void)
{
	/* THE ESCAPE HATCH. This has never run on a MorphOS machine, and the one
	 * thing in it that can hang rather than merely fail is the Wait for the
	 * safe-to-draw message. A file called `nodbuf` beside the executable turns
	 * the whole thing off, so a player who meets that does not need a rebuild
	 * or a different download - the same trick backend.txt already uses. */
	{
		FILE *f = fopen("PROGDIR:nodbuf", "r");
		if (f != NULL) {
			fclose(f);
			amigagfx_log("double buffering off (PROGDIR:nodbuf exists)");
			return 0;
		}
	}

	g_sb_safe = CreateMsgPort();
	g_sb_disp = CreateMsgPort();
	if (g_sb_safe == NULL || g_sb_disp == NULL) { dbuf_close(); return 0; }

	/* SB_SCREEN_BITMAP for the first: it adopts the bitmap the screen already
	 * has rather than allocating a third. The second is a fresh one. */
	g_sb[0] = AllocScreenBuffer(g_screen, NULL, SB_SCREEN_BITMAP);
	g_sb[1] = AllocScreenBuffer(g_screen, NULL, 0);
	if (g_sb[0] == NULL || g_sb[1] == NULL) {
		amigagfx_log("double buffering unavailable - drawing straight to the screen");
		dbuf_close();
		return 0;
	}

	g_sb[0]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = g_sb_safe;
	g_sb[1]->sb_DBufInfo->dbi_SafeMessage.mn_ReplyPort = g_sb_safe;
	g_sb[0]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = g_sb_disp;
	g_sb[1]->sb_DBufInfo->dbi_DispMessage.mn_ReplyPort = g_sb_disp;

	/* A RastPort of our own pointing at the back buffer. Copied from the
	 * screen's so it inherits the pens and the layer-less setup, then aimed
	 * somewhere else - which is exactly what the CGX drivers do. */
	g_sb_rp = g_screen->RastPort;
	g_sb_rp.Layer = NULL;
	g_sb_cur = 1;                       /* draw into the one not displayed */
	g_sb_rp.BitMap = g_sb[g_sb_cur]->sb_BitMap;
	g_sb_pending = 0;
	g_dbuf = 1;

	amigagfx_log("double buffered - no tearing, and the blit never races the beam");
	return 1;
}

/* Show what was just drawn and move to the other buffer.
 *
 * Collect the previous flip's two messages FIRST: the display message says the
 * old back buffer is now on screen, the safe message says the old front is no
 * longer being read and can be drawn into. Only then is the next buffer ours.
 * Both waits are guarded by g_sb_pending, so the first frame does not wait for
 * a flip that never happened. */
static void dbuf_present(void)
{
	if (!g_dbuf) return;

	if (ChangeScreenBuffer(g_screen, g_sb[g_sb_cur]) == 0) {
		/* Refused - the frame is still correct, it just was not flipped.
		 * Try again next frame rather than tearing the state down. */
		return;
	}
	g_sb_pending = 1;
	g_sb_cur ^= 1;

	while (GetMsg(g_sb_disp) == NULL) WaitPort(g_sb_disp);
	while (GetMsg(g_sb_safe) == NULL) WaitPort(g_sb_safe);
	g_sb_pending = 0;

	g_sb_rp.BitMap = g_sb[g_sb_cur]->sb_BitMap;
}

/* ----------------------------------------------------------------- open --- */

/* Open the deepest screen this machine will give us at w x h, preferring 8 bits
 * because that is the format the renderer already produces.
 *
 * THE ORDER IS NOT AN OPINION. An 8-bit screen makes the blit a straight copy;
 * every other depth costs a conversion per pixel.
 *
 * AND THE RADEON CAN DO 8-BIT - an earlier version of this comment said it
 * frequently could not, which was wrong and worth correcting rather than
 * quietly deleting. These cards have a hardware colour lookup table and
 * MorphOS maintains it: the 3.18 release notes mention separate LUTs for
 * dual-head R5xx "to avoid problems with mixed palette/truecolour display",
 * and 3.20 fixed "8-bit bitmap blitting issues". No release note anywhere
 * deprecates 8-bit screens.
 *
 * What is actually true is narrower and user-fixable: an 8-bit mode AT THIS
 * SIZE has to exist in the machine's screenmode database, and a stock install
 * may carry only the deeper ones. BestCModeIDTagList returning INVALID_ID for
 * depth 8 means "not configured here", not "not possible here" - which is why
 * the README tells the player to add one in Preferences/Monitors rather than
 * telling them their card cannot do it.
 *
 * So 8 is asked for first and the rest are tried after - not as a degraded
 * mode, but as what an unconfigured machine actually has. */
static int open_the_screen(int w, int h, ULONG quiet, ULONG title)
{
	/* 16 BEFORE 32, and the reason is bandwidth rather than taste.
	 *
	 * Every frame is converted by the CPU and then pushed across the bus to
	 * the card: at 640x480 that is 614 KB per frame at 16 bits and 1228 KB at
	 * 32. The picture is 256 colours out of a fixed palette, so a 16-bit
	 * screen loses nothing visible - there is no gradient in it that 5-6-5
	 * cannot hold - and it halves the most expensive thing the display path
	 * does. 8 is still asked for first, because there the chunky buffer IS the
	 * display format and the conversion disappears entirely. */
	static const int depths[] = { 8, 16, 32, 24, 15 };
	unsigned int i;

	for (i = 0; i < sizeof depths / sizeof depths[0]; i++) {
		ULONG modeid = best_mode(w, h, depths[i]);
		if (modeid == (ULONG)INVALID_ID) continue;

		g_want_modeid = modeid;
		g_screen = OpenScreenTags(NULL,
		                          SA_Width,     (ULONG)w,
		                          SA_Height,    (ULONG)h,
		                          SA_Depth,     (ULONG)depths[i],
		                          SA_Type,      (ULONG)CUSTOMSCREEN,
		                          SA_Quiet,     quiet,
		                          SA_ShowTitle, title,
		                          SA_Title,     (ULONG)g_screen_title,
		                          SA_DetailPen, (ULONG)g_detail_pen,
		                          SA_BlockPen,  (ULONG)g_block_pen,
		                          SA_Pens,      (ULONG)g_screen_pens,
		                          SA_DisplayID, modeid,
		                          /* MorphOS has a tag for hiding the pointer on
		                           * a custom screen, which is better than the
		                           * blank-sprite trick in two ways: the pointer
		                           * never appears at all (the sprite is applied
		                           * after the window exists, so it flickered
		                           * once at startup), and nothing has to be
		                           * allocated in Chip RAM to say "nothing".
		                           * The sprite path stays for the runtime
		                           * toggle, which this tag cannot do. */
		                          SA_ShowPointer,
		                          (ULONG)(g_hide_pointer ? FALSE : TRUE),
		                          /* A screen blanker cutting in over a game
		                           * that is being played with a joypad, or
		                           * watched rather than touched, is a bug the
		                           * player blames on the game. */
		                          SA_StopBlanker, (ULONG)TRUE,
		                          TAG_END);
		if (g_screen != NULL) {
			g_depth = depths[i];
			return 0;
		}
		fprintf(stdout, "morphos: OpenScreen refused mode $%08lx at %dx%dx%d\n",
		        (unsigned long)modeid, w, h, depths[i]);
		fflush(stdout);
	}

	amigagfx_log("no CyberGraphX screen mode could be opened at all");
	return 5;
}

int amigagfx_open(int w, int h, int show_bar, int backend)
{
	/* SA_Quiet must be OFF when the bar is wanted, or Intuition renders no
	 * screen gadgetry at all; SA_ShowTitle keeps the bar in front of our
	 * backdrop window. The bar is Intuition's own - never a drawn imitation. */
	ULONG quiet = show_bar ? FALSE : TRUE;
	ULONG title = show_bar ? TRUE  : FALSE;
	int want_h;

	/* MorphOS has exactly one kind of display and it is the RTG one. A caller
	 * asking for AGA, EHB or a Workbench window is asking for hardware that is
	 * not here; it gets the screen this file can open, and one line says so
	 * rather than leaving it to look like the request was honoured. */
	if (backend != AMIGAGFX_BACKEND_RTG) {
		fprintf(stdout, "morphos: backend %d requested; this build has only the"
		                " RTG path (there are no bitplanes on PowerPC)\n", backend);
		fflush(stdout);
	}

	fprintf(stdout, "morphos: amigagfx_open(%d,%d) wb_bar=%d\n", w, h, show_bar);
	fflush(stdout);

	/* Reset every piece of geometry, because this is called again on a title-bar
	 * toggle (gta_main.c's toggle_bar closes and reopens) and a value left over
	 * from the previous screen is not a smaller bug than a wrong one. */
	g_width  = w;
	g_height = h;
	g_pitch  = w;
	g_vis_w  = w;
	g_vis_h  = h;
	g_xoff   = 0;
	g_yoff   = 0;
	g_bar    = 0;
	g_blits  = 0;
	g_epoch  = raw_ticks();

	/* ASK FOR THE BAR'S HEIGHT UP FRONT, not by shrinking the game area
	 * afterwards. The engine renders a fixed w x h and cannot reflow, so the
	 * screen has to be that much taller and the picture sits below the bar.
	 * The height is whatever Intuition reports for the mode and font it
	 * actually opened with, which is why the screen is opened twice when the
	 * bar is on: once to be told, once at the right size. */
	want_h = h;
	if (open_the_screen(w, want_h, quiet, title) != 0) { amigagfx_close(); return 5; }

	if (show_bar) {
		g_bar = (int)g_screen->BarHeight + 1;
		if (g_bar > 0 && g_bar < h / 2) {
			CloseScreen(g_screen); g_screen = NULL;
			want_h = h + g_bar;
			if (open_the_screen(w, want_h, quiet, title) != 0) {
				/* Taller refused - go back to the original size and let the
				 * bar eat into the picture, which is ugly but is a running
				 * game rather than no display at all. */
				want_h = h;
				if (open_the_screen(w, want_h, quiet, title) != 0) {
					amigagfx_close();
					return 5;
				}
			}
			g_bar = (int)g_screen->BarHeight + 1;
		} else {
			fprintf(stdout, "morphos: bar height %d implausible for %d lines"
			                " - bar ignored\n", g_bar, h);
			fflush(stdout);
			g_bar = 0;
		}
	}

	/* WHERE THE PICTURE SITS. MorphOS will not always grant a screen of the
	 * exact size asked for - a Radeon has no 320x240 mode and the driver hands
	 * back the nearest thing it does have - so the granted size is READ BACK
	 * and the game area is centred inside it rather than assumed to fill it.
	 * Without this the blit would run off the end of a screen that turned out
	 * narrower, and would draw in the top-left corner of one that turned out
	 * bigger. */
	{
		int sw = (int)g_screen->Width;
		int sh = (int)g_screen->Height;
		int avail_h = sh - g_bar;

		if (avail_h < 0) avail_h = 0;

		/* What the ENGINE draws never changes - see the note on g_vis_w. */
		g_width  = w;
		g_height = h;
		g_pitch  = w;

		/* What the SCREEN can show of it. */
		g_vis_w = (w < sw)      ? w : sw;
		g_vis_h = (h < avail_h) ? h : avail_h;

		g_xoff  = (sw - g_vis_w) / 2;
		g_yoff  = g_bar + (avail_h - g_vis_h) / 2;
		if (g_xoff < 0)   g_xoff = 0;
		if (g_yoff < g_bar) g_yoff = g_bar;

		/* WHERE A FRAME LANDS. Centred at its own size, or stretched to the
		 * whole screen below the bar - see amigagfx_set_scale. Scaling uses
		 * the FULL rendered size as its source, not the clipped visible one:
		 * the point of stretching is to show all of the picture. */
		if (g_scale) {
			g_dst_x = 0;
			g_dst_y = g_bar;
			g_dst_w = sw;
			g_dst_h = avail_h;
		} else {
			g_dst_x = g_xoff;
			g_dst_y = g_yoff;
			g_dst_w = g_vis_w;
			g_dst_h = g_vis_h;
		}

		if (g_vis_w != w || g_vis_h != h)
			fprintf(stdout, "morphos: asked for %dx%d, screen granted %dx%d"
			                " - showing %dx%d of the picture, the rest is"
			                " rendered and cropped\n",
			        w, h, sw, sh, g_vis_w, g_vis_h);
		fprintf(stdout, "morphos: screen %dx%dx%d, modeid wanted $%08lx got $%08lx,"
		                " game area %dx%d visible %dx%d at %d,%d (bar %d)\n",
		        sw, sh, g_depth, (unsigned long)g_want_modeid,
		        (unsigned long)GetVPModeID(&g_screen->ViewPort),
		        g_width, g_height, g_vis_w, g_vis_h, g_xoff, g_yoff, g_bar);
		fprintf(stdout, "morphos: picture %s - destination %dx%d at %d,%d\n",
		        g_scale ? "STRETCHED to the screen" : "centred at its own size",
		        g_dst_w, g_dst_h, g_dst_x, g_dst_y);
		fflush(stdout);
	}

	/* THE DEPTH WE ACTUALLY GOT, not the one we asked for.
	 *
	 * Intuition substitutes silently when a mode is unavailable, so depths[i]
	 * is a request and nothing more. Getting this wrong is not cosmetic: it
	 * decides between WriteLUTPixelArray and WritePixelArray(RECTFMT_LUT8),
	 * and RECTFMT_LUT8 into a 16-bit rastport is not a defined conversion - it
	 * would paint garbage with no error anywhere. It also decides whether
	 * LoadRGB32 is called on a screen that has no colour registers. */
	if (CyberGfxBase != NULL &&
	    GetCyberMapAttr(g_screen->RastPort.BitMap, CYBRMATTR_ISCYBERGFX)) {
		int got = (int)GetCyberMapAttr(g_screen->RastPort.BitMap, CYBRMATTR_DEPTH);
		if (got > 0 && got != g_depth) {
			fprintf(stdout, "morphos: asked for depth %d, screen is actually"
			                " %d - using that\n", g_depth, got);
			fflush(stdout);
			g_depth = got;
		}
	}

	/* Black the whole screen the instant it exists. Intuition opens it with
	 * the system's default colours and that flashed for a moment before the
	 * game loaded a palette; it also blacks the border around a game area that
	 * does not fill the screen, which is the only thing that ever paints there.
	 *
	 * This wipes the title bar too - it is pixels in the same bitmap, and
	 * Intuition has no reason to notice - so the bar is put back explicitly
	 * rather than left to be redrawn by whatever happens to touch the screen
	 * next. */
	SetRast(&g_screen->RastPort, 0);
	amigagfx_refresh_titlebar();
	if (g_depth <= 8) {
		static ULONG blank[2 + 256 * 3];
		blank[0] = (256UL << 16);
		LoadRGB32(&g_screen->ViewPort, blank);
	}

	probe_blit_method();

	/* After the blit method, because the lock path writes into the SCREEN's
	 * bitmap and double buffering moves that target - and before the chunky
	 * buffer, so a failure here is still a clean fall back to single. The lock
	 * path and double buffering are mutually exclusive for that reason: a
	 * locked write would go to whichever bitmap the screen is showing, not to
	 * the one being drawn. */
	if (g_blit == BLIT_LOCK) {
		amigagfx_log("direct LUT8 lock in use - single buffered, since a lock "
		             "writes to the displayed bitmap and not to the back one");
	} else {
		dbuf_open();
	}

	/* Sized for what the ENGINE renders, never for what the screen can show:
	 * gta_main.c draws a fixed SCREEN_W x SCREEN_H and never asks this file
	 * how much of it fits. */
	g_chunky = (UBYTE *)AllocVec((ULONG)g_pitch * g_height, MEMF_ANY | MEMF_CLEAR);
	if (g_chunky == NULL) { amigagfx_close(); return 1; }

	/* THE WINDOW COVERS THE WHOLE SCREEN BELOW THE BAR, not just the picture.
	 *
	 * It is only there to receive input, and a window that covers a
	 * sub-rectangle of the screen can be DEACTIVATED by a click on the screen
	 * behind it - after which IDCMP_RAWKEY goes elsewhere and the game stops
	 * responding to the keyboard while still rendering, which looks exactly
	 * like a hang. The 68k file is not exposed to this because there the
	 * screen is always exactly the size asked for and the window always spans
	 * it; here the screen is routinely larger. So the window takes everything,
	 * the picture is positioned by the blit alone, and amigagfx_poll subtracts
	 * the offsets to get back to game coordinates.
	 *
	 * The bar is left to Intuition: the depth gadget and screen drags stay
	 * system-handled, which is how a MorphOS user expects to get back to
	 * Ambient. */
	g_window = OpenWindowTags(NULL,
	                          WA_CustomScreen, (ULONG)g_screen,
	                          WA_Left,   0UL,
	                          WA_Top,    (ULONG)g_bar,
	                          WA_Width,  (ULONG)g_screen->Width,
	                          WA_Height, (ULONG)(g_screen->Height - g_bar),
	                          WA_Flags, (ULONG)(WFLG_BACKDROP | WFLG_BORDERLESS |
	                                            WFLG_ACTIVATE | WFLG_REPORTMOUSE |
	                                            WFLG_RMBTRAP | WFLG_NOCAREREFRESH),
	                          WA_IDCMP, (ULONG)(IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS |
	                                            IDCMP_RAWKEY | IDCMP_INTUITICKS),
	                          TAG_END);
	if (g_window == NULL) { amigagfx_close(); return 4; }

	/* WFLG_ACTIVATE alone did not reliably give a backdrop window key focus on
	 * the 68k side - keyboard events never arrived. Ask explicitly. */
	ActivateWindow(g_window);
	pointer_apply();

	amigagfx_log("window open, IDCMP active - handing control to the game");
	return 0;
}

void amigagfx_close(void)
{
	pointer_free();
	/* Before CloseScreen: FreeScreenBuffer needs the screen it belongs to. */
	if (g_dbuf || g_sb[0] != NULL || g_sb[1] != NULL) dbuf_close();
	if (g_window != NULL) { CloseWindow(g_window); g_window = NULL; }
	if (g_screen != NULL) { CloseScreen(g_screen); g_screen = NULL; }
	if (g_chunky != NULL) { FreeVec(g_chunky); g_chunky = NULL; }
	if (g_scratch != NULL) { FreeVec(g_scratch); g_scratch = NULL; g_scratch_px = 0; }
	/* The screen is gone, so nothing can be locked any more; hand the library
	 * back. A resolution change closes and reopens, which costs one OpenLibrary
	 * on an already-resident library - not worth keeping state for. */
	cgx_close();
	g_xoff = g_yoff = g_bar = 0;
	g_vis_w = g_vis_h = 0;
	g_depth = 0;
}

/* -------------------------------------------------------------- buffers --- */

unsigned char *amigagfx_chunky(void)     { return g_chunky; }
int            amigagfx_pitch(void)      { return g_pitch;  }
int            amigagfx_game_width(void) { return g_width;  }
int            amigagfx_game_height(void){ return g_height; }

/* -------------------------------------------------------------- palette --- */

void amigagfx_set_palette(const unsigned char *rgb, int first, int count)
{
	ULONG table[1 + 256 * 3 + 1];
	int i;

	if (g_screen == NULL || count <= 0) return;
	if (first < 0) first = 0;
	if (first + count > 256) count = 256 - first;

	table[0] = ((ULONG)count << 16) | (ULONG)first;
	for (i = 0; i < count; i++) {
		table[1 + i*3 + 0] = ((ULONG)rgb[i*3 + 0]) * 0x01010101UL;
		table[1 + i*3 + 1] = ((ULONG)rgb[i*3 + 1]) * 0x01010101UL;
		table[1 + i*3 + 2] = ((ULONG)rgb[i*3 + 2]) * 0x01010101UL;
		/* The CTABFMT_XRGB8 mirror WriteLUTPixelArray needs. Maintained
		 * unconditionally rather than only on a deep screen: it costs three
		 * shifts per changed colour, and on this target the deep screen is the
		 * likely case rather than the exception. */
		g_ctable[first + i] = ((ULONG)rgb[i*3 + 0] << 16) |
		                      ((ULONG)rgb[i*3 + 1] <<  8) |
		                       (ULONG)rgb[i*3 + 2];
	}
	table[1 + count * 3] = 0UL;

	/* A truecolour screen has no colour registers; the table above IS the
	 * palette there, handed to WriteLUTPixelArray on every blit. */
	if (g_depth <= 8) LoadRGB32(&g_screen->ViewPort, table);
}

/* ----------------------------------------------------------------- blit --- */

/* See the LOCKING DISCIPLINE note at the top before touching this. Every bound
 * is computed BEFORE the lock, nothing but memcpy happens inside it, and the
 * single UnLockBitMap is reached on every path including the "wrong format"
 * one. Returns 0 if the lock could not be used, which demotes this screen. */
static int blit_locked(int x, int y, int w, int h)
{
	APTR  handle;
	APTR  base   = NULL;
	ULONG bpr    = 0;
	ULONG depth  = 0;
	ULONG pixfmt = (ULONG)~0;
	struct TagItem tags[5];
	int   usable;

	tags[0].ti_Tag = LBMI_BASEADDRESS; tags[0].ti_Data = (ULONG)&base;
	tags[1].ti_Tag = LBMI_BYTESPERROW; tags[1].ti_Data = (ULONG)&bpr;
	tags[2].ti_Tag = LBMI_DEPTH;       tags[2].ti_Data = (ULONG)&depth;
	tags[3].ti_Tag = LBMI_PIXFMT;      tags[3].ti_Data = (ULONG)&pixfmt;
	tags[4].ti_Tag = TAG_DONE;         tags[4].ti_Data = 0;

	handle = LockBitMapTagList((APTR)g_screen->RastPort.BitMap, tags);
	if (handle == NULL) return 0;

	/* ---- LOCK HELD: memcpy only, no OS calls, no early return ---- */
	usable = (base != NULL && depth == 8UL && pixfmt == PIXFMT_LUT8 &&
	          bpr >= (ULONG)(g_xoff + x + w));
	if (usable) {
		const UBYTE *src = g_chunky + (ULONG)y * g_pitch + x;
		/* The destination is the SCREEN row and column: the chunky buffer
		 * covers the game area only, so the centring offsets are added here
		 * and nowhere else. bpr is the card's pitch, re-read every lock. */
		UBYTE *dst = (UBYTE *)base + (ULONG)(y + g_yoff) * bpr + (g_xoff + x);
		int row;
		for (row = 0; row < h; row++) {
			memcpy(dst, src, (size_t)w);
			src += g_pitch;
			dst += bpr;
		}
	}
	UnLockBitMap(handle);
	/* ---- LOCK RELEASED ---- */

	return usable;
}

/* 8-bit screen, library route. RECTFMT_LUT8 into an 8-bit destination writes
 * the bytes as PEN NUMBERS, unremapped - which is exactly right, because the
 * screen is ours and carries our palette. */
static void blit_pens(int x, int y, int w, int h)
{
	WritePixelArray((APTR)g_chunky, (UWORD)x, (UWORD)y, (UWORD)g_pitch,
	                target_rp(),
	                (UWORD)(g_xoff + x), (UWORD)(g_yoff + y),
	                (UWORD)w, (UWORD)h, (UBYTE)RECTFMT_LUT8);
}

/* Truecolour screen. Our chunky bytes plus our colour table; the library does
 * the conversion, which on PowerPC is cheap enough that a 320x240 frame does
 * not show up next to the cost of rendering it. */
static void blit_wlut(int x, int y, int w, int h)
{
	WriteLUTPixelArray((APTR)g_chunky, (UWORD)x, (UWORD)y, (UWORD)g_pitch,
	                   target_rp(), (APTR)g_ctable,
	                   (UWORD)(g_xoff + x), (UWORD)(g_yoff + y),
	                   (UWORD)w, (UWORD)h, (UBYTE)CTABFMT_XRGB8);
}

/* ---- the scaled paths --------------------------------------------------- *
 *
 * Both take the WHOLE rendered picture and put it on the whole destination,
 * because a stretched dirty rectangle is not a dirty rectangle: the
 * destination pixels a source rectangle maps onto depend on the scale factor
 * and do not line up with anything the caller knows about. gta_main.c blits
 * full-frame anyway, so nothing is lost by saying so plainly here.
 */

/* 8-bit screen: cybergraphics scales it for us. RECTFMT_LUT8 means the bytes
 * are pen numbers, which is right on a screen carrying our palette. */
static void blit_scaled_pens(void)
{
	ScalePixelArray((APTR)g_chunky, (UWORD)g_width, (UWORD)g_height,
	                (UWORD)g_pitch, target_rp(),
	                (UWORD)g_dst_x, (UWORD)g_dst_y,
	                (UWORD)g_dst_w, (UWORD)g_dst_h, (UBYTE)RECTFMT_LUT8);
}

/* Truecolour screen: there is no ScaleLUTPixelArray, so the palette lookup and
 * the stretch are done in one pass into a scratch buffer and the result is
 * pushed out as ARGB.
 *
 * Nearest neighbour, by a fixed-point step per axis. It is one table lookup and
 * one store per DESTINATION pixel, which on a full screen is the largest loop
 * in the frame - but it is also a loop a PowerPC eats, and the alternative
 * (converting then calling ScalePixelArray) walks the pixels twice.
 *
 * The source row index is computed once per output row rather than per pixel,
 * which is what keeps the inner loop to a load, a lookup and a store. */
static void blit_scaled_wlut(void)
{
	long need = (long)g_dst_w * g_dst_h;
	ULONG xstep, ystep, ypos;
	int dy;

	if (g_dst_w <= 0 || g_dst_h <= 0) return;

	if (g_scratch == NULL || g_scratch_px < need) {
		if (g_scratch != NULL) FreeVec(g_scratch);
		g_scratch = (ULONG *)AllocVec((ULONG)need * sizeof(ULONG), MEMF_ANY);
		g_scratch_px = (g_scratch != NULL) ? need : 0;
		if (g_scratch == NULL) return;      /* silently skip a frame, not crash */
	}

	/* 16.16 steps. g_dst_* are at least 1 here, so neither divides by zero. */
	xstep = ((ULONG)g_width  << 16) / (ULONG)g_dst_w;
	ystep = ((ULONG)g_height << 16) / (ULONG)g_dst_h;

	ypos = 0;
	for (dy = 0; dy < g_dst_h; dy++) {
		const UBYTE *src = g_chunky + (ULONG)(ypos >> 16) * g_pitch;
		ULONG *dst = g_scratch + (long)dy * g_dst_w;
		ULONG xpos = 0;
		int dx;
		for (dx = 0; dx < g_dst_w; dx++) {
			dst[dx] = g_ctable[src[xpos >> 16]];
			xpos += xstep;
		}
		ypos += ystep;
	}

	WritePixelArray((APTR)g_scratch, 0, 0, (UWORD)(g_dst_w * 4),
	                target_rp(),
	                (UWORD)g_dst_x, (UWORD)g_dst_y,
	                (UWORD)g_dst_w, (UWORD)g_dst_h, (UBYTE)RECTFMT_ARGB);
}

/* Truecolour, conversion on our side. Shares the scratch buffer with the
 * scaled path - both want the same thing, a block of native pixels in cached
 * memory - and reuses it across frames, because allocating one per frame would
 * be the most expensive thing in the loop. */
static void blit_raw(int x, int y, int w, int h)
{
	long need = (long)w * h;
	int row;

	if (g_scratch == NULL || g_scratch_px < need) {
		if (g_scratch != NULL) FreeVec(g_scratch);
		g_scratch = (ULONG *)AllocVec((ULONG)need * sizeof(ULONG), MEMF_ANY);
		g_scratch_px = (g_scratch != NULL) ? need : 0;
		if (g_scratch == NULL) { blit_wlut(x, y, w, h); return; }
	}

	for (row = 0; row < h; row++) {
		const UBYTE *src = g_chunky + (ULONG)(y + row) * g_pitch + x;
		ULONG *dst = g_scratch + (long)row * w;
		int i;
		for (i = 0; i < w; i++) dst[i] = g_ctable[src[i]];
	}

	WritePixelArray((APTR)g_scratch, 0, 0, (UWORD)(w * 4), target_rp(),
	                (UWORD)(g_xoff + x), (UWORD)(g_yoff + y),
	                (UWORD)w, (UWORD)h, (UBYTE)RECTFMT_ARGB);
}

void amigagfx_blit(int x, int y, int w, int h)
{
	int x2, y2;

	if (g_screen == NULL || g_chunky == NULL) return;

	/* The 32-pixel column granularity the 68k build snaps to is a property of
	 * Kalms' chunky-to-planar and of nothing else. There is no c2p here, so a
	 * rectangle is used exactly as given and only clipped - snapping it
	 * outwards would convert pixels that never changed, for no reason. */
	/* A FLIPPED FRAME IS ALWAYS WHOLE - see the note on the double-buffer
	 * state. Anything not redrawn this frame would otherwise show what was in
	 * this bitmap two frames ago. */
	if (g_dbuf) { x = 0; y = 0; w = g_width; h = g_height; }

	x2 = x + w;
	y2 = y + h;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	/* Clipped to what the SCREEN can show, not to what the engine rendered.
	 * When the two differ the extra was drawn into the chunky buffer and is
	 * simply not sent - which is why the buffer is allocated at the full
	 * rendered size and this is the only place the difference is applied. */
	if (x2 > g_vis_w) x2 = g_vis_w;
	if (y2 > g_vis_h) y2 = g_vis_h;
	if (x2 <= x || y2 <= y) return;

	w = x2 - x;
	h = y2 - y;

	/* SCALING TAKES OVER THE WHOLE BLIT. The rectangle has been clipped and
	 * found non-empty, which is all it was needed for - a scaled frame goes out
	 * whole, for the reason given above the two functions. */
	if (g_scale) {
		if (g_depth <= 8) blit_scaled_pens();
		else              blit_scaled_wlut();
		g_blits++;
		if (g_blits == 1 || (g_verbose && (g_blits % 200) == 0)) {
			fprintf(stdout, "morphos: blit #%lu  %dx%d -> %dx%d  %s\n",
			        g_blits, g_width, g_height, g_dst_w, g_dst_h,
			        g_depth <= 8 ? "ScalePixelArray(LUT8)"
			                     : "scale+LUT -> WritePixelArray(ARGB)");
			fflush(stdout);
		}
		dbuf_present();
		return;
	}

	if (g_blit == BLIT_LOCK && !blit_locked(x, y, w, h)) {
		/* Demote once, and for this screen only. Logged outside the lock. */
		g_blit = BLIT_PENS;
		if (!g_demoted) {
			g_demoted = 1;
			amigagfx_log("blit: LockBitMap became unusable - "
			             "switching to WritePixelArray for this screen");
		}
	}
	if (g_blit == BLIT_PENS) blit_pens(x, y, w, h);
	else if (g_blit == BLIT_RAW)  blit_raw(x, y, w, h);
	else if (g_blit == BLIT_WLUT) blit_wlut(x, y, w, h);

	/* The FIRST blit is logged always - it is the one-shot proof that the main
	 * loop reached the screen. The heartbeat costs I/O for the whole session,
	 * so it needs verbose. */
	g_blits++;
	if (g_blits == 1 || (g_verbose && (g_blits % 200) == 0)) {
		fprintf(stdout, "morphos: blit #%lu  %dx%d at %d,%d  %s\n",
		        g_blits, w, h, x, y,
		        g_blit == BLIT_LOCK ? "LockBitMap+memcpy" :
		        g_blit == BLIT_PENS ? "WritePixelArray(LUT8)" :
		        g_blit == BLIT_RAW  ? "LUT->ARGB + WritePixelArray"
		                            : "WriteLUTPixelArray");
		fflush(stdout);
	}
	dbuf_present();
}

/* ----------------------------------------------------------------- poll --- */

int amigagfx_poll(AmigaGfxEvent *ev)
{
	struct IntuiMessage *msg;

	ev->type = AMIGAGFX_EV_NONE;
	if (g_window == NULL) return 0;

	msg = (struct IntuiMessage *)GetMsg(g_window->UserPort);
	if (msg == NULL) return 0;

	{
		ULONG cls  = msg->Class;
		UWORD code = msg->Code;
		WORD  mx   = msg->MouseX;
		WORD  my   = msg->MouseY;
		ReplyMsg((struct Message *)msg);

		/* IDCMP coordinates are relative to the WINDOW, and the window covers
		 * the whole screen below the title bar rather than the picture (see
		 * the note on OpenWindowTags). So the picture's own offset has to come
		 * off, or a click lands as far out as the border is wide. The window
		 * already starts at y = g_bar, so only the part of g_yoff BELOW the bar
		 * is left to subtract. Nothing in AmiGTA reads the mouse, but a wrong
		 * coordinate that nobody looks at is still a trap for whoever does. */
		ev->x = mx - g_xoff;
		ev->y = my - (g_yoff - g_bar);
		ev->code = 0;

		switch (cls) {
		case IDCMP_MOUSEMOVE:
			ev->type = AMIGAGFX_EV_MOUSEMOVE;
			break;
		case IDCMP_CLOSEWINDOW:
			ev->type = AMIGAGFX_EV_QUIT;
			break;
		case IDCMP_MOUSEBUTTONS:
			switch (code) {
			case SELECTDOWN: ev->type = AMIGAGFX_EV_MOUSEDOWN; ev->code = AMIGAGFX_BUTTON_LEFT;  break;
			case SELECTUP:   ev->type = AMIGAGFX_EV_MOUSEUP;   ev->code = AMIGAGFX_BUTTON_LEFT;  break;
			case MENUDOWN:   ev->type = AMIGAGFX_EV_MOUSEDOWN; ev->code = AMIGAGFX_BUTTON_RIGHT; break;
			case MENUUP:     ev->type = AMIGAGFX_EV_MOUSEUP;   ev->code = AMIGAGFX_BUTTON_RIGHT; break;
			default: break;
			}
			break;
		case IDCMP_RAWKEY:
			/* Pass EVERY raw code through, releases included (bit 7 set): the
			 * game tracks held keys and needs both edges. MorphOS delivers the
			 * same raw key codes as AmigaOS, so gta_main.c's KEY_* table is
			 * correct here without a single change. */
			ev->type = AMIGAGFX_EV_KEY;
			ev->code = (int)code;
			break;
		default:
			break;
		}
	}
	return 1;
}

/* --------------------------------------------------------------- splash --- */

/* The splash is an AGA-era feature: it converts an image chunky-to-planar once
 * and animates the colour registers to fade it, neither of which exists on a
 * truecolour MorphOS screen. AmiGTA does not call it. Kept as a stub so
 * amiga_gfx.h stays one header for both targets. */
void amigagfx_splash(const char *path)
{
	(void)path;
}

#endif /* __MORPHOS__ */
