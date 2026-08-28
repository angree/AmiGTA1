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

/* Thin C API over the native AmigaOS display and input.
 *
 * DELIBERATELY free of Amiga headers. Including <proto/exec.h> from OpenTTD C++
 * drags in dos/intuition/graphics, whose macros (Insert, Remove, Allocate) and
 * types (struct ViewPort, Point) collide head-on with OpenTTD's own names - that
 * collision is what sank the previous attempt. All Amiga code therefore lives in
 * amiga_gfx.c, compiled as plain C, and the C++ side sees only this file.
 */

#ifndef AMIGA_GFX_H
#define AMIGA_GFX_H

#ifdef __cplusplus
extern "C" {
#endif

/* Event kinds handed back by amigagfx_poll(). */
#define AMIGAGFX_EV_NONE      0
#define AMIGAGFX_EV_MOUSEMOVE 1
#define AMIGAGFX_EV_MOUSEDOWN 2
#define AMIGAGFX_EV_MOUSEUP   3
#define AMIGAGFX_EV_KEY       4
#define AMIGAGFX_EV_QUIT      5
/* Window mode only: the player dragged the sizing gadget. ev.x/ev.y carry the
 * NEW game area in pixels, already applied on this side - the caller must
 * re-read amigagfx_chunky()/pitch()/game_width()/game_height() and tell
 * OpenTTD, then redraw everything. */
#define AMIGAGFX_EV_RESIZE    6

/* Button ids for MOUSEDOWN / MOUSEUP. */
#define AMIGAGFX_BUTTON_LEFT  0
#define AMIGAGFX_BUTTON_RIGHT 1

typedef struct {
	int type;    /* AMIGAGFX_EV_* */
	int x, y;    /* pointer position, screen coordinates */
	int code;    /* button id, or raw key code */
} AmigaGfxEvent;

/* Which display backend a screen runs on. Passed to amigagfx_open() and
 * reported back by amigagfx_backend() - which is what actually opened, and may
 * differ from what was asked for (see the fallback notes on amigagfx_open).
 *
 *   AGA - planar Intuition screen, 8 bitplanes in one contiguous Chip RAM
 *         block, every dirty rectangle pushed through Kalms' chunky-to-planar.
 *   EHB - planar Intuition screen in the chipset's Extra-Half-Brite mode: SIX
 *         bitplanes, so a quarter less Chip RAM and a quarter less c2p work
 *         than AGA, and it is a mode plain OCS/ECS machines have too. Colour
 *         registers 32..63 are fixed by the hardware at half the intensity of
 *         0..31, which is why the palette handed to amigagfx_set_ehb_palette()
 *         must already obey that rule. A different c2p is used here - see the
 *         contiguity note on amigagfx_blit.
 *   RTG - 8-bit CyberGraphX/Picasso96 screen. The chunky buffer IS the display
 *         format there, so the c2p disappears entirely and a blit becomes a
 *         per-row memcpy into the card's bitmap. No Chip RAM is used at all.
 *   WB  - NOT a screen at all: a normal, resizable, draggable Intuition WINDOW
 *         on the Workbench (default public) screen, sharing that screen's
 *         palette instead of owning one. The only backend where the game does
 *         not control the display mode, and therefore the only one that has to
 *         negotiate for colours - see amigagfx_wb_colours(). Also the only one
 *         whose size can change while it is open (AMIGAGFX_EV_RESIZE). */
#define AMIGAGFX_BACKEND_AGA 0
#define AMIGAGFX_BACKEND_RTG 1
#define AMIGAGFX_BACKEND_EHB 2
#define AMIGAGFX_BACKEND_WB  3

/* Is an 8-bit RTG mode of at least w x h available on this machine? Returns 0
 * when cybergraphics.library is missing (a plain AGA Amiga), when the library
 * offers no matching 8-bit mode, or when the "best" mode it returns turns out
 * not to be a Cybergraphics mode at all - BestCModeIDTagList happily falls back
 * to a native chipset mode, which would put us back on the c2p path wearing an
 * RTG label. Safe to call before amigagfx_open(); used to decide which RTG
 * entries to offer in the resolution list at all, so an AGA-only machine sees
 * exactly the list it saw before RTG support existed. */
int amigagfx_rtg_has_mode(int w, int h);

/* AMIGAGFX_BACKEND_* of the screen currently open (AGA when none is). */
int amigagfx_backend(void);

/* The AGA screen's bitplanes as one contiguous Chip RAM block, and its size in
 * bytes. NULL / 0 on RTG, where there are no bitplanes at all.
 *
 * FOR MEASUREMENT ONLY - see the comment on the definition. It answers whether
 * c2p is expensive because of the bit shuffle or because Chip RAM is slow to
 * write, which is what decides whether a planar tile blitter would save
 * anything. Writing here paints garbage on the visible screen. */
unsigned char *amigagfx_planes(void);
long           amigagfx_planes_bytes(void);

/* Open a w x h screen on the requested backend - 256 colours on AGA and RTG,
 * 64 on EHB. Returns 0 on success, non-zero on failure.
 *
 * backend AMIGAGFX_BACKEND_RTG asks for a CyberGraphX/Picasso96 8-bit screen
 * and SILENTLY FALLS BACK to AGA if anything about it fails (no library, no
 * such mode, OpenScreen refused) - one log line says which and why, and the
 * game keeps running. amigagfx_backend() then reports AGA.
 *
 * backend AMIGAGFX_BACKEND_EHB asks for a 6-bitplane Extra-Half-Brite screen
 * and falls back the same way, to a normal 8-bitplane AGA screen, if the
 * chipset or Intuition will not give one. That fallback is harmless rather
 * than merely survivable: the caller has already reduced its sprites into the
 * 64-entry EHB index space, and those indices display correctly on an 8-plane
 * screen carrying the same 64 colours in registers 0..63. The player loses the
 * memory and speed win, not the picture.
 *
 * On AGA and EHB w must be a multiple of 32 (both c2p routines work in
 * 32-pixel columns); on RTG there is no such constraint and the caller must
 * not impose one.
 *
 * show_bar non-zero keeps the SYSTEM screen title bar visible (the real
 * Intuition bar with the depth gadget, so the player can flip to Workbench
 * like any other Amiga program). The bar takes vertical space: the drawable
 * game area is then w x amigagfx_game_height(), which is h minus the bar
 * height Intuition reports for the opened screen - it depends on the font
 * and the mode, so it is never hard-coded. With show_bar 0 the game area is
 * the full w x h, as before.
 *
 * backend AMIGAGFX_BACKEND_WB opens a resizable window on the Workbench screen
 * instead of a screen of our own, and w x h is then the INNER size asked for -
 * clamped to what the Workbench screen can hold. show_bar is meaningless there
 * (the window has its own title bar) and is ignored. It falls back to AGA like
 * the others, and the most likely reason it has to is that there is no public
 * screen at all: a game started from User-Startup runs BEFORE LoadWB, so there
 * is no Workbench to put a window on. The caller is expected to notice the
 * fallback (amigagfx_backend()) and repair its own setting, so a machine that
 * cannot do window mode does not start into it again next time. */
int amigagfx_open(int w, int h, int show_bar, int backend);

/* Could a window be opened on a public screen right now? Non-zero if there is
 * one. Safe before amigagfx_open() and does not disturb anything: it locks the
 * default public screen, notes that it exists and unlocks it again. Used to
 * offer (or refuse) window mode without having to fail an open first. */
int amigagfx_wb_available(void);

/* How many of our 256 colours the display can actually show, and how they were
 * obtained. Meaningful only in window mode - the other backends own their
 * palette and always answer 256/AMIGAGFX_WBCOL_OWN. For the log and for the
 * settings GUI, so a washed-out picture has a visible explanation rather than
 * looking like a palette bug. */
#define AMIGAGFX_WBCOL_OWN    0  /* our own screen: all 256, exact          */
#define AMIGAGFX_WBCOL_DIRECT 1  /* >8bpp Workbench: all 256, exact, no
                                  * negotiation needed - the colour table is
                                  * handed to the graphics card per blit      */
#define AMIGAGFX_WBCOL_PENS   2  /* <=8bpp Workbench: pens negotiated with
                                  * Intuition, the rest approximated          */
int amigagfx_wb_colours(int *granted);

/* Width and height in pixels of the drawable game area. Height is the opened
 * height minus the system title bar when that is visible; in window mode both
 * are the window's inner size and BOTH CAN CHANGE while the game runs (see
 * AMIGAGFX_EV_RESIZE). These are what _screen.width/height must be set to -
 * and they are NOT the pitch, which in window mode stays fixed at the largest
 * size the window could ever take. */
int amigagfx_game_width(void);
int amigagfx_game_height(void);

/* Loading splash: 1 = keep the system pointer visible regardless of the
 * hide-pointer setting, 0 = back to the setting. */
void amigagfx_pointer_suspend(int on);

void amigagfx_close(void);

/* Hide (1) or restore (0) the Intuition pointer over our window. Safe to call
 * before the window exists; it is re-applied when one opens. */
void amigagfx_set_hide_system_pointer(int on);

/* Set the text shown in the screen title bar (the wb_bar option). Safe before
 * the screen exists (stored, used at open) and live afterwards. */
void amigagfx_set_screen_title(const char *title);

/* Which palette INDICES Intuition should draw the screen title bar with:
 * the title text, the bar fill, and the trim line under it.
 *
 * A screen pen is an index, not a colour, so the built-in defaults are only
 * right for the palette they were chosen against (OpenTTD's). A game with its
 * own 256 colours must pick these from that palette or the bar is drawn in two
 * arbitrary ones - which in GTA's palette came out black on black and read as
 * "the bar did not open" rather than "the bar is the wrong colour".
 *
 * Call BEFORE amigagfx_open(); afterwards the screen already has its pens. */
void amigagfx_set_bar_pens(int text, int fill, int trim);

/* Redraw the screen title bar. Needed after anything writes straight into the
 * bitplanes (amigagfx_planes), because the bar lives in those same planes and
 * Intuition will not notice it has been painted over. No-op without a bar. */
void amigagfx_refresh_titlebar(void);

/* The chunky 8bpp framebuffer, in Fast RAM. This becomes _screen.dst_ptr, so
 * OpenTTD's blitter draws straight into it with no conversion. */
unsigned char *amigagfx_chunky(void);

/* Bytes per row of the chunky buffer. Equals the width on every backend that
 * owns its screen. In WINDOW mode it does NOT: the buffer is allocated once at
 * the full Workbench screen size and the window is a sub-rectangle of it, so
 * resizing never reallocates and never fails for want of memory. Always read
 * this rather than assuming it matches the width. */
int amigagfx_pitch(void);

/* rgb points at count*3 bytes. */
void amigagfx_set_palette(const unsigned char *rgb, int first, int count);

/* Hand this file a private copy of the 64-entry EHB palette (64*3 bytes, the
 * same table the caller loads with amigagfx_set_palette). Only the splash uses
 * it, and only on an EHB screen, where an image carrying its own 256-colour
 * palette cannot be shown as-is: there are 64 pens and half of them are not
 * settable. Passed in rather than shared as a symbol so this file keeps its
 * one hard rule - no dependency on anything of OpenTTD's. Safe to call before
 * amigagfx_open(); ignored on AGA and RTG. */
void amigagfx_set_ehb_palette(const unsigned char *rgb64);

/* Push one dirty rectangle to the screen. On AGA and EHB x/width are snapped
 * outwards to the 32-pixel grid the c2p requires; on RTG they are used as
 * given, because that granularity is a c2p property and nothing else needs it.
 * Clipping is handled here on all three backends.
 *
 * EHB additionally needs its chunky input CONTIGUOUS - the 6-plane routine
 * takes no row modulo - so a rectangle narrower than the screen is copied into
 * a small scratch band first. That is handled internally and callers see no
 * difference; a full-width rectangle skips the copy entirely. */
void amigagfx_blit(int x, int y, int w, int h);

/* Milliseconds since program start; OpenTTD's main loop needs a ms clock. */
unsigned long amigagfx_millis(void);

/* Pops one pending event. Returns 0 when the queue is empty. */
int amigagfx_poll(AmigaGfxEvent *ev);

/* Move the system pointer to the ABSOLUTE pixel position x,y in the GAME AREA
 * of our screen (the title-bar offset, if any, is added internally) by
 * writing an IECLASS_NEWPOINTERPOS / IESUBCLASS_PIXEL event to input.device
 * (opened lazily, closed again by amigagfx_close). The screen pointer stays
 * internal to amiga_gfx.c. Returns 1 if the event was sent, 0 if it could not
 * be (input.device unavailable) - in that case it is a harmless no-op.
 * NOTE: Intuition will deliver a normal mouse-move IDCMP event for the warp;
 * the caller must expect and suppress it. */
int amigagfx_warp_pointer(int x, int y);

/* Append a line to the log. amigagfx_open() redirects stderr to Work:ottd.log
 * unbuffered, which is the only way to see anything at all: OpenTTD logs to
 * stderr and the AmigaDOS 3.1 shell can only redirect stdout. */
void amigagfx_log(const char *msg);

/* Non-zero re-enables the chatty per-play diagnostics on the C side (the
 * periodic "blit #N" heartbeat). Off by default for a quiet release log; the
 * C++ driver calls this for "-v amiga:verbose". */
void amigagfx_set_verbose(int verbose);

/* Startup memory probe: appends one line to PROGDIR:amiga_mem.log with the
 * label plus AvailMem(MEMF_FAST), AvailMem(MEMF_FAST|MEMF_LARGEST) and
 * AvailMem(MEMF_CHIP) in KB. Total-free vs largest-block matters: with two
 * Fast regions (8 MB + 32 MB Z3) fragmentation can move total-free readings
 * by megabytes with no real change in usage. First call truncates the log;
 * capped at a fixed number of lines so it can never flood. Safe to call at
 * any time, including before amigagfx_open(). Lives here because AvailMem
 * needs Amiga headers, which OpenTTD C++ must never include. */
void AmigaMemProbe(const char *label);

/* Free Fast RAM in bytes (see amiga_gfx.c). */
unsigned long AmigaFreeFastMem(void);

/* Largest contiguous free Fast block in bytes - what an allocation can use. */
unsigned long AmigaLargestFastMem(void);

/* Show the startup splash image (ASPL file, see build/make-splash.py) on the
 * already-open screen: fade in ~0.5 s, hold 2.5 s, fade out ~0.5 s. The image
 * is converted chunky-to-planar exactly once; the fade animates only the
 * palette registers, so it is cheap even on a 68030. Draws at 1x on screens
 * below 400 lines, 2x nearest-neighbour otherwise. On any problem (missing
 * file, bad magic, does not fit) it logs one line and returns without drawing.
 * Leaves the chunky buffer cleared to index 0 and the palette black - the
 * caller must restore the game palette afterwards.
 *
 * Works on whatever screen is open, which matters more than it sounds: the
 * whole audience for the EHB mode is machines that cannot show an 8-bitplane
 * screen at all, so a splash that assumed one would fail before the game even
 * started. On an EHB screen the image is reduced to the 64 EHB pens through a
 * nearest-colour match against the palette given to
 * amigagfx_set_ehb_palette(), and the fade then scales THAT palette instead of
 * the file's own - the screen mode is never changed to suit the picture. */
void amigagfx_splash(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* AMIGA_GFX_H */
