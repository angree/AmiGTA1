/* Native AmigaOS display and input: AGA (chunky->planar) and CyberGraphX RTG.
 *
 * Written by the author of this port for `openttd_amiga_68k`, carried through
 * `Amiga_OpenXCOM`, and reused here unchanged. It contains no upstream OpenTTD
 * code: it grew out of the standalone probes in that project's native/agac2p.c
 * and native/againput.c. The file previously carried OpenTTD's GPLv2 file
 * template, which was a copy-pasted header rather than a statement about
 * authorship; as sole author the licence here is MIT, like the rest of this
 * port.
 *
 * The chunky->planar kernels this drives (c2p_rect.s,
 * c2p1x1_6_c5_bm_040.s) are NOT ours - they are Mikael Kalms' routines, used
 * verbatim with their own headers intact. See LICENSING.md.
 */

/* Native AmigaOS display + input for OpenTTD. Plain C: see amiga_gfx.h for why.
 *
 * Everything here is the code proven in native/agac2p.c and native/againput.c:
 *   - one contiguous Chip RAM block for all 8 bitplanes, handed to Intuition
 *     with SA_BitMap (AllocBitMap does NOT guarantee equally spaced planes, and
 *     Kalms' c2p requires them)
 *   - a chunky 8bpp buffer in Fast RAM, which is what OpenTTD renders into
 *   - Kalms' c2p_rect for chunky->planar: measured 117 ms for a full 640x512
 *     frame on a 68040/25 versus 388 ms for graphics.library WritePixelArray8
 *   - a backdrop, borderless window purely to receive IDCMP input
 *
 * Since 2026-07-21 there is a SECOND display backend here: CyberGraphX/RTG.
 * On an 8-bit RTG screen the chunky buffer is already the display format, so
 * the whole chunky-to-planar step - by far the most expensive thing this file
 * does, 117 ms for a full 640x512 frame on an 040/25 - simply disappears and a
 * dirty rectangle becomes a per-row memcpy into the card's bitmap. The two
 * backends share everything above the blit (chunky buffer, dirty rectangles,
 * Intuition input window, palette, splash) and differ only in how a screen is
 * opened and how a rectangle reaches it.
 *
 * cybergraphics.library is targeted rather than the Picasso96-specific API,
 * because P96 ships a CGX-compatible layer: one implementation then serves both
 * P96 and genuine CyberGraphX users. The library is opened at runtime and
 * everything falls back to AGA if it is not there.
 */

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <graphics/gfx.h>
#include <graphics/displayinfo.h>
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/input.h>
#include <devices/inputevent.h>
#include <string.h>
#include <stdio.h>

/* cybergraphics.library is not part of the bebbo NDK, so the official CGX
 * developer headers are vendored under cgx-include/ and reached with an -I on
 * this file's (hand-written) compile line. The inline/ header there is the
 * repaired one: the 1995 FD2Inline original lists d0 both as the "=r" output
 * and in the clobber list and declares "register _res" with no type at all,
 * which GCC 6.5 either rejects or miscompiles. */
#include <proto/cybergraphics.h>

#include "amiga_gfx.h"

/* Declared extern by <proto/cybergraphics.h>; ours to define and to fill. */
struct Library *CyberGfxBase;

/* OpenTTD writes everything - DEBUG(), errors, the lot - to stderr, and the
 * AmigaDOS 3.1 shell can only redirect stdout, so a crash or a hang leaves no
 * trace at all. Point stderr at a file ourselves and make it UNBUFFERED, so the
 * last line written before a hang is actually on disk. */
void amigagfx_log(const char *msg)
{
	fprintf(stdout, "amiga: %s\n", msg);
	fflush(stdout);
}

/* Chatty per-play diagnostics (currently only the periodic "blit #N" line) are
 * off by default; the C++ driver switches them on for "-v amiga:verbose". */
static int g_verbose;

void amigagfx_set_verbose(int verbose)
{
	g_verbose = verbose;
}

/* ---- startup memory probe -----------------------------------------------
 * Which startup phase takes ~5 MB of Fast RAM and which gives it back?
 * One line per call: total free Fast, largest free Fast block, free Chip -
 * all in KB. Total vs largest matters because the machine has two Fast
 * regions (8 MB motherboard + 32 MB Z3): an allocation that lands in the
 * other region, or fragmentation, moves total-free by megabytes while
 * largest-block tells the real story. The log is only readable from the
 * host after the game exits, so one run must be conclusive: every line is
 * numbered and labelled, the first call truncates the file, and a hard cap
 * keeps it from flooding no matter how often a phase repeats. */
#define AMIGA_MEM_LOG      "PROGDIR:amiga_mem.log"
#define AMIGA_MEM_MAXLINES 100

/* Free Fast RAM right now, in bytes. Used by the file loader to decide whether
 * it can afford to hold whole files in memory. Same source as the memory probe
 * below, so the number in amiga_mem.log and the one the decision used agree. */
/* Largest CONTIGUOUS free Fast block, in bytes. Total free is not enough to
 * decide an allocation by: a 24 MB machine was measured with 16.3 MB free but
 * a largest block of 8191 KB - one kilobyte short of 8 MB, so an 8 MB malloc
 * failed and took the game down with it. */
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

void AmigaMemProbe(const char *label)
{
	static int mem_lines = 0;
	ULONG fast, largest, chip;
	FILE *f;

	if (mem_lines >= AMIGA_MEM_MAXLINES) return;

	/* Forbid so the three readings are one consistent snapshot. */
	Forbid();
	fast    = AvailMem(MEMF_FAST);
	largest = AvailMem(MEMF_FAST | MEMF_LARGEST);
	chip    = AvailMem(MEMF_CHIP);
	Permit();

	f = fopen(AMIGA_MEM_LOG, mem_lines == 0 ? "w" : "a");
	if (f == NULL) return;
	mem_lines++;
	fprintf(f, "M%03d %-26s fast=%luK largest=%luK chip=%luK\n",
	        mem_lines, label,
	        (unsigned long)(fast >> 10),
	        (unsigned long)(largest >> 10),
	        (unsigned long)(chip >> 10));
	fclose(f);
}

/* Argument blocks for the two assembly c2p routines; both layouts must match
 * native/c2p_glue.s exactly. See that file for why there are two. */
struct C2PArgs {
	UWORD x, y, w, h;
	UWORD cmod, bmod;
	ULONG bplsize;
	APTR  chunky;
	APTR  bpl;
};
extern void c2p_rect_asm(struct C2PArgs *a);

struct C2P6Args {
	UWORD chunkyx, chunkyy;
	UWORD offsx, offsy;
	APTR  chunky;
	APTR  bitmap;
};
extern void c2p6_bm_asm(struct C2P6Args *a);

/* Bitplanes per backend. EHB's six are the whole point of the mode: a quarter
 * less Chip RAM for the display and a quarter less work in the c2p, on a chipset
 * feature every Amiga back to the A1000 has. */
#define DEPTH_AGA 8
#define DEPTH_EHB 6

/* Rows of scratch for the EHB c2p, which needs contiguous chunky input (see
 * amigagfx_blit). A band rather than a full-screen copy: 64 rows of the widest
 * EHB mode is 22 KB, where a full-screen shadow of the chunky buffer would be
 * 160 KB standing idle for the whole session. The band is walked down the
 * rectangle, so height is never a limit - only how much is copied at once. */
#define EHB_SCRATCH_ROWS 64

static struct Screen *g_screen;
static struct Window *g_window;
static struct BitMap  g_bitmap;
static UBYTE *g_chip;
static UBYTE *g_chunky;
static UBYTE *g_ehb_scratch;              /* NULL unless an EHB screen is open */
static int    g_ehb_scratch_rows;
static UBYTE  g_ehb_pal[64 * 3];          /* set by amigagfx_set_ehb_palette */
static int    g_ehb_pal_valid;
static int    g_depth;                    /* bitplanes of the open planar screen */
static int    g_width, g_height, g_bpr;   /* g_height = GAME AREA height */
static int    g_yoff;                     /* first game-area line: 0, or the
                                           * system title bar height when the
                                           * bar is left visible */
static ULONG  g_planesize;
static ULONG  g_epoch;
static unsigned long g_blits;
static ULONG g_want_modeid;
static int   g_used_fallback;

/* ---- RTG (CyberGraphX / Picasso96) state --------------------------------
 *
 * g_backend says which of the two paths the CURRENTLY OPEN screen uses. It is
 * the single switch every backend-dependent decision below reads, so nothing
 * has to re-derive "am I on RTG" from the width or the mode id.
 *
 * How a rectangle reaches an 8-bit RTG screen - the two candidates, and why
 * this is the order:
 *
 *   RTG_METHOD_LOCK (preferred): LockBitMapTagList() hands back the base
 *     address and bytes-per-row of the screen's own bitmap, which on an 8-bit
 *     LUT8 screen holds exactly the bytes our chunky buffer holds. The blit is
 *     then a memcpy per row into card memory - no conversion and no
 *     intermediate copy, which is the entire reason to want RTG.
 *
 *   RTG_METHOD_WLUT (fallback): WriteLUTPixelArray() with a CTABFMT_XRGB8
 *     colour table. Always correct, but it goes through the library per
 *     rectangle and, because it is specified in terms of colours rather than
 *     pen numbers, it may remap every pixel instead of copying it. Used only if
 *     the lock is refused or reports something other than a LUT8 8-bit bitmap.
 *
 * The choice is made by ACTUALLY TAKING the lock once when the screen opens
 * (probe_rtg_lock) rather than by trusting the mode we asked for, and a later
 * failure demotes to WLUT permanently for that screen.
 *
 * LOCKING DISCIPLINE - getting this wrong hangs the machine, so it is stated
 * once and obeyed everywhere:
 *   1. Between LockBitMapTagList() and UnLockBitMap() this file calls NOTHING:
 *      no OS function, no logging, no Wait, no allocation. Only memcpy over
 *      memory whose bounds were computed BEFORE the lock was taken.
 *   2. Every path out of a locked region passes through exactly one
 *      UnLockBitMap(). The validity check inside the lock does not return
 *      early; it sets a flag, falls out, unlocks, and only then acts on it.
 *   3. The base address and bytes-per-row are valid ONLY inside the lock that
 *      produced them. They are re-read on every single lock and never cached
 *      across one - RTG display memory can move between locks.
 *   4. The lock is held for one rectangle at a time, never across the event
 *      loop and never across a frame.
 */
#define RTG_METHOD_LOCK 0
#define RTG_METHOD_WLUT 1

static int   g_backend;        /* AMIGAGFX_BACKEND_* of the open screen */
static int   g_rtg_method;
static int   g_rtg_demoted;    /* log the LOCK->WLUT demotion once */
static ULONG g_ctable[256];    /* CTABFMT_XRGB8 mirror of the palette */

/* ---- WINDOW mode (a window on the Workbench screen) ---------------------
 *
 * Everything above owns its display: a custom screen, our palette, our mode.
 * This backend owns none of that. The window is a guest on somebody else's
 * screen, so two things that are free everywhere else have to be worked for.
 *
 * COLOURS. The game is 8bpp with a palette it changes as it likes; the
 * Workbench screen is whatever the user set up. Two cases, and they are not
 * variations of one method - they are genuinely different code:
 *
 *   deeper than 8bpp (any RTG truecolor Workbench): there IS no palette to
 *     share. WriteLUTPixelArray takes our chunky bytes plus g_ctable and
 *     converts, so all 256 colours arrive EXACTLY and nothing is negotiated.
 *     This is the good case and it is also the common one on the machines
 *     likely to run a windowed OpenTTD at all.
 *
 *   8bpp or less: there are 256 pens on the screen and Workbench already owns
 *     some. ObtainBestPen is asked for each of our colours once, at open time,
 *     and hands back either a fresh pen, a shared one that already held that
 *     colour, or the nearest thing it has. g_lut maps a game colour index to
 *     the pen that will be drawn for it, and the blit remaps through it.
 *
 * WHY THE PENS ARE OBTAINED ONLY ONCE. OpenTTD animates its palette - water,
 * lights, the fizzy bits - every few ticks, and re-running 256 ObtainBestPen
 * calls per animation step would be hopeless on a 68030. So the pens are a
 * fixed resource taken at open time, their granted RGB is remembered in
 * g_pen_rgb, and a palette change only rebuilds g_lut by nearest-colour match
 * over that fixed set. A change is normally the ~28 animated entries rather
 * than all 256, which is why wb_lut_rebuild takes a range.
 *
 * SIZE. The window is resizable, and a resize must never fail: the chunky
 * buffer is therefore allocated ONCE at the full Workbench screen size and the
 * window is a sub-rectangle of it. g_pitch stays at that full width for the
 * whole session while g_width/g_height follow the window. That is the entire
 * reason g_pitch exists as a separate variable.
 */
#define WB_SCRATCH_ROWS 32     /* rows remapped per band on the pen path */

static struct Screen *g_pubscreen;   /* the screen our window sits on, or NULL */

/* ------------------------------------------------------- SYSTEM POINTER ---
 * Hide the Intuition (Workbench) mouse pointer over our window.
 *
 * Intuition has no "no pointer" call. The way to do it is SetPointer() with a
 * sprite made of zeros, and the sprite data MUST live in Chip RAM. One line is
 * enough: two words of header, one line x two bit planes, two words of
 * terminator. ClearPointer() puts the system pointer back.
 *
 * This is per WINDOW, which is what makes it safe in both of our modes. On our
 * own screen the pointer is simply gone. In a window on the Workbench it is
 * gone only while it is over our window, and the normal pointer returns over
 * everything else - which is what you want; a game has no business blanking
 * the pointer across the whole system.
 *
 * Independent of amiga.hide_game_pointer, which hides the cursor the GAME
 * draws. Turning both on leaves no pointer at all; that is allowed. */
static UWORD *g_blank_pointer;      /* Chip RAM, all zeros */
static int    g_hide_sys_pointer;   /* what the setting asked for */
static int    g_pointer_blanked;    /* what is actually on screen right now */

/* Make sure the Chip RAM sprite exists. Returns 0 if it could not be had, in
 * which case the pointer simply stays visible. */
static int wb_pointer_have_sprite(void)
{
	if (g_blank_pointer == NULL) {
		/* 6 words: 2 header + 1 line x 2 planes + 2 terminator. MEMF_CLEAR
		 * gives us the zeros, which is exactly what makes it invisible. */
		g_blank_pointer = (UWORD *)AllocVec(6 * sizeof(UWORD),
		                                    MEMF_CHIP | MEMF_CLEAR);
	}
	return g_blank_pointer != NULL;
}

/* While the loading splash is up the system pointer stays VISIBLE, so the
 * user sees the machine is alive and can flip to the Workbench and use it.
 * The game's blanking request is remembered and applied when the splash
 * hands the display back (amiga_splash.c drives this). */
static int g_pointer_suspended;

static void wb_pointer_blank(int on)
{
	if (g_pointer_suspended) on = 0;
	if (g_window == NULL || on == g_pointer_blanked) return;

	if (on) {
		if (!wb_pointer_have_sprite()) return;
		SetPointer(g_window, g_blank_pointer, 1, 16, 0, 0);
		g_pointer_blanked = 1;
	} else {
		ClearPointer(g_window);
		g_pointer_blanked = 0;
	}
}

/* Intuition ties a custom pointer to the ACTIVE WINDOW, not to where the mouse
 * happens to be. Blanking it once therefore left the pointer invisible across
 * the WHOLE Workbench for as long as our window stayed active - the mouse could
 * be over another window entirely and still have nothing to show.
 *
 * So we follow the mouse ourselves. IDCMP_MOUSEMOVE already arrives with
 * coordinates relative to the inside of the window, which makes "outside" just
 * a coordinate beyond 0..width / 0..height. The Intuition calls are made only
 * on a transition, not on every move.
 *
 * On our own screen the game window is a backdrop covering everything, so the
 * mouse is always inside and the pointer stays hidden - which is exactly what
 * that mode should do. */
static void wb_pointer_track(int inner_x, int inner_y)
{
	if (!g_hide_sys_pointer || g_window == NULL) return;

	wb_pointer_blank(inner_x >= 0 && inner_y >= 0 &&
	                 inner_x < g_width && inner_y < g_height);
}

static void wb_pointer_apply(void)
{
	if (g_window == NULL) return;
	/* Applied without knowing where the mouse is - assume inside, the next
	 * mouse move corrects it. */
	wb_pointer_blank(g_hide_sys_pointer);
}

/* on=1: force the system pointer visible (loading splash); on=0: go back to
 * whatever the setting asks for. */
void amigagfx_pointer_suspend(int on)
{
	g_pointer_suspended = on;
	if (g_window == NULL) return;
	if (on) {
		if (g_pointer_blanked) { ClearPointer(g_window); g_pointer_blanked = 0; }
	} else {
		wb_pointer_apply();
	}
}

void amigagfx_set_hide_system_pointer(int on)
{
	g_hide_sys_pointer = on ? 1 : 0;
	wb_pointer_apply();
}

static void wb_pointer_free(void)
{
	if (g_window != NULL) ClearPointer(g_window);
	if (g_blank_pointer != NULL) {
		FreeVec(g_blank_pointer);
		g_blank_pointer = NULL;
	}
	g_pointer_blanked = 0;
}

static int    g_pitch;               /* chunky stride; == g_width off window mode */
static int    g_win_mode;            /* non-zero while a Workbench window is open */
static int    g_win_depth;           /* depth of the public screen */
static int    g_win_direct;          /* non-zero: >8bpp, WriteLUTPixelArray path */
static int    g_win_bl, g_win_bt;    /* window border: inner area origin */
static int    g_win_maxw, g_win_maxh;/* public screen size = chunky buffer size */
static LONG   g_pen[256];            /* pen granted for game colour i, -1 none */
static UBYTE  g_pen_rgb[256 * 3];    /* what that pen ACTUALLY shows */
static int    g_pen_n;               /* how many of g_pen[] are valid */
static UBYTE  g_pen_owned[256];      /* 1 = WE obtained this pen and hold a reference */
static UBYTE  g_lut[256];            /* game colour index -> pen to draw */
static UBYTE *g_wb_scratch;          /* remap band, pen path only */
static struct RastPort g_temprp;     /* WritePixelArray8 scratch, no-CGX only */
static struct BitMap  *g_tempbm;

/* Shown in the screen title bar (the wb_bar option). Writable: the game sets
 * it through amigagfx_set_screen_title() - sdlmini routes SDL_WM_SetCaption
 * here - so the bar always shows the game's own name and version and this
 * file never hard-codes either. The default only ever shows if the screen
 * opens before the game names itself. */
static UBYTE g_screen_title[80] = "AmiGTA 68K";

void amigagfx_set_screen_title(const char *t)
{
	if (t != NULL && *t != '\0') {
		strncpy((char *)g_screen_title, t, sizeof(g_screen_title) - 1);
		g_screen_title[sizeof(g_screen_title) - 1] = '\0';
	}
	/* Applied live when a window exists: the screen bar shows the ACTIVE
	 * window's screen title, and our backdrop window is always active. */
	if (g_window != NULL)
		SetWindowTitles(g_window, (STRPTR)~0UL, (STRPTR)g_screen_title);
}

/* Intuition's default screen pens (DetailPen 0, BlockPen 1) index whatever
 * palette the GAME loads - and in the TTD *Windows* palette (OpenGFX's
 * default) indices 1..9 are magenta (212,0,212): that was the pink bar.
 * The indices below hold the SAME colour in both TTD palettes (DOS and
 * Windows, src/table/palettes.h) and lie outside the animated range
 * 217..254, so they survive UpdatePalette() and any base-set choice:
 *   0  = black          (0,0,0)
 *   15 = white          (252,252,252)
 *   17 = dark blue-grey (68,76,92)   - OpenTTD's own chrome family
 *   19 = mid blue-grey  (108,116,132)
 * Result: dark neutral bar, white legible title, black trim line. */
/* The 1.3-compatible pair, which OpenScreenTags takes on its own tags and
 * which the title bar actually draws with. Defaults match the table below. */
static ULONG g_detail_pen = 15;
static ULONG g_block_pen  = 17;

static UWORD g_screen_pens[] = {
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
	17,         /* BARBLOCKPEN     - screen title bar fill (3.x) */
	0,          /* BARTRIMPEN       - line under the bar         */
	(UWORD)~0
};

/* AND THOSE INDICES ARE ONLY RIGHT FOR OPENTTD'S PALETTE.
 *
 * A screen pen is an INDEX, and the colour it lands on is whatever palette the
 * game happens to load. The table above was chosen for TTD's palette, where 15
 * is white and 17 a dark blue-grey. Hand the same screen a different game's
 * 256 colours and the bar is drawn in two arbitrary ones - in GTA's palette
 * that came out as good as black on black, and the title bar looked like it
 * had simply failed to appear. It had not: it was there, in the wrong colours,
 * which is a far more confusing failure than a missing one.
 *
 * So a game with its own palette says which indices to use, and picks them
 * FROM that palette rather than guessing. Call before amigagfx_open(); after
 * it, the screen already has its pens. */
void amigagfx_set_bar_pens(int text, int fill, int trim)
{
	if (text < 0 || text > 255 || fill < 0 || fill > 255 ||
	    trim < 0 || trim > 255)
		return;

	/* SA_DetailPen and SA_BlockPen are passed SEPARATELY to OpenScreenTags and
	 * they OVERRIDE what SA_Pens says for exactly the two the title bar uses.
	 * They were hard-coded to 15 and 17 - TTD's white and dark blue-grey - so
	 * setting SA_Pens alone changed the screen's gadgetry and left the bar
	 * itself untouched. The symptom was precise and misleading: the depth
	 * gadget appeared (it draws from SHINEPEN/SHADOWPEN, which SA_Pens does
	 * control) while the bar's fill and its title stayed invisible. */
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
 * THIS EXISTS BECAUSE THE BAR IS PIXELS IN OUR OWN BITMAP. The AGA screen is
 * opened with SA_BitMap pointing at Chip RAM this file allocated, so Intuition
 * renders the bar into the same planes the game's chunky-to-planar writes to.
 * Anything that writes over rows 0..BarHeight erases it, and Intuition has no
 * reason to notice: it redraws the bar when something makes it - a click on
 * the depth gadget, for instance - and not before. The symptom was the bar
 * being invisible until the screen was clicked, which reads as "the bar never
 * opened" and is not that at all.
 *
 * The one thing in this port that does write there is the Chip RAM write
 * benchmark, which paints over the whole display ON PURPOSE (see
 * amigagfx_planes). Rather than make that benchmark tiptoe around the bar, the
 * bar is simply redrawn afterwards.
 *
 * Toggling ShowTitle off and on is what forces the redraw; there is no
 * "refresh the bar" call in Intuition. Harmless when there is no bar. */
void amigagfx_refresh_titlebar(void)
{
	if (g_screen == NULL) return;
	if (g_yoff <= 0) return;          /* no bar on this screen */
	ShowTitle(g_screen, FALSE);
	ShowTitle(g_screen, TRUE);
	if (g_window != NULL)
		SetWindowTitles(g_window, (STRPTR)~0UL, (STRPTR)g_screen_title);
}

int amigagfx_backend(void) { return g_backend; }

/* The AGA screen's bitplanes, as one contiguous Chip RAM block, and its size.
 *
 * FOR MEASUREMENT ONLY. It exists to answer one question that decides a large
 * piece of the port's direction: is chunky-to-planar expensive because of the
 * BIT SHUFFLE, or because writing to Chip RAM is slow? If it is the write,
 * then a planar tile blitter saves nothing - the same bytes still have to
 * reach the same memory, only in a different order - and the whole
 * planar/hardware-scroll design is dead before it is written. If it is the
 * shuffle, that design is worth building.
 *
 * NULL on RTG, where there are no bitplanes and no Chip RAM in the picture at
 * all. Writing here paints garbage on the visible screen, which is exactly
 * what makes it an honest measurement: it is the real display memory, with
 * display DMA competing for the same bus, not a quiet buffer somewhere else. */
unsigned char *amigagfx_planes(void) { return (unsigned char *)g_chip; }
long amigagfx_planes_bytes(void)
{
	return (long)g_planesize * (long)g_depth;
}

/* cybergraphics.library, opened on demand. V41 is the first version with
 * LockBitMapTagList and WriteLUTPixelArray, and it is also what Picasso96's
 * compatibility layer reports, so it is the right floor for both. */
static int cgx_open(void)
{
	if (CyberGfxBase != NULL) return 1;
	/* v41 IS A PREFERENCE, NOT A REQUIREMENT.
	 *
	 * Asking for 41 and giving up is how MorphOS ended up with no RTG at all:
	 * the screen path then fell through to a planar screen and c2p painted
	 * colour noise into it. Everything used here - BestCModeIDTagList,
	 * GetCyberMapAttr, WriteLUTPixelArray, LockBitMapTags - is v40, and the
	 * mode that comes back is validated three ways anyway (see
	 * rtg_best_mode), so a lower library cannot smuggle a bad one through. */
	CyberGfxBase = OpenLibrary((CONST_STRPTR)CYBERGFXNAME, 41L);
	if (CyberGfxBase == NULL)
		CyberGfxBase = OpenLibrary((CONST_STRPTR)CYBERGFXNAME, 40L);
	if (CyberGfxBase == NULL)
		CyberGfxBase = OpenLibrary((CONST_STRPTR)CYBERGFXNAME, 0L);
	if (CyberGfxBase == NULL) {
		amigagfx_log("no cybergraphics.library at all - RTG modes disabled");
		return 0;
	}
	{
		char b[80];
		snprintf(b, sizeof b, "cybergraphics.library v%d opened",
		         (int)((struct Library *)CyberGfxBase)->lib_Version);
		amigagfx_log(b);
	}
	return 1;
}

static void cgx_close(void)
{
	if (CyberGfxBase != NULL) { CloseLibrary(CyberGfxBase); CyberGfxBase = NULL; }
}

/* Ask CGX for the best 8-bit mode of at least w x h, then verify it really is
 * one. BestCModeIDTagList never fails outright: when nothing matches it returns
 * a NATIVE chipset mode id, which would silently put an "RTG" screen back on
 * the planar path with no c2p behind it - garbage on screen. So the result is
 * checked three ways (is it a Cybergraphics id, is it 8 bits deep, is it one
 * byte per pixel) before it is believed. Returns INVALID_ID if not usable. */
static ULONG rtg_best_mode(int w, int h)
{
	ULONG id;
	struct TagItem tags[4];

	if (!cgx_open()) return (ULONG)INVALID_ID;

	tags[0].ti_Tag = CYBRBIDTG_NominalWidth;  tags[0].ti_Data = (ULONG)w;
	tags[1].ti_Tag = CYBRBIDTG_NominalHeight; tags[1].ti_Data = (ULONG)h;
	tags[2].ti_Tag = CYBRBIDTG_Depth;         tags[2].ti_Data = 8UL;
	tags[3].ti_Tag = TAG_END;                 tags[3].ti_Data = 0UL;

	id = BestCModeIDTagList(tags);

	if (id == (ULONG)INVALID_ID) return (ULONG)INVALID_ID;
	if (!IsCyberModeID(id))      return (ULONG)INVALID_ID;
	if (GetCyberIDAttr(CYBRIDATTR_DEPTH, id) != 8UL) return (ULONG)INVALID_ID;
	if (GetCyberIDAttr(CYBRIDATTR_BPPIX, id) != 1UL) return (ULONG)INVALID_ID;

	return id;
}

int amigagfx_rtg_has_mode(int w, int h)
{
	ULONG id = rtg_best_mode(w, h);

	/* The probe runs while building the resolution list, long before any
	 * screen exists. Leave the library open only if it is already carrying an
	 * open screen; otherwise hand it straight back, so an AGA-only session
	 * never keeps cybergraphics.library referenced for nothing. */
	if (g_screen == NULL) cgx_close();

	return id != (ULONG)INVALID_ID;
}

/* Take the lock once, look at what we actually got, release it, and decide
 * which blit method this screen will use. Deliberately a real lock rather than
 * an inspection of the mode: the mode says what was requested, the lock says
 * what LockBitMapTagList will hand the blit every frame. */
static void probe_rtg_lock(void)
{
	APTR  handle;
	APTR  base   = NULL;
	ULONG bpr    = 0;
	ULONG depth  = 0;
	ULONG pixfmt = (ULONG)~0;
	struct TagItem tags[5];

	g_rtg_method = RTG_METHOD_WLUT;
	g_rtg_demoted = 0;

	tags[0].ti_Tag = LBMI_BASEADDRESS; tags[0].ti_Data = (ULONG)&base;
	tags[1].ti_Tag = LBMI_BYTESPERROW; tags[1].ti_Data = (ULONG)&bpr;
	tags[2].ti_Tag = LBMI_DEPTH;       tags[2].ti_Data = (ULONG)&depth;
	tags[3].ti_Tag = LBMI_PIXFMT;      tags[3].ti_Data = (ULONG)&pixfmt;
	tags[4].ti_Tag = TAG_DONE;         tags[4].ti_Data = 0;

	handle = LockBitMapTagList((APTR)g_screen->RastPort.BitMap, tags);
	if (handle != NULL) UnLockBitMap(handle);   /* nothing at all in between */

	if (handle == NULL) {
		amigagfx_log("RTG: LockBitMap refused - using WriteLUTPixelArray");
		return;
	}
	if (base == NULL || depth != 8UL || pixfmt != PIXFMT_LUT8 ||
	    bpr < (ULONG)g_width) {
		fprintf(stdout, "amiga: RTG: lock gave depth=%lu pixfmt=%lu bpr=%lu base=%p"
		                " - not a usable LUT8 surface, using WriteLUTPixelArray\n",
		        (unsigned long)depth, (unsigned long)pixfmt,
		        (unsigned long)bpr, base);
		fflush(stdout);
		return;
	}

	g_rtg_method = RTG_METHOD_LOCK;
	fprintf(stdout, "amiga: RTG: direct LUT8 bitmap access, bpr %lu (chunky pitch %d)"
	                " - no chunky-to-planar at all\n",
	        (unsigned long)bpr, g_width);
	fflush(stdout);
}

/* input.device, opened lazily and only for IND_WRITEEVENT: Intuition has no
 * "warp the pointer" call, but feeding an IECLASS_NEWPOINTERPOS event with
 * IESUBCLASS_PIXEL (screen + pixel coordinates) into the input stream moves
 * the system pointer to an absolute position. Needed for right-drag map
 * scrolling, where the pointer must not pile up at the screen edge.
 * g_input_state: 0 = not tried yet, 1 = open, -1 = failed (stay a no-op). */
static struct MsgPort  *g_input_mp;
static struct IOStdReq *g_input_io;
static int              g_input_state;

static int input_device_open(void)
{
	if (g_input_state != 0) return g_input_state > 0;
	g_input_state = -1;

	g_input_mp = CreateMsgPort();
	if (g_input_mp == NULL) {
		amigagfx_log("input.device: CreateMsgPort FAILED - pointer warp disabled");
		return 0;
	}

	g_input_io = (struct IOStdReq *)CreateIORequest(g_input_mp, sizeof(struct IOStdReq));
	if (g_input_io == NULL) {
		DeleteMsgPort(g_input_mp); g_input_mp = NULL;
		amigagfx_log("input.device: CreateIORequest FAILED - pointer warp disabled");
		return 0;
	}

	if (OpenDevice((CONST_STRPTR)"input.device", 0, (struct IORequest *)g_input_io, 0) != 0) {
		DeleteIORequest(g_input_io); g_input_io = NULL;
		DeleteMsgPort(g_input_mp);   g_input_mp = NULL;
		amigagfx_log("input.device: OpenDevice FAILED - pointer warp disabled");
		return 0;
	}

	amigagfx_log("input.device open - pointer warp ready (NEWPOINTERPOS/PIXEL)");
	g_input_state = 1;
	return 1;
}

static void input_device_close(void)
{
	if (g_input_state > 0) CloseDevice((struct IORequest *)g_input_io);
	if (g_input_io != NULL) { DeleteIORequest(g_input_io); g_input_io = NULL; }
	if (g_input_mp != NULL) { DeleteMsgPort(g_input_mp);   g_input_mp = NULL; }
	g_input_state = 0;
}

int amigagfx_warp_pointer(int x, int y)
{
	struct InputEvent ie;
	struct IEPointerPixel pp;
	BYTE err;

	if (g_screen == NULL && g_pubscreen == NULL) return 0;
	if (!input_device_open()) return 0;

	/* IECLASS_POINTERPOS did NOT work here: its ie_X/ie_Y are in the input
	 * device's own coordinate space, not screen pixels, and on our
	 * hires-interlaced screens the two differ per axis - the pointer never
	 * visibly moved. IECLASS_NEWPOINTERPOS (V36, so any OS 2.0+) with
	 * IESUBCLASS_PIXEL instead takes an explicit target screen plus true
	 * pixel coordinates in that screen, which is unambiguous in every mode. */
	/* Callers pass game-area coordinates; the screen wants absolute pixels in
	 * the screen the pointer is on. The warp echo comes back window-relative,
	 * i.e. already in game-area coordinates - the C++ side's echo matching
	 * needs no offset either way.
	 *
	 * Custom screen: shift past the title bar when it is visible.
	 * Window mode: the game area sits at the window's position PLUS its
	 * border, and the window can be anywhere on the Workbench screen, so both
	 * have to be added - a warp that ignored them would fling the pointer off
	 * to the top-left corner of the desktop. */
	if (g_win_mode) {
		pp.iepp_Screen     = g_pubscreen;
		pp.iepp_Position.X = (WORD)((int)g_window->LeftEdge + g_win_bl + x);
		pp.iepp_Position.Y = (WORD)((int)g_window->TopEdge  + g_win_bt + y);
	} else {
		pp.iepp_Screen     = g_screen;
		pp.iepp_Position.X = (WORD)x;
		pp.iepp_Position.Y = (WORD)(y + g_yoff);
	}

	memset(&ie, 0, sizeof(ie));        /* zeroes ie_NextEvent and ie_TimeStamp */
	ie.ie_Class        = IECLASS_NEWPOINTERPOS;
	ie.ie_SubClass     = IESUBCLASS_PIXEL;
	ie.ie_Code         = IECODE_NOBUTTON;
	ie.ie_Qualifier    = 0;            /* absolute: no IEQUALIFIER_RELATIVEMOUSE */
	ie.ie_EventAddress = &pp;          /* pp lives across DoIO - it is synchronous */

	g_input_io->io_Command = IND_WRITEEVENT;
	g_input_io->io_Data    = &ie;
	g_input_io->io_Length  = sizeof(ie);
	err = DoIO((struct IORequest *)g_input_io);

	{
		static int warps, fails;
		if ((err == 0 && warps < 6) || (err != 0 && fails < 4)) {
			char b[96];
			snprintf(b, sizeof(b), "warp sent: pixel %d,%d on %dx%d screen, DoIO rc=%d",
			         x, y, g_width, g_height, (int)err);
			amigagfx_log(b);
			if (err == 0) warps++; else fails++;
		}
	}
	return err == 0;
}

static ULONG raw_ticks(void)
{
	struct DateStamp ds;
	DateStamp(&ds);
	return (ULONG)ds.ds_Minute * 3000UL + (ULONG)ds.ds_Tick;
}

unsigned long amigagfx_millis(void)
{
	/* DateStamp ticks are 1/50 s. Coarse, but OpenTTD only needs this to pace
	 * its 30 ms game loop, not to time frames. */
	return (unsigned long)((raw_ticks() - g_epoch) * 20UL);
}

/* AGA and EHB: one contiguous Chip RAM block for equally spaced bitplanes,
 * handed to Intuition with SA_BitMap. depth is DEPTH_AGA (8) or DEPTH_EHB (6);
 * six additionally sets the chipset's Extra-Half-Brite bit in the mode id, which
 * is what makes registers 32..63 half-intensity copies of 0..31 and is the
 * difference between a real EHB screen and a 6-plane screen that merely uses 64
 * pens. Sets g_bpr / g_planesize / g_chip / g_bitmap / g_depth - none of which
 * exist on the RTG path, where display memory lives on the card and Chip RAM
 * must not be touched at all. Returns 0 on success. */
static int open_screen_aga(int w, int h, ULONG quiet, ULONG title, int depth)
{
	g_bpr    = ((w + 15) >> 4) << 1;
	g_planesize = (ULONG)g_bpr * h;   /* planes ALWAYS cover the full screen */

	g_chip = (UBYTE *)AllocMem(g_planesize * depth, MEMF_CHIP | MEMF_CLEAR);
	if (g_chip == NULL) return 2;

	InitBitMap(&g_bitmap, depth, w, h);
	{
		int i;
		for (i = 0; i < depth; i++)
			g_bitmap.Planes[i] = (PLANEPTR)(g_chip + (ULONG)i * g_planesize);
	}

	/* Pick the display mode from the size actually asked for, instead of always
	 * forcing hires-interlaced. 320-wide means lores, and anything at or below
	 * 256 lines fits a PAL frame without interlace - which also means no
	 * flicker, the main reason to want a lores mode in the first place. So
	 * 320x256 and 352x272 come out lores and 640x480 hires interlaced, with no
	 * special-casing anywhere. Kept general on purpose: it derives the mode from
	 * the size rather than from a table, so it stays correct whatever the
	 * resolution list happens to offer.
	 *
	 * EXTRAHALFBRITE_KEY is a LORES-only key - graphics/modeid.h defines it and
	 * EXTRAHALFBRITELACE_KEY and nothing else, because the chipset has no hires
	 * EHB. The resolution list is what keeps that promise: it offers EHB at
	 * lores widths only, so the OR below never produces an undefined mode. */
	{
		ULONG modeid = PAL_MONITOR_ID;
		int   lores  = (w <= 400);
		int   lace   = (h > 300);
		int   overscan;

		modeid |= lores ? LORES_KEY : HIRES_KEY;
		if (lace) modeid |= 0x0004;             /* LACE bit */
		if (depth == DEPTH_EHB) modeid |= EXTRAHALFBRITE_KEY;
		g_want_modeid = modeid;

		/* Does the requested size fit inside the STANDARD display clip for that
		 * mode, or does it need overscan?
		 *
		 * This decides one tag, and getting it wrong is a hard failure rather
		 * than a cosmetic one: a PAL lores non-interlaced screen is 320x256 as
		 * standard, so 352x272 does not fit, OpenScreen refuses it outright, and
		 * the driver reports "could not open a 352x272 screen (error 3)". That is
		 * exactly what happened to the 352x272 entries - both the AGA one and the
		 * OCS one, since OCS downgrades to 8 planes at the same size mid-session.
		 * SA_Overscan/OSCAN_MAX widens the clip to the largest the display can
		 * actually show, which is what an overscan mode is.
		 *
		 * Applied ONLY when the size genuinely exceeds the standard clip, so
		 * 320x256 and 640x480 - which fit and which work today - open with the
		 * exact same tag list they have always used. OSCAN_MAX moves the screen
		 * origin out to the overscan corner, and there is no reason to pay that
		 * for a mode that never needed it. */
		{
			int max_w = lores ? 320 : 640;   /* standard PAL width  for the mode */
			int max_h = lace  ? 512 : 256;   /* standard PAL height for the mode */
			overscan = (w > max_w || h > max_h);
		}

		g_screen = OpenScreenTags(NULL,
		                          SA_BitMap,    (ULONG)&g_bitmap,
		                          SA_Width,     (ULONG)w,
		                          SA_Height,    (ULONG)h,
		                          SA_Depth,     (ULONG)depth,
		                          SA_Type,      (ULONG)CUSTOMSCREEN,
		                          SA_Quiet,     quiet,
		                          SA_ShowTitle, title,
		                          SA_Title,     (ULONG)g_screen_title,
		                          SA_DetailPen, g_detail_pen,
		                          SA_BlockPen,  g_block_pen,
		                          SA_Pens,      (ULONG)g_screen_pens,
		                          SA_DisplayID, modeid,
		                          /* TAG_IGNORE leaves the tag out entirely, so a
		                           * fitting mode gets byte-identical treatment. */
		                          overscan ? SA_Overscan : TAG_IGNORE,
		                          (ULONG)OSCAN_MAX,
		                          TAG_END);
		if (overscan) {
			fprintf(stdout, "amiga: %dx%d exceeds the standard clip for mode $%08lx"
			                " - opening with OSCAN_MAX\n",
			        w, h, (unsigned long)modeid);
			fflush(stdout);
		}
	}
	if (g_screen == NULL && depth == DEPTH_AGA) {
		/* Fallback: the system picks. NOTE this drops our mode entirely, so a
		 * lores request silently becomes whatever Workbench runs - usually hires.
		 *
		 * Deliberately NOT done for EHB. Without SA_DisplayID Intuition picks a
		 * mode with no Extra-Half-Brite bit, and a 6-plane screen in an ordinary
		 * mode wants 64 independently settable registers - which ECS does not
		 * have at all, and which our palette is not written for either, since
		 * entries 32..63 are deliberately the hardware halves. The honest answer
		 * to a refused EHB mode is to report failure and let amigagfx_open retry
		 * at 8 planes, where the same reduced sprites still display correctly. */
		g_used_fallback = 1;
		g_screen = OpenScreenTags(NULL,
		                          SA_BitMap,    (ULONG)&g_bitmap,
		                          SA_Width,     (ULONG)w,
		                          SA_Height,    (ULONG)h,
		                          SA_Depth,     (ULONG)depth,
		                          SA_Type,      (ULONG)CUSTOMSCREEN,
		                          SA_Quiet,     quiet,
		                          SA_ShowTitle, title,
		                          SA_Title,     (ULONG)g_screen_title,
		                          SA_DetailPen, g_detail_pen,
		                          SA_BlockPen,  g_block_pen,
		                          SA_Pens,      (ULONG)g_screen_pens,
		                          TAG_END);
	}
	if (g_screen == NULL) {
		FreeMem(g_chip, g_planesize * depth); g_chip = NULL;
		if (depth == DEPTH_EHB) {
			fprintf(stdout, "amiga: EHB: OpenScreen refused mode $%08lx at %dx%d"
			                " - falling back to 8 bitplanes\n",
			        (unsigned long)g_want_modeid, w, h);
			fflush(stdout);
			return 7;
		}
		return 3;
	}

	g_depth   = depth;
	g_backend = (depth == DEPTH_EHB) ? AMIGAGFX_BACKEND_EHB : AMIGAGFX_BACKEND_AGA;

	/* Scratch band for the 6-plane c2p, which takes no chunky rowmod. Allocated
	 * with the screen so the blit never allocates; if it cannot be had, the EHB
	 * screen is not opened at all rather than blitting only full-width
	 * rectangles and leaving the rest of the display stale. */
	if (depth == DEPTH_EHB) {
		g_ehb_scratch_rows = EHB_SCRATCH_ROWS;
		g_ehb_scratch = (UBYTE *)AllocVec((ULONG)w * g_ehb_scratch_rows, MEMF_ANY);
		if (g_ehb_scratch == NULL) {
			CloseScreen(g_screen); g_screen = NULL;
			FreeMem(g_chip, g_planesize * depth); g_chip = NULL;
			g_ehb_scratch_rows = 0;
			amigagfx_log("EHB: no memory for the c2p scratch band"
			             " - falling back to 8 bitplanes");
			return 8;
		}
	}
	return 0;
}

/* IS THIS MACHINE GRAPHICS-CARD-ONLY?
 *
 * Asked when the RTG screen could not be had, to decide between a planar
 * screen and a window. The test is the Workbench screen's own depth: the
 * Amiga chipset cannot show more than 8 bitplanes, so a deeper public screen
 * means the display is a card and a planar screen here would be emulated -
 * Intuition grants it, c2p writes into bitplanes that are not laid out the way
 * it assumes, and the result is a recognisable city in shredded colours.
 *
 * Deliberately NOT asked through cybergraphics.library: the case this exists
 * for is precisely the one where that library could not be opened.
 *
 * MEASURED 2026-08-28 ON MORPHOS 3.20: IT DID NOT FIRE. The port still took
 * the planar path there and still drew colour noise. GetBitMapAttr is known
 * to report 8 for CGX bitmaps whatever the display really is, so this test is
 * probably answering the wrong question - but that is a guess, and guessing
 * is what produced three failed fixes in a row. What would settle it is the
 * port's own startup log, which could not be read inside MorphOS: the game
 * covers the console, the console never took keyboard focus, and the CD has
 * no serial.device or Port-Handler to mount SER: with. Until that log can be
 * read, `backend.txt` containing `wb` is the working answer on MorphOS and it
 * is documented as such. */
static int wb_screen_is_deep(void)
{
	struct Screen *pub = LockPubScreen(NULL);
	int deep = 0;

	if (pub != NULL) {
		if (pub->RastPort.BitMap != NULL)
			deep = ((int)GetBitMapAttr(pub->RastPort.BitMap, BMA_DEPTH) > 8);
		UnlockPubScreen(NULL, pub);
	}
	return deep;
}

/* RTG: an 8-bit CyberGraphX/Picasso96 screen. Deliberately NO SA_BitMap - the
 * card's display memory is allocated by CGX behind the mode id, so not one byte
 * of Chip RAM is spent here. Returns 0 on success; any non-zero means the
 * caller should open an AGA screen instead. */
static int open_screen_rtg(int w, int h, ULONG quiet, ULONG title)
{
	ULONG modeid = rtg_best_mode(w, h);

	if (modeid == (ULONG)INVALID_ID) {
		fprintf(stdout, "amiga: RTG: no 8-bit mode for %dx%d - falling back to AGA\n", w, h);
		fflush(stdout);
		return 5;
	}
	g_want_modeid = modeid;

	g_screen = OpenScreenTags(NULL,
	                          SA_Width,     (ULONG)w,
	                          SA_Height,    (ULONG)h,
	                          SA_Depth,     (ULONG)DEPTH_AGA,
	                          SA_Type,      (ULONG)CUSTOMSCREEN,
	                          SA_Quiet,     quiet,
	                          SA_ShowTitle, title,
	                          SA_Title,     (ULONG)g_screen_title,
	                          SA_DetailPen, g_detail_pen,
	                          SA_BlockPen,  g_block_pen,
	                          SA_Pens,      (ULONG)g_screen_pens,
	                          SA_DisplayID, modeid,
	                          TAG_END);
	if (g_screen == NULL) {
		fprintf(stdout, "amiga: RTG: OpenScreen refused mode $%08lx at %dx%d"
		                " - falling back to AGA\n", (unsigned long)modeid, w, h);
		fflush(stdout);
		return 6;
	}

	/* Nominal only: nothing on the RTG path allocates bitplanes or frees by
	 * depth - the card owns the display memory - but g_depth must not be left
	 * reading 6 from a previous EHB screen. */
	g_depth   = DEPTH_AGA;
	g_backend = AMIGAGFX_BACKEND_RTG;
	probe_rtg_lock();
	return 0;
}

/* ======================= WINDOW MODE ===================================== */

int amigagfx_wb_available(void)
{
	struct Screen *pub = LockPubScreen(NULL);
	if (pub == NULL) return 0;
	UnlockPubScreen(NULL, pub);
	return 1;
}

/* Hand every pen back. Reference counted by Intuition, so this must run EXACTLY
 * once per SUCCESSFUL ObtainBestPen. Duplicates in g_pen[] are NOT separate
 * references - they are copies of a pen number, taken without asking Intuition
 * again - so releasing them hands back pens we never took. Pass 2 deliberately
 * SHARES pens the Workbench already owns, so the over-release freed the
 * system its own colours: that is what turned NewIcons black into pink or
 * orange, a different shade on every run, and it happened on every full
 * palette update rather than only at exit. */
static void wb_pens_release(void)
{
	int i;

	if (g_pubscreen != NULL) {
		struct ColorMap *cm = g_pubscreen->ViewPort.ColorMap;
		for (i = 0; i < g_pen_n; i++) {
			if (g_pen_owned[i] && g_pen[i] >= 0) ReleasePen(cm, (ULONG)g_pen[i]);
		}
	}
	for (i = 0; i < 256; i++) { g_pen[i] = -1; g_pen_owned[i] = 0; }
	g_pen_n = 0;
}

/* Ask Intuition for a pen per game colour and record what it actually gave.
 *
 * TWO PASSES, and the order is the whole quality of the picture:
 *
 *   pass 1 - PRECISION_EXACT with OBP_FailIfBad TRUE. This allocates a fresh
 *     pen for a colour Workbench does not already have, and fails rather than
 *     fobbing us off with an approximation. Run first, so the free entries of
 *     the colour map are spent on colours that genuinely need them.
 *
 *   pass 2 - PRECISION_IMAGE with OBP_FailIfBad FALSE, for whatever pass 1
 *     could not place. This never fails: it hands back the nearest pen the
 *     screen already owns. That is the "adapt to the system palette" half of
 *     the job, and it only ever applies once the map is genuinely full.
 *
 * Doing pass 2's loose, sharing-happy request FIRST is the mistake worth
 * naming: PRECISION_IMAGE shares aggressively, so it answers the first few
 * colours with a handful of existing pens and never allocates anything, and a
 * 256-colour Workbench with 248 free entries yields a picture in about a dozen
 * colours. Exact-first is what actually uses the screen. */
static void wb_pens_obtain(void)
{
	struct ColorMap *cm;
	int i, pass, exact = 0, distinct = 0;
	char b[160];

	if (g_pubscreen == NULL) return;
	cm = g_pubscreen->ViewPort.ColorMap;

	wb_pens_release();
	for (i = 0; i < 256; i++) g_pen[i] = -1;

	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < 256; i++) {
			ULONG r, g, b8;
			LONG  p;

			if (g_pen[i] >= 0) continue;          /* placed by pass 1 */
			r  = (g_ctable[i] >> 16) & 0xff;
			g  = (g_ctable[i] >>  8) & 0xff;
			b8 =  g_ctable[i]        & 0xff;

			/* A colour we already hold a pen for costs nothing to reuse, and
			 * reusing it leaves a free map entry for a colour that has none.
			 * The TTD palettes repeat entries often enough for this to matter. */
			{
				int j, hit = -1;
				for (j = 0; j < i; j++) {
					if (g_pen[j] < 0) continue;
					if (g_ctable[j] == g_ctable[i]) { hit = j; break; }
				}
				if (hit >= 0) {
					g_pen[i] = g_pen[hit];
					memcpy(&g_pen_rgb[i * 3], &g_pen_rgb[hit * 3], 3);
					continue;
				}
			}

			p = ObtainBestPen(cm,
			                  r * 0x01010101UL, g * 0x01010101UL, b8 * 0x01010101UL,
			                  OBP_Precision, (ULONG)(pass == 0 ? PRECISION_EXACT
			                                                   : PRECISION_IMAGE),
			                  OBP_FailIfBad, (ULONG)(pass == 0 ? TRUE : FALSE),
			                  TAG_END);
			if (p < 0) {
				/* Pass 1 failing is normal and expected; pass 2 failing means
				 * there is no colour map to speak of, and the nearest-match
				 * table below will send this index to pen 0. */
				g_pen_rgb[i * 3 + 0] = (UBYTE)r;
				g_pen_rgb[i * 3 + 1] = (UBYTE)g;
				g_pen_rgb[i * 3 + 2] = (UBYTE)b8;
				continue;
			}
			g_pen[i] = p;
			g_pen_owned[i] = 1;   /* a real reference - release exactly this many */
			/* What the pen SHOWS, not what we asked for. The difference is the
			 * whole point: the nearest-match table must measure distance
			 * against reality, or an animated colour will jump to a pen that
			 * looks nothing like it. */
			{
				ULONG rgb[3];
				GetRGB32(cm, (ULONG)p, 1, rgb);
				g_pen_rgb[i * 3 + 0] = (UBYTE)(rgb[0] >> 24);
				g_pen_rgb[i * 3 + 1] = (UBYTE)(rgb[1] >> 24);
				g_pen_rgb[i * 3 + 2] = (UBYTE)(rgb[2] >> 24);
			}
		}
	}
	g_pen_n = 256;

	for (i = 0; i < 256; i++) {
		int j, seen = 0;
		if (g_pen[i] < 0) continue;
		if (g_pen_rgb[i * 3 + 0] == (UBYTE)((g_ctable[i] >> 16) & 0xff) &&
		    g_pen_rgb[i * 3 + 1] == (UBYTE)((g_ctable[i] >>  8) & 0xff) &&
		    g_pen_rgb[i * 3 + 2] == (UBYTE)( g_ctable[i]        & 0xff)) exact++;
		for (j = 0; j < i; j++) if (g_pen[j] == g_pen[i]) { seen = 1; break; }
		if (!seen) distinct++;
	}

	snprintf(b, sizeof(b),
	         "window: %d of 256 game colours exact, %d distinct pens held"
	         " on a %d-bit Workbench",
	         exact, distinct, g_win_depth);
	amigagfx_log(b);
}

/* Rebuild g_lut[first..first+count) - game colour index to the pen that will
 * actually be drawn - by nearest match over the pens obtained at open time.
 *
 * Ranged on purpose. OpenTTD's palette animation touches roughly 28 entries a
 * few times a second; doing all 256 each time would be 65k distance
 * computations where 7k will do. */
static void wb_lut_rebuild(int first, int count)
{
	int i;

	if (g_pen_n == 0) return;
	if (first < 0) first = 0;
	if (first + count > 256) count = 256 - first;

	for (i = first; i < first + count; i++) {
		int r = (int)((g_ctable[i] >> 16) & 0xff);
		int g = (int)((g_ctable[i] >>  8) & 0xff);
		int b = (int)( g_ctable[i]        & 0xff);
		int best = 0, bestd = 0x7fffffff, j;

		/* The pen taken for THIS index is normally still the right answer -
		 * only animated entries move - so try it first and skip the search
		 * when it is an exact hit. That is the common case by a wide margin. */
		if (g_pen[i] >= 0 &&
		    g_pen_rgb[i * 3 + 0] == (UBYTE)r &&
		    g_pen_rgb[i * 3 + 1] == (UBYTE)g &&
		    g_pen_rgb[i * 3 + 2] == (UBYTE)b) {
			g_lut[i] = (UBYTE)g_pen[i];
			continue;
		}

		for (j = 0; j < 256; j++) {
			int dr, dg, db, d;
			if (g_pen[j] < 0) continue;
			dr = r - (int)g_pen_rgb[j * 3 + 0];
			dg = g - (int)g_pen_rgb[j * 3 + 1];
			db = b - (int)g_pen_rgb[j * 3 + 2];
			d  = dr * dr + dg * dg + db * db;
			if (d < bestd) { bestd = d; best = (int)g_pen[j]; if (d == 0) break; }
		}
		g_lut[i] = (UBYTE)best;
	}
}

int amigagfx_wb_colours(int *granted)
{
	if (!g_win_mode) { if (granted != NULL) *granted = 256; return AMIGAGFX_WBCOL_OWN; }
	if (g_win_direct) { if (granted != NULL) *granted = 256; return AMIGAGFX_WBCOL_DIRECT; }
	if (granted != NULL) {
		int i, j, n = 0;
		for (i = 0; i < 256; i++) {
			int seen = 0;
			if (g_pen[i] < 0) continue;
			for (j = 0; j < i; j++) if (g_pen[j] == g_pen[i]) { seen = 1; break; }
			if (!seen) n++;
		}
		*granted = n;
	}
	return AMIGAGFX_WBCOL_PENS;
}

/* Inner (drawable) size of the window as it stands right now. Intuition
 * reports the OUTER size plus the border widths, and the borders differ per
 * window and per screen font, so the inner area is never assumed. */
static void wb_inner_size(int *w, int *h)
{
	*w = (int)g_window->Width  - (int)g_window->BorderLeft - (int)g_window->BorderRight;
	*h = (int)g_window->Height - (int)g_window->BorderTop  - (int)g_window->BorderBottom;
	if (*w < 1) *w = 1;
	if (*h < 1) *h = 1;
}

/* Open the game as a window on the default public screen. Returns 0, or
 * non-zero to tell amigagfx_open to fall back to a screen of its own. */
static int open_window_wb(int w, int h)
{
	struct Screen *pub;
	int inner_w, inner_h;
	char b[160];

	/* Wait a bounded moment for a public screen rather than giving up on the
	 * first try. The Startup-Sequence runs User-Startup - where this port is
	 * normally launched from - BEFORE LoadWB, so a game that starts with the
	 * machine genuinely does reach this point before Workbench exists, and
	 * failing there would make window mode impossible for exactly the setup
	 * most people have. Five seconds at ten ticks a go; the cost is paid only
	 * when window mode was explicitly asked for and Workbench is not up yet,
	 * and the fallback to a screen of our own is still there behind it. */
	{
		int tries;
		for (tries = 0; tries < 25; tries++) {
			pub = LockPubScreen(NULL);
			if (pub != NULL) break;
			if (tries == 0) amigagfx_log("window mode: no public screen yet - waiting for Workbench");
			Delay(10);
		}
	}
	if (pub == NULL) {
		amigagfx_log("window mode: no public screen after 5 s - is Workbench running?"
		             " (a game started from User-Startup runs before LoadWB)");
		return 10;
	}

	/* Depth decides the entire colour strategy, so it is read from the bitmap
	 * rather than from the mode id: a CGX screen can report a mode id whose
	 * depth bits mean nothing, and GetCyberMapAttr is the only honest answer
	 * on one. GetBitMapAttr covers the planar case. */
	if (cgx_open() && GetCyberMapAttr(pub->RastPort.BitMap, CYBRMATTR_ISCYBERGFX)) {
		g_win_depth = (int)GetCyberMapAttr(pub->RastPort.BitMap, CYBRMATTR_DEPTH);
	} else {
		g_win_depth = (int)GetBitMapAttr(pub->RastPort.BitMap, BMA_DEPTH);
	}
	g_win_direct = (g_win_depth > 8) ? 1 : 0;
	g_win_maxw   = (int)pub->Width;
	g_win_maxh   = (int)pub->Height;

	/* A truecolor Workbench needs WriteLUTPixelArray, which is the one thing
	 * here that genuinely requires the library. Without it there is no way to
	 * put 8-bit data on a 16/24-bit surface, so refuse rather than draw
	 * garbage - amigagfx_open then opens a screen of our own instead. */
	if (g_win_direct && CyberGfxBase == NULL) {
		UnlockPubScreen(NULL, pub);
		amigagfx_log("window mode: Workbench is deeper than 8 bit but"
		             " cybergraphics.library is missing - cannot convert");
		return 11;
	}

	if (w > g_win_maxw) w = g_win_maxw;
	if (h > g_win_maxh) h = g_win_maxh;

	g_window = OpenWindowTags(NULL,
	                          WA_PubScreen,   (ULONG)pub,
	                          WA_Title,       (ULONG)g_screen_title,
	                          WA_InnerWidth,  (ULONG)w,
	                          WA_InnerHeight, (ULONG)h,
	                          WA_MinWidth,    160UL, WA_MinHeight, 100UL,
	                          WA_MaxWidth,    (ULONG)g_win_maxw,
	                          WA_MaxHeight,   (ULONG)g_win_maxh,
	                          WA_Flags, (ULONG)(WFLG_DRAGBAR | WFLG_DEPTHGADGET |
	                                            WFLG_CLOSEGADGET | WFLG_SIZEGADGET |
	                                            WFLG_SIZEBRIGHT | WFLG_SIZEBBOTTOM |
	                                            WFLG_ACTIVATE | WFLG_REPORTMOUSE |
	                                            WFLG_RMBTRAP | WFLG_SMART_REFRESH |
	                                            WFLG_NOCAREREFRESH),
	                          /* IDCMP_NEWSIZE is what makes the window
	                           * resizable in any useful sense, and
	                           * IDCMP_CLOSEWINDOW gives the gadget meaning -
	                           * on a custom screen there is neither. */
	                          WA_IDCMP, (ULONG)(IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS |
	                                            IDCMP_RAWKEY | IDCMP_INTUITICKS |
	                                            IDCMP_NEWSIZE | IDCMP_CLOSEWINDOW),
	                          TAG_END);
	/* Unlock as soon as the window exists: the window itself now keeps the
	 * screen open, and holding a public screen lock any longer would block
	 * anyone trying to close it. */
	UnlockPubScreen(NULL, pub);
	if (g_window == NULL) {
		amigagfx_log("window mode: OpenWindow refused - falling back to a screen");
		return 12;
	}

	g_pubscreen = g_window->WScreen;
	g_win_bl = (int)g_window->BorderLeft;
	g_win_bt = (int)g_window->BorderTop;
	wb_inner_size(&inner_w, &inner_h);

	g_win_mode = 1;
	g_backend  = AMIGAGFX_BACKEND_WB;
	g_depth    = DEPTH_AGA;   /* nominal; nothing here allocates by depth */
	g_width    = inner_w;
	g_height   = inner_h;
	g_yoff     = 0;           /* the border offset is applied in wb_blit only */

	snprintf(b, sizeof(b),
	         "window: %dx%d inner on a %dx%d %d-bit %s Workbench, colours %s",
	         inner_w, inner_h, g_win_maxw, g_win_maxh, g_win_depth,
	         (CyberGfxBase != NULL && GetCyberMapAttr(g_pubscreen->RastPort.BitMap,
	                                                  CYBRMATTR_ISCYBERGFX)) ? "RTG" : "native",
	         g_win_direct ? "exact (colour table per blit)" : "negotiated (pens)");
	amigagfx_log(b);
	return 0;
}

/* Blit one rectangle into the window. Two entirely separate routes - see the
 * COLOURS note in the window state block. */
static void wb_blit(int x, int y, int w, int h)
{
	struct RastPort *rp = g_window->RPort;
	int dx = g_win_bl + x;
	int dy = g_win_bt + y;

	if (g_win_direct) {
		/* Deeper than 8 bit: hand the graphics card our chunky bytes and our
		 * colour table and let it convert. Exact, and no pens exist to share. */
		WriteLUTPixelArray((APTR)g_chunky,
		                   (UWORD)x, (UWORD)y, (UWORD)g_pitch,
		                   rp, (APTR)g_ctable,
		                   (UWORD)dx, (UWORD)dy,
		                   (UWORD)w, (UWORD)h,
		                   (UBYTE)CTABFMT_XRGB8);
		return;
	}

	/* 8 bit or less: remap game colour indices to the pens we hold, a band at
	 * a time, and push the pens straight out. The remap is a byte table lookup
	 * per pixel - the cheapest thing that can possibly work here. */
	if (g_wb_scratch == NULL) return;
	while (h > 0) {
		int band = (h < WB_SCRATCH_ROWS) ? h : WB_SCRATCH_ROWS;
		const UBYTE *src = g_chunky + (ULONG)y * g_pitch + x;
		UBYTE *dst = g_wb_scratch;
		int row, col;

		for (row = 0; row < band; row++) {
			for (col = 0; col < w; col++) dst[col] = g_lut[src[col]];
			src += g_pitch;
			dst += w;
		}

		if (CyberGfxBase != NULL) {
			/* RECTFMT_LUT8 into an 8-bit destination writes the bytes as PEN
			 * NUMBERS, unremapped - which is exactly right here, because the
			 * remap already happened in the loop above. (WriteChunkyPixels
			 * would say the same thing more directly, but it is a Picasso96
			 * extension and is not in the CyberGraphX API this port targets.) */
			WritePixelArray((APTR)g_wb_scratch, 0, 0, (UWORD)w,
			                rp, (UWORD)dx, (UWORD)(g_win_bt + y),
			                (UWORD)w, (UWORD)band, (UBYTE)RECTFMT_LUT8);
		} else if (g_tempbm != NULL) {
			/* Plain AGA Workbench. WritePixelArray8 wants a scratch RastPort
			 * whose bitmap is at least as wide as the rectangle, and it wants
			 * the width to be a multiple of 16 - both are guaranteed by the
			 * caller (amigagfx_blit snaps the rectangle) and by allocating the
			 * scratch bitmap at the full screen width. */
			WritePixelArray8(rp, (UWORD)dx, (UWORD)(g_win_bt + y),
			                 (UWORD)(dx + w - 1), (UWORD)(g_win_bt + y + band - 1),
			                 (APTR)g_wb_scratch, &g_temprp);
		}

		y += band;
		h -= band;
	}
}

/* Everything the window backend needs once the window itself exists. Split out
 * of amigagfx_open only because that function's tail is all screen business -
 * bar height, mode id, backdrop window - and none of it applies here.
 *
 * The chunky buffer is deliberately allocated at the FULL Workbench screen
 * size, not at the window size: a resize then costs nothing and cannot fail,
 * which is what makes the sizing gadget safe to drag around. g_pitch holds
 * that full width for the rest of the session. */
static int finish_window_open(void)
{
	int i;

	g_pitch  = g_win_maxw;
	g_chunky = (UBYTE *)AllocVec((ULONG)g_win_maxw * g_win_maxh, MEMF_ANY | MEMF_CLEAR);
	if (g_chunky == NULL) {
		amigagfx_log("window mode: no memory for the chunky buffer");
		amigagfx_close();
		return 1;
	}

	for (i = 0; i < 256; i++) { g_pen[i] = -1; g_lut[i] = (UBYTE)i; }
	g_pen_n = 0;

	if (!g_win_direct) {
		/* Remap band, and - only when there is no cybergraphics.library, i.e. a
		 * plain AGA Workbench - the scratch RastPort WritePixelArray8 insists
		 * on. Both are sized for the widest rectangle possible, so neither the
		 * blit nor a resize ever allocates. */
		g_wb_scratch = (UBYTE *)AllocVec((ULONG)g_win_maxw * WB_SCRATCH_ROWS, MEMF_ANY);
		if (g_wb_scratch == NULL) {
			amigagfx_log("window mode: no memory for the remap band");
			amigagfx_close();
			return 1;
		}
		if (CyberGfxBase == NULL) {
			InitRastPort(&g_temprp);
			/* Rounded to 16: WritePixelArray8 requires it of the array width,
			 * and the scratch bitmap must cover the widest one we can pass. */
			g_tempbm = AllocBitMap((ULONG)((g_win_maxw + 15) & ~15), 1UL,
			                       (ULONG)g_win_depth, 0UL,
			                       g_pubscreen->RastPort.BitMap);
			if (g_tempbm == NULL) {
				amigagfx_log("window mode: no scratch bitmap for WritePixelArray8");
				amigagfx_close();
				return 1;
			}
			g_temprp.BitMap = g_tempbm;
		}
	}

	ActivateWindow(g_window);
	fprintf(stdout, "amiga: window open on the Workbench screen,"
	                " chunky %dx%d (pitch %d) - handing control to the game\n",
	        g_width, g_height, g_pitch);
	fflush(stdout);
	return 0;
}

int amigagfx_open(int w, int h, int show_bar, int backend)
{
	/* SA_Quiet must be OFF when the bar is wanted, or Intuition renders no
	 * screen gadgetry at all; SA_ShowTitle keeps the bar in front of our
	 * backdrop window. The bar is Intuition's own - never a drawn imitation. */
	ULONG quiet = show_bar ? FALSE : TRUE;
	ULONG title = show_bar ? TRUE  : FALSE;
	int err;

	fprintf(stdout, "amiga: amigagfx_open(%d,%d) wb_bar=%d backend=%s\n",
	        w, h, show_bar,
	        backend == AMIGAGFX_BACKEND_WB  ? "WINDOW" :
	        backend == AMIGAGFX_BACKEND_RTG ? "RTG" :
	        backend == AMIGAGFX_BACKEND_EHB ? "EHB" : "AGA");
	fflush(stdout);
	g_width  = w;
	g_height = h;            /* provisional; reduced below if the bar shows */
	g_pitch  = w;
	g_yoff   = 0;
	g_used_fallback = 0;
	g_backend = AMIGAGFX_BACKEND_AGA;
	g_depth   = DEPTH_AGA;
	g_bpr = 0;
	g_planesize = 0;
	g_win_mode = 0;
	g_epoch  = raw_ticks();

	/* Window mode is handled entirely on its own: there is no screen to open,
	 * no title bar to subtract, no mode id to report and no backdrop window to
	 * add - the window IS the display. It falls back into the screen path
	 * below like any other backend when it cannot be had. */
	if (backend == AMIGAGFX_BACKEND_WB) {
		if (open_window_wb(w, h) == 0) return finish_window_open();
		amigagfx_log("window mode unavailable - opening a screen of our own instead");
		backend = AMIGAGFX_BACKEND_AGA;
	}

	/* Ask for what was requested; fall back to a plain 8-bitplane AGA screen
	 * whenever that did not work out. A machine with no graphics card therefore
	 * behaves exactly as it did before RTG support existed, one without the
	 * chipset mode behaves as it did before EHB did, and one log line always
	 * says why. */
	switch (backend) {
		case AMIGAGFX_BACKEND_RTG:
			err = open_screen_rtg(w, h, quiet, title);
			break;
		case AMIGAGFX_BACKEND_EHB:
			err = open_screen_aga(w, h, quiet, title, DEPTH_EHB);
			break;
		default:
			err = 5;
			break;
	}
	if (err != 0) {
		/* AN RTG-ONLY MACHINE MUST NOT FALL BACK TO A PLANAR SCREEN.
		 *
		 * If cybergraphics.library opened but had no 8-bit mode to give, this
		 * is a graphics-card-only system - MorphOS on a Pegasos, a Picasso96
		 * setup with no LUT8 modes. Intuition will still GRANT a planar
		 * screen there, so `open_screen_aga` below succeeds and c2p writes
		 * into bitplanes that are not laid out the way it assumes: the
		 * geometry comes out right and every colour is noise.
		 *
		 * Measured on MorphOS 3.20 under QEMU pegasos2, 2026-08-28: all three
		 * builds drew a recognisable but colour-shredded city, at 320x240,
		 * 640x480 and 800x600 alike, with `c2p` in the profile line. The same
		 * binary with `wb` in backend.txt drew Liberty City correctly - the
		 * window path goes through WriteLUTPixelArray, which does not care
		 * how deep the display is.
		 *
		 * So: window first, planar only if there is no window either. On a
		 * real Amiga with no card cgx_open() fails and none of this runs, so
		 * the AGA path is exactly what it always was. */
		if (backend == AMIGAGFX_BACKEND_RTG && wb_screen_is_deep()) {
			amigagfx_log("RTG has no 8-bit mode - a planar screen here would "
			             "be c2p garbage; opening a WINDOW instead");
			cgx_close();
			if (open_window_wb(w, h) == 0) return finish_window_open();
			amigagfx_log("...and no window either - falling back to planar");
		} else if (backend == AMIGAGFX_BACKEND_RTG) {
			cgx_close();
		}
		err = open_screen_aga(w, h, quiet, title, DEPTH_AGA);
		if (err != 0) { amigagfx_close(); return err; }
	}

	/* The bar height is whatever Intuition says it is for the mode and font it
	 * actually opened with - it is NEVER hard-coded. The bar itself occupies
	 * BarHeight+1 lines (BarHeight excludes the separator line).
	 *
	 * OpenXcom difference from the OpenTTD original: the game's UI is a FIXED
	 * 320x200 and cannot reflow, so the game area must not shrink under the
	 * bar (that mismatched the SDL surface against the mouse scale and cut the
	 * bottom of the screen). Instead the screen is closed and reopened
	 * bar-many lines TALLER, so the full requested h sits below the bar. Costs
	 * one extra open/close at startup; if the taller screen is refused, the
	 * old reduce-the-area behaviour is the fallback. */
	if (show_bar) {
		int bar = (int)g_screen->BarHeight + 1;
		if (bar > 0 && bar < h / 2) {
			int prev = g_backend;
			/* close only what open_screen_* allocated */
			if (g_screen != NULL) { CloseScreen(g_screen); g_screen = NULL; }
			if (g_chip != NULL)   { FreeMem(g_chip, g_planesize * g_depth); g_chip = NULL; }
			if (g_ehb_scratch != NULL) { FreeVec(g_ehb_scratch); g_ehb_scratch = NULL; }
			g_ehb_scratch_rows = 0;
			if (prev == AMIGAGFX_BACKEND_RTG)
				err = open_screen_rtg(w, h + bar, quiet, title);
			else
				err = open_screen_aga(w, h + bar, quiet, title,
				                      prev == AMIGAGFX_BACKEND_EHB ? DEPTH_EHB : DEPTH_AGA);
			if (err == 0) {
				bar = (int)g_screen->BarHeight + 1;   /* same mode, same font */
				g_yoff   = bar;
				g_height = h;
				fprintf(stdout, "amiga: wb bar: screen %dx%d, full game area %dx%d at y=%d\n",
				        w, h + bar, g_width, g_height, g_yoff);
			} else {
				/* taller refused - reopen at the original height and reduce */
				if (prev == AMIGAGFX_BACKEND_RTG)
					err = open_screen_rtg(w, h, quiet, title);
				else
					err = open_screen_aga(w, h, quiet, title,
					                      prev == AMIGAGFX_BACKEND_EHB ? DEPTH_EHB : DEPTH_AGA);
				if (err != 0) { amigagfx_close(); return err; }
				bar = (int)g_screen->BarHeight + 1;
				g_yoff   = bar;
				g_height = h - bar;
				fprintf(stdout, "amiga: wb bar: taller screen refused, reduced game area %dx%d at y=%d\n",
				        g_width, g_height, g_yoff);
			}
		} else {
			fprintf(stdout, "amiga: wb bar height %d implausible for %d lines - bar ignored\n",
			        bar, h);
		}
		fflush(stdout);
	}

	{
		/* AMIGA-PORT: blank the whole palette the instant the screen exists.
		 * Intuition opens it with the default colours (grey background, red
		 * bar pens), which flashed for a moment before the splash or the
		 * game loaded a real palette. All black until someone says otherwise. */
		{
			static ULONG blank_[2 + 256 * 3];
			blank_[0] = (256UL << 16);
			LoadRGB32(&g_screen->ViewPort, blank_);
		}
		/* Report the mode Intuition ACTUALLY granted, not the one we asked for.
		 * It substitutes silently when a mode is unavailable, and a lores-sized
		 * screen running in a hires mode looks identical in a log that only
		 * prints the pixel dimensions. */
		ULONG got = GetVPModeID(&g_screen->ViewPort);
		if (g_backend == AMIGAGFX_BACKEND_RTG) {
			fprintf(stdout, "amiga: RTG screen open %dx%d depth 8 (no Chip RAM, no c2p)\n", w, h);
			fprintf(stdout, "amiga: modeid wanted $%08lx got $%08lx  blit=%s\n",
			        (unsigned long)g_want_modeid, (unsigned long)got,
			        g_rtg_method == RTG_METHOD_LOCK ? "LockBitMap+memcpy"
			                                        : "WriteLUTPixelArray");
		} else {
			fprintf(stdout, "amiga: %s screen open %dx%d depth %d, bpr %d,"
			                " chip %lu KB\n",
			        g_backend == AMIGAGFX_BACKEND_EHB ? "EHB" : "AGA",
			        w, h, g_depth, g_bpr,
			        (unsigned long)((g_planesize * g_depth) >> 10));
			fprintf(stdout, "amiga: modeid wanted $%08lx got $%08lx  %s%s%s  [%s]\n",
			        (unsigned long)g_want_modeid, (unsigned long)got,
			        (got & HIRES_KEY) ? "HIRES " : "LORES ",
			        (got & 0x0004) ? "INTERLACED" : "non-interlaced",
			        (got & EXTRAHALFBRITE_KEY) ? " EHB" : "",
			        g_used_fallback ? "SYSTEM FALLBACK - our mode was refused"
			                        : "our mode accepted");
			/* The EHB bit is the whole mode. If Intuition granted the screen but
			 * dropped it, registers 32..63 are NOT hardware halves and half the
			 * palette is wrong - so say so loudly rather than let it look like a
			 * palette bug later. */
			if (g_backend == AMIGAGFX_BACKEND_EHB && !(got & EXTRAHALFBRITE_KEY)) {
				amigagfx_log("EHB: WARNING - granted mode has no EXTRAHALFBRITE bit;"
				             " colours 32..63 will not be hardware half-brights");
			}
		}
		fflush(stdout);
	}

	/* Chunky buffer covers the GAME AREA only, so any write past its bounds is
	 * a bug, not silently absorbed border space. Allocated after the screen
	 * opens because only then is the bar height - and thus g_height - known. */
	g_chunky = (UBYTE *)AllocVec((ULONG)g_width * g_height, MEMF_ANY | MEMF_CLEAR);
	if (g_chunky == NULL) { amigagfx_close(); return 1; }

	/* The input window covers only the game area, leaving the title bar to
	 * Intuition: clicks on the depth gadget and screen drags stay system-
	 * handled, and IDCMP mouse coordinates (window-relative) automatically
	 * become game-area coordinates. */
	g_window = OpenWindowTags(NULL,
	                          WA_CustomScreen, (ULONG)g_screen,
	                          WA_Left, 0UL, WA_Top, (ULONG)g_yoff,
	                          WA_Width, (ULONG)w, WA_Height, (ULONG)g_height,
	                          WA_Flags, (ULONG)(WFLG_BACKDROP | WFLG_BORDERLESS |
	                                            WFLG_ACTIVATE | WFLG_REPORTMOUSE |
	                                            WFLG_RMBTRAP | WFLG_NOCAREREFRESH),
	                          WA_IDCMP, (ULONG)(IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS |
	                                            IDCMP_RAWKEY | IDCMP_INTUITICKS),
	                          TAG_END);
	if (g_window == NULL) { amigagfx_close(); return 4; }

	/* WFLG_ACTIVATE alone did not reliably give a backdrop window key focus -
	 * keyboard events never arrived. Ask explicitly. */
	ActivateWindow(g_window);
	fprintf(stdout, "amiga: window open, IDCMP active - handing control to the game\n");
	return 0;
}

void amigagfx_close(void)
{
	wb_pointer_free();
	input_device_close();
	/* Pens are Intuition's, borrowed for the session, and they MUST go back
	 * before the window that justified holding them - otherwise the Workbench
	 * screen keeps them allocated for good and every later program sees a
	 * palette with no free entries. Released while g_pubscreen is still valid,
	 * i.e. before CloseWindow. */
	wb_pens_release();
	if (g_tempbm != NULL) { FreeBitMap(g_tempbm); g_tempbm = NULL; }
	if (g_wb_scratch != NULL) { FreeVec(g_wb_scratch); g_wb_scratch = NULL; }
	g_pubscreen = NULL;
	g_win_mode = 0;
	g_win_direct = 0;
	g_win_bl = g_win_bt = 0;
	if (g_window != NULL) { CloseWindow(g_window); g_window = NULL; }
	if (g_screen != NULL) { CloseScreen(g_screen); g_screen = NULL; }
	/* Chip RAM only ever exists on the AGA path; on RTG g_chip stays NULL and
	 * this is a no-op, which is exactly the point - the card owns the display
	 * memory and none of the machine's scarce Chip RAM is spent on it. */
	if (g_chip != NULL)   { FreeMem(g_chip, g_planesize * g_depth); g_chip = NULL; }
	if (g_chunky != NULL) { FreeVec(g_chunky); g_chunky = NULL; }
	if (g_ehb_scratch != NULL) { FreeVec(g_ehb_scratch); g_ehb_scratch = NULL; }
	g_ehb_scratch_rows = 0;
	g_depth = DEPTH_AGA;
	/* The screen is gone, so nothing can be locked any more; hand the library
	 * back. A resolution change closes and reopens it, which costs one
	 * OpenLibrary on an already-resident library - not worth keeping state for. */
	cgx_close();
	g_backend = AMIGAGFX_BACKEND_AGA;
	g_yoff = 0;
}

unsigned char *amigagfx_chunky(void) { return g_chunky; }
int amigagfx_pitch(void) { return g_pitch; }
int amigagfx_game_width(void) { return g_width; }
int amigagfx_game_height(void) { return g_height; }

void amigagfx_set_palette(const unsigned char *rgb, int first, int count)
{
	ULONG table[1 + 256 * 3 + 1];
	int i;

	if ((g_screen == NULL && !g_win_mode) || count <= 0) return;
	if (first < 0) first = 0;
	if (first + count > 256) count = 256 - first;

	table[0] = ((ULONG)count << 16) | (ULONG)first;
	for (i = 0; i < count; i++) {
		table[1 + i*3 + 0] = ((ULONG)rgb[i*3 + 0]) * 0x01010101UL;
		table[1 + i*3 + 1] = ((ULONG)rgb[i*3 + 1]) * 0x01010101UL;
		table[1 + i*3 + 2] = ((ULONG)rgb[i*3 + 2]) * 0x01010101UL;
		/* Mirror into the CTABFMT_XRGB8 table WriteLUTPixelArray needs. Kept
		 * up to date unconditionally rather than only on RTG: it costs three
		 * shifts per changed colour and means the WLUT fallback is always
		 * ready, including when a mid-session lock failure demotes to it. */
		g_ctable[first + i] = ((ULONG)rgb[i*3 + 0] << 16) |
		                      ((ULONG)rgb[i*3 + 1] <<  8) |
		                       (ULONG)rgb[i*3 + 2];
	}
	table[1 + count * 3] = 0UL;

	/* WINDOW MODE STOPS HERE, and the reason is worth stating plainly: that
	 * ViewPort belongs to WORKBENCH. Loading our palette into it would repaint
	 * the user's whole desktop in TTD colours - icons, other programs, the lot
	 * - which is precisely what a well-behaved window must never do. The
	 * colour table built above is all we need: on a deep screen it is handed
	 * to WriteLUTPixelArray per blit, and on a shallow one it feeds the pen
	 * table instead. */
	if (g_win_mode) {
		if (g_win_direct) return;
		/* A FULL 0..256 update re-negotiates the pens; anything smaller only
		 * re-maps against the pens already held.
		 *
		 * "Full update" rather than "the first update" is the important part.
		 * The first palette this driver ever sees is not the game's: startup
		 * order is open screen -> UpdatePalette -> splash (which fades the
		 * palette to black and back) -> restore. Negotiating on the first call
		 * therefore spent all the pens on the splash's palette - and, worse,
		 * on a moment when most of it was black, so nearly every colour
		 * collapsed onto one pen and the game ran in a handful of colours ever
		 * after. Re-obtaining on every full update means the last word belongs
		 * to whatever palette the game actually settled on. Full updates are
		 * rare - startup, splash, a base-set change - so the cost is not paid
		 * during play, where only the ~28 animated entries move. */
		if (first == 0 && count == 256) {
			wb_pens_obtain();
			wb_lut_rebuild(0, 256);
		} else {
			wb_lut_rebuild(first, count);
		}
		return;
	}

	LoadRGB32(&g_screen->ViewPort, table);
}

void amigagfx_set_ehb_palette(const unsigned char *rgb64)
{
	if (rgb64 == NULL) { g_ehb_pal_valid = 0; return; }
	memcpy(g_ehb_pal, rgb64, sizeof(g_ehb_pal));
	g_ehb_pal_valid = 1;
}

/* Nearest EHB pen for one RGB triple, plain squared distance over the 64
 * entries. Used only by the splash and only once per colour in its palette
 * (at most 256), so 16k distance computations for the whole image - nothing
 * worth a smarter search on a 68030. */
static UBYTE ehb_nearest(int r, int g, int b)
{
	int best = 0, bestd = 0x7fffffff, i;

	for (i = 0; i < 64; i++) {
		int dr = r - (int)g_ehb_pal[i * 3 + 0];
		int dg = g - (int)g_ehb_pal[i * 3 + 1];
		int db = b - (int)g_ehb_pal[i * 3 + 2];
		int d  = dr * dr + dg * dg + db * db;
		if (d < bestd) { bestd = d; best = i; }
	}
	return (UBYTE)best;
}

/* ---- Startup splash ------------------------------------------------------
 *
 * File format ("ASPL", written by build/make-splash.py), all fields big-endian
 * which on this 68k is simply native order - but they are still assembled
 * byte-by-byte so the loader does not depend on struct layout or alignment:
 *
 *   offset 0  : magic    4 bytes, ASCII "ASPL"
 *   offset 4  : uint16   width
 *   offset 6  : uint16   height
 *   offset 8  : uint16   ncolours (<= 256)
 *   offset 10 : uint16   reserved (0)
 *   offset 12 : palette  ncolours * 3 bytes, R,G,B
 *   then      : pixels   width * height bytes of palette indices
 *
 * Index 0 is pure black and fills the screen around the image, so the whole
 * screen fades as one. The fade NEVER re-runs the c2p or touches the chunky
 * buffer: the image is converted once, then only the colour registers are
 * rewritten with the stored RGB triples scaled by a 0..256 fixed-point
 * factor. A fade step is 256 palette writes, not a frame conversion. */

#define SPLASH_FADE_MS 500UL
#define SPLASH_HOLD_TICKS 125   /* Delay() ticks of 20 ms -> 2.5 s */

/* One palette write of the splash palette scaled by factor 0..256. */
static void splash_palette_step(const UBYTE *pal, int ncol, long factor)
{
	UBYTE rgb[256 * 3];
	int i;

	for (i = 0; i < ncol * 3; i++) {
		rgb[i] = (UBYTE)(((long)pal[i] * factor) >> 8);
	}
	amigagfx_set_palette(rgb, 0, ncol);
}

/* Ramp the palette factor from 'from' to 'to' over dur_ms, paced by the
 * DateStamp millisecond clock (20 ms granularity) and Delay(1). */
static void splash_fade(const UBYTE *pal, int ncol, long from, long to, ULONG dur_ms)
{
	ULONG t0 = amigagfx_millis();

	for (;;) {
		ULONG el = amigagfx_millis() - t0;
		if (el >= dur_ms) break;
		splash_palette_step(pal, ncol, from + ((to - from) * (long)el) / (long)dur_ms);
		Delay(1);
	}
	splash_palette_step(pal, ncol, to);   /* land exactly on the end value */
}

void amigagfx_splash(const char *path)
{
	FILE *f = NULL;
	UBYTE *pix = NULL;
	UBYTE hdr[12];
	UBYTE pal[256 * 3];
	UBYTE ehb_map[256];          /* file palette index -> EHB pen, EHB only */
	const UBYTE *fade_pal;       /* palette the fade actually animates */
	int fade_ncol;
	int is_ehb;
	int w, h, ncol, scale, dx, dy, x, y;
	char msg[128];

	if (g_screen == NULL || g_chunky == NULL) return;

	/* The splash must render in whatever mode the game opened, never the other
	 * way round: the entire audience for the EHB mode is machines that cannot
	 * show an 8-bitplane screen, so a splash that insisted on one would fail
	 * before the game started. On EHB the IMAGE is reduced to the screen's 64
	 * pens; the screen is not changed to suit the image. */
	is_ehb = (g_backend == AMIGAGFX_BACKEND_EHB);
	if (is_ehb && !g_ehb_pal_valid) {
		amigagfx_log("splash: EHB screen but no EHB palette was handed over - skipped");
		return;
	}

	f = fopen(path, "rb");
	if (f == NULL) {
		snprintf(msg, sizeof(msg), "splash: %s not found - skipped", path);
		amigagfx_log(msg);
		return;
	}

	if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
	    hdr[0] != 'A' || hdr[1] != 'S' || hdr[2] != 'P' || hdr[3] != 'L') {
		amigagfx_log("splash: bad or truncated header - skipped");
		fclose(f);
		return;
	}

	w    = ((int)hdr[4] << 8) | hdr[5];
	h    = ((int)hdr[6] << 8) | hdr[7];
	ncol = ((int)hdr[8] << 8) | hdr[9];

	/* 2x nearest-neighbour on tall (hires) screens, 1x on lores. */
	scale = (g_height >= 400) ? 2 : 1;

	if (w <= 0 || h <= 0 || ncol <= 0 || ncol > 256 ||
	    w * scale > g_width || h * scale > g_height) {
		snprintf(msg, sizeof(msg), "splash: %dx%d ncol %d does not fit %dx%d @%dx - skipped",
		         w, h, ncol, g_width, g_height, scale);
		amigagfx_log(msg);
		fclose(f);
		return;
	}

	pix = (UBYTE *)AllocVec((ULONG)w * h, MEMF_ANY);
	if (pix == NULL) {
		amigagfx_log("splash: no memory for pixels - skipped");
		fclose(f);
		return;
	}

	if (fread(pal, 1, (size_t)ncol * 3, f) != (size_t)ncol * 3 ||
	    fread(pix, 1, (size_t)w * h, f) != (size_t)w * h) {
		amigagfx_log("splash: file shorter than header claims - skipped");
		FreeVec(pix);
		fclose(f);
		return;
	}
	fclose(f);
	f = NULL;

	/* Which palette the screen will actually be showing, and therefore which one
	 * the fade animates.
	 *
	 * On AGA and RTG that is the file's own palette, loaded as-is - unchanged
	 * behaviour. On EHB it cannot be: there are 64 pens, and 32 of them are
	 * hardware halves that cannot be set at all, so an arbitrary 256-colour
	 * palette is simply not loadable. The image is therefore reduced to the EHB
	 * pens once, here, and the fade then scales the EHB palette. Because entries
	 * 32..63 of that palette are exactly the half-intensity images of 0..31,
	 * scaling all 64 by one factor keeps the table consistent with what the
	 * chipset derives - so the fade needs no special case of its own. */
	if (is_ehb) {
		int i;
		for (i = 0; i < ncol; i++) {
			ehb_map[i] = ehb_nearest((int)pal[i * 3 + 0],
			                         (int)pal[i * 3 + 1],
			                         (int)pal[i * 3 + 2]);
		}
		/* Index 0 is the border and must stay black in the reduced image too,
		 * or the surround fades up to whatever happened to be nearest. */
		ehb_map[0] = 0;
		fade_pal  = g_ehb_pal;
		fade_ncol = 64;
	} else {
		fade_pal  = pal;
		fade_ncol = ncol;
	}

	/* Black the palette out BEFORE the image reaches the screen, so the fade
	 * starts from darkness instead of flashing the game palette. */
	splash_palette_step(fade_pal, fade_ncol, 0);

	/* Compose once: black border (index 0), image centred. */
	memset(g_chunky, 0, (ULONG)g_width * g_height);
	dx = (g_width  - w * scale) / 2;
	dy = (g_height - h * scale) / 2;
	for (y = 0; y < h; y++) {
		const UBYTE *src = pix + (ULONG)y * w;
		UBYTE *dst = g_chunky + (ULONG)(dy + y * scale) * g_width + dx;
		if (scale == 1) {
			if (is_ehb) {
				for (x = 0; x < w; x++) dst[x] = ehb_map[src[x]];
			} else {
				memcpy(dst, src, (size_t)w);
			}
		} else {
			UBYTE *d = dst;
			if (is_ehb) {
				for (x = 0; x < w; x++) { UBYTE c = ehb_map[src[x]]; *d++ = c; *d++ = c; }
			} else {
				for (x = 0; x < w; x++) { UBYTE c = src[x]; *d++ = c; *d++ = c; }
			}
			memcpy(dst + g_width, dst, (size_t)w * 2);   /* double the row */
		}
	}
	FreeVec(pix);
	pix = NULL;

	/* The ONE chunky-to-planar conversion of the splash. */
	amigagfx_blit(0, 0, g_width, g_height);

	snprintf(msg, sizeof(msg), "splash: %dx%d ncol %d at %d,%d scale %dx%s",
	         w, h, ncol, dx, dy, scale, is_ehb ? " (reduced to 64 EHB pens)" : "");
	amigagfx_log(msg);

	/* Fade in, hold, fade out - palette-only from here on. */
	splash_fade(fade_pal, fade_ncol, 0, 256, SPLASH_FADE_MS);
	Delay(SPLASH_HOLD_TICKS);
	splash_fade(fade_pal, fade_ncol, 256, 0, SPLASH_FADE_MS);

	/* Leave the screen genuinely black: clear the chunky buffer and convert
	 * once more, so restoring the game palette cannot flash the image back. */
	memset(g_chunky, 0, (ULONG)g_width * g_height);
	amigagfx_blit(0, 0, g_width, g_height);
}

/* RTG blit, direct route: lock the screen's bitmap, memcpy the rectangle row by
 * row, unlock. On an 8-bit LUT8 surface our chunky bytes ARE the pen numbers,
 * so there is no conversion of any kind - this is the whole reason RTG is worth
 * having. Returns 0 if the lock could not be used, which permanently demotes
 * this screen to the WriteLUTPixelArray path.
 *
 * Read the LOCKING DISCIPLINE comment at the top of the RTG state block before
 * touching this. In short: every bound is computed BEFORE the lock, nothing but
 * memcpy happens inside it, and the single UnLockBitMap is reached on every
 * path including the "wrong format" one. */
static int rtg_blit_locked(int x, int y, int w, int h)
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
	          bpr >= (ULONG)(x + w));
	if (usable) {
		const UBYTE *src = g_chunky + (ULONG)y * g_width + x;
		/* The destination row is the SCREEN row: the chunky buffer covers the
		 * game area only, so the title bar offset is added here and nowhere
		 * else. bpr is the card's pitch and is NOT g_width - it is whatever
		 * this lock reported, re-read every time, never cached. */
		UBYTE *dst = (UBYTE *)base + (ULONG)(y + g_yoff) * bpr + x;
		int row;
		for (row = 0; row < h; row++) {
			memcpy(dst, src, (size_t)w);
			src += g_width;
			dst += bpr;
		}
	}
	UnLockBitMap(handle);
	/* ---- LOCK RELEASED ---- */

	return usable;
}

/* RTG blit, library route. Correct on any CGX screen but goes through the
 * library per rectangle and is specified in colours rather than pen numbers, so
 * it may remap every pixel where the locked path copies it. Only ever reached
 * when the lock is unavailable. */
static void rtg_blit_wlut(int x, int y, int w, int h)
{
	WriteLUTPixelArray((APTR)g_chunky,
	                   (UWORD)x, (UWORD)y, (UWORD)g_width,
	                   &g_screen->RastPort, (APTR)g_ctable,
	                   (UWORD)x, (UWORD)(y + g_yoff),
	                   (UWORD)w, (UWORD)h,
	                   (UBYTE)CTABFMT_XRGB8);
}

/* EHB blit: six bitplanes through Kalms' stock c2p1x1_6_c5_bm_040.
 *
 * That routine takes no chunky ROWMOD - unlike the 8-plane c2p_rect, which is
 * the only Kalms routine that does - so it insists its input be contiguous and
 * exactly chunkyx wide. Two cases follow, and the first one is why this is
 * cheap in practice:
 *
 *   full-width rectangle (x == 0, w == g_width): the rows of the chunky buffer
 *     ARE contiguous, so the buffer is handed over directly and nothing is
 *     copied at all. Most large dirty rectangles - and every full-screen
 *     redraw, which is the expensive case - go this way.
 *
 *   narrower rectangle: the rows are copied into a scratch band first, in
 *     chunks of at most EHB_SCRATCH_ROWS rows, and the routine is called once
 *     per band. One extra memcpy of exactly the pixels being converted.
 *
 * Copying rather than hand-writing a 6-plane rect routine is a deliberate
 * choice: this is released, validated assembly, and a memcpy is far cheaper
 * than the class of bug a bespoke c2p would introduce. */
static void ehb_blit(int x, int y, int w, int h)
{
	struct C2P6Args a;

	a.chunkyx = (UWORD)w;
	a.offsx   = (UWORD)x;
	a.bitmap  = (APTR)&g_bitmap;

	if (x == 0 && w == g_width) {
		a.chunkyy = (UWORD)h;
		a.offsy   = (UWORD)(y + g_yoff);
		a.chunky  = (APTR)(g_chunky + (ULONG)y * g_width);
		c2p6_bm_asm(&a);
		return;
	}

	while (h > 0) {
		int band = (h < g_ehb_scratch_rows) ? h : g_ehb_scratch_rows;
		const UBYTE *src = g_chunky + (ULONG)y * g_width + x;
		UBYTE *dst = g_ehb_scratch;
		int row;

		for (row = 0; row < band; row++) {
			memcpy(dst, src, (size_t)w);
			src += g_width;
			dst += w;
		}

		a.chunkyy = (UWORD)band;
		a.offsy   = (UWORD)(y + g_yoff);
		a.chunky  = (APTR)g_ehb_scratch;
		c2p6_bm_asm(&a);

		y += band;
		h -= band;
	}
}

void amigagfx_blit(int x, int y, int w, int h)
{
	struct C2PArgs args;
	int x2, y2;

	if (g_screen == NULL && !g_win_mode) return;

	x2 = x + w;
	y2 = y + h;

	if (g_win_mode) {
		/* WritePixelArray8 requires the array width to be a multiple of 16, so
		 * the rectangle is snapped outwards on that path and on that path only
		 * - CGX has no such rule and must not pay for one. */
		if (!g_win_direct && CyberGfxBase == NULL) {
			x  &= ~15;
			x2  = (x2 + 15) & ~15;
		}
		if (x < 0) x = 0;
		if (y < 0) y = 0;
		if (x2 > g_width)  x2 = g_width;
		if (y2 > g_height) y2 = g_height;
		if (x2 <= x || y2 <= y) return;

		wb_blit(x, y, x2 - x, y2 - y);

		g_blits++;
		if (g_blits == 1 || (g_verbose && (g_blits % 200) == 0)) {
			fprintf(stdout, "amiga: window blit #%lu  %dx%d at %d,%d  %s\n",
			        g_blits, x2 - x, y2 - y, x, y,
			        g_win_direct ? "WriteLUTPixelArray" :
			        (CyberGfxBase != NULL ? "pens+WritePixelArray(LUT8)"
			                              : "pens+WritePixelArray8"));
			fflush(stdout);
		}
		return;
	}

	/* The 32-pixel column granularity below is a property of the two c2p
	 * routines and of nothing else, so RTG must not pay for it: an RTG rectangle
	 * is used as given and only clipped. Snapping it outwards there would
	 * convert pixels that never changed, for no reason at all.
	 *
	 * Both planar routines need it, and both are satisfied by the same snap:
	 * c2p_rect wants x and width on a 32-pixel grid, and the 6-plane routine
	 * wants a width that is a multiple of 32 and an x offset that is a multiple
	 * of 8 - which a multiple of 32 already is. */
	if (g_backend != AMIGAGFX_BACKEND_RTG) {
		/* Kalms' c2p works on 32-pixel columns: grow the rect outwards to that
		 * grid rather than refusing it, then clip to the screen. */
		x  &= ~31;
		x2  = (x2 + 31) & ~31;
	}

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x2 > g_width)  x2 = g_width;
	if (y2 > g_height) y2 = g_height;
	if (x2 <= x || y2 <= y) return;

	if (g_backend == AMIGAGFX_BACKEND_RTG) {
		if (g_rtg_method == RTG_METHOD_LOCK &&
		    !rtg_blit_locked(x, y, x2 - x, y2 - y)) {
			/* Demote once and for this screen only. Logged outside the lock. */
			g_rtg_method = RTG_METHOD_WLUT;
			if (!g_rtg_demoted) {
				g_rtg_demoted = 1;
				amigagfx_log("RTG: LockBitMap became unusable - "
				             "switching to WriteLUTPixelArray for this screen");
			}
		}
		if (g_rtg_method == RTG_METHOD_WLUT) rtg_blit_wlut(x, y, x2 - x, y2 - y);

		g_blits++;
		if (g_blits == 1 || (g_verbose && (g_blits % 200) == 0)) {
			fprintf(stdout, "amiga: rtg blit #%lu  %dx%d at %d,%d\n",
			        g_blits, x2 - x, y2 - y, x, y);
			fflush(stdout);
		}
		return;
	}

	if (g_backend == AMIGAGFX_BACKEND_EHB) {
		ehb_blit(x, y, x2 - x, y2 - y);

		g_blits++;
		if (g_blits == 1 || (g_verbose && (g_blits % 200) == 0)) {
			fprintf(stdout, "amiga: ehb blit #%lu  %dx%d at %d,%d  %s\n",
			        g_blits, x2 - x, y2 - y, x, y,
			        (x == 0 && x2 == g_width) ? "direct" : "via scratch band");
			fflush(stdout);
		}
		return;
	}

	args.x = (UWORD)x;
	args.y = (UWORD)y;
	args.w = (UWORD)(x2 - x);
	args.h = (UWORD)(y2 - y);
	args.cmod = (UWORD)g_width;
	args.bmod = (UWORD)g_bpr;
	args.bplsize = g_planesize;
	args.chunky = g_chunky;
	/* Skip the title bar lines when the bar is visible: plane spacing
	 * (bplsize) stays the full-screen plane size, only the row origin moves.
	 * g_bpr is a multiple of 4 for every offered width, so the 32-pixel column
	 * alignment of the c2p is unaffected. */
	args.bpl = g_chip + (ULONG)g_yoff * g_bpr;
	c2p_rect_asm(&args);

	/* The FIRST blit is logged always - it is the one-shot proof that the main
	 * loop reached the screen. The every-200th heartbeat costs I/O for the
	 * whole session, so it now needs "-v amiga:verbose". */
	g_blits++;
	if (g_blits == 1 || (g_verbose && (g_blits % 200) == 0)) {
		fprintf(stdout, "amiga: blit #%lu  %dx%d at %d,%d\n",
		        g_blits, (int)args.w, (int)args.h, (int)args.x, (int)args.y);
		fflush(stdout);
	}
}

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

		/* IDCMP mouse coordinates are relative to the window's OUTER left/top.
		 * On a custom screen the backdrop window is borderless and the two are
		 * the same; a Workbench window has a title bar and frame, so the
		 * border has to come off or every click lands high and left. */
		ev->x = mx - g_win_bl;
		ev->y = my - g_win_bt;
		ev->code = 0;

		switch (cls) {
		case IDCMP_MOUSEMOVE:
			wb_pointer_track(ev->x, ev->y);
			ev->type = AMIGAGFX_EV_MOUSEMOVE;
			break;
		case IDCMP_CLOSEWINDOW:
			/* Window mode only - a custom screen has no close gadget. Treated
			 * as a quit request, which OpenTTD turns into its normal "really
			 * quit?" flow rather than an immediate exit. */
			ev->type = AMIGAGFX_EV_QUIT;
			break;
		case IDCMP_NEWSIZE: {
			/* Apply the new size HERE, on the C side, because everything that
			 * has to change is C-side state: the window's inner area, and the
			 * width/height the blit clips against. Nothing is reallocated -
			 * the chunky buffer was sized for the whole Workbench screen when
			 * the window opened, precisely so that this cannot fail. */
			int nw, nh;
			wb_inner_size(&nw, &nh);
			g_win_bl = (int)g_window->BorderLeft;
			g_win_bt = (int)g_window->BorderTop;
			if (nw > g_win_maxw) nw = g_win_maxw;
			if (nh > g_win_maxh) nh = g_win_maxh;
			if (nw == g_width && nh == g_height) break;   /* a move, not a resize */
			g_width  = nw;
			g_height = nh;
			ev->type = AMIGAGFX_EV_RESIZE;
			ev->x = nw;
			ev->y = nh;
			/* Logged unconditionally, unlike the per-frame chatter: a resize is
			 * a rare, user-driven event, and it is the one thing about window
			 * mode that cannot be checked from a screenshot after the fact. */
			fprintf(stdout, "amiga: window resized to %dx%d (pitch stays %d)\n",
			        nw, nh, g_pitch);
			fflush(stdout);
			break;
		}
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
			 * C++ side needs them to track shift and control. ESC is no longer
			 * swallowed as "quit" either - the game wants it for closing
			 * windows, and quitting belongs in the menu. */
			ev->type = AMIGAGFX_EV_KEY;
			ev->code = (int)code;
			break;
		default:
			break;
		}
	}
	return 1;
}
