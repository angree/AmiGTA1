/* Liberty City, on the Amiga, under the arrow keys.
 *
 * This is the whole Phase 4 milestone: the 2.5D map view and nothing else. No
 * cars, no pedestrians, no missions, no interface. You start it, you see the
 * city the way GTA draws it - roofs displaced outward from their bases so the
 * walls facing the middle of the screen are visible - and you drive the camera
 * around it.
 *
 * What this program is really for is joining five things that only meet here:
 * the m68k cross build, the platform layer carried over from the OpenTTD and
 * OpenXcom ports, chunky-to-planar, the baked tile set, and native/gta_render.c.
 * The renderer itself has already been looked at on the host (gtadump view), so
 * the question left for the Amiga is not "does it look right" but "is it fast
 * enough, and does it survive a 68020 without an FPU".
 *
 * Everything it needs is in its OWN drawer, PROGDIR: - GTADATA/ sits beside
 * the executable and tools/bin/deploy.sh puts it there. It writes only to
 * stdout, which the `run` script redirects to a log beside it, because the
 * boot volume is mounted read-only.
 *
 * Controls: arrow keys scroll, shift scrolls faster, - and = (or keypad - and
 * +) zoom out and in, SPACE dumps the current frame to frame_live.raw,
 * ESC quits. F3 shows or hides the Workbench title bar - it is on by default,
 * because this is an Amiga port and the screen's depth gadget is how the
 * machine gets multitasked in and out of the game.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amiga_gfx.h"
#include "amiga_uclock.h"
#include "gta_map.h"
#include "gta_tiles.h"
#include "gta_render.h"
#include "gta_hud.h"
#include "gta_player.h"
#include "gta_traffic.h"
#include "gta_vehphys.h"
#include "gta_peds.h"

/* THE SCREEN, AND WHY IT IS A COMPILE-TIME CHOICE NOW.
 *
 * 320x200 is what an AGA screen gives and what every measurement in
 * the notes was taken at. RTG can do better, and a tester should get the
 * screen they were handed a binary for without having to write a text file
 * into the game drawer - so the three shipped builds differ here and
 * nowhere else.
 *
 *   gta-aga        320x200, AGA          the reference, unchanged
 *   gta-rtg240     320x240, RTG          the same picture, more of the city
 *   gta-rtg480     640x480, RTG          320x240 doubled; see GTA_SCALE2X
 *
 * `backend.txt` beside the binary still overrides the backend at runtime,
 * so a single
 * binary can still be pointed at either screen for an A/B measurement. */
#ifndef GTA_SCREEN_W
#define GTA_SCREEN_W 320
#endif
#ifndef GTA_SCREEN_H
#define GTA_SCREEN_H 200
#endif
#ifndef GTA_DEFAULT_BACKEND
#define GTA_DEFAULT_BACKEND AMIGAGFX_BACKEND_AGA
#endif

/* WHAT THE RENDERER DRAWS, which is not always what the screen shows.
 *
 * With GTA_SCALE2X the screen is twice the rendered picture in each axis and
 * the present step doubles it. Everything upstream - renderer, HUD, frame
 * dumps, every timing in the notes - still works in RENDER_W/RENDER_H, so
 * none of it has to know. */
#ifdef GTA_SCALE2X
#define RENDER_W (GTA_SCREEN_W / 2)
#define RENDER_H (GTA_SCREEN_H / 2)
#else
#define RENDER_W GTA_SCREEN_W
#define RENDER_H GTA_SCREEN_H
#endif

#define SCREEN_W RENDER_W
#define SCREEN_H RENDER_H

/* EVERY PATH THIS PROGRAM OPENS IS RELATIVE TO ITS OWN DRAWER.
 *
 * `PROGDIR:` is the automatic assign AmigaOS makes for the directory the
 * running executable was loaded from - it is set for a Workbench double-click
 * and for a CLI start alike, and it needs no assign from the player.
 *
 * It used to be `Work:`, which is not a place - it is whatever the machine
 * happens to have assigned, usually the boot partition's work drawer. So a
 * player who unpacked the archive to DH1:Games/AmiGTA got "cannot open
 * Work:GTADATA/..." and the v0.0.1 README had to tell them to assign Work:
 * to the game drawer, which is asking the player to work around a bug.
 * Reported from the outside: people were asking what the work directory is.
 *
 * Anything reading or writing a file goes through this, so there is one place
 * to change and nowhere for a nineteenth hard-coded `Work:` to hide. */
#define GTA_DIR    "PROGDIR:"

#define TILES_PATH GTA_DIR "GTADATA/style001.til"
#define MAP_PATH   GTA_DIR "GTADATA/nyc.cmp"

/* Amiga raw key codes. These are the codes the keyboard sends, not ASCII, and
 * amigagfx_poll() passes every one of them through with bit 7 set on release
 * so held keys can be tracked. */
#define KEY_UP     0x4C
#define KEY_DOWN   0x4D
#define KEY_RIGHT  0x4E
#define KEY_LEFT   0x4F
#define KEY_ESC    0x45
#define KEY_SPACE  0x40
#define KEY_MINUS    0x0B   /* main keyboard - and = */
#define KEY_EQUALS   0x0C
#define KEY_NUMMINUS 0x4A   /* keypad - and + */
#define KEY_NUMPLUS  0x5E
#define KEY_LSHIFT 0x60
#define KEY_RSHIFT 0x61
#define KEY_TAB    0x42     /* switch between walking and free camera */
#define KEY_RETURN 0x44     /* enter / leave the nearest car */

/* Function keys. F1..F10 are 0x50..0x59 on the Amiga keyboard. */
#define KEY_F1     0x50
#define KEY_F2     0x51
#define KEY_F3     0x52
#define KEY_F4     0x53
#define KEY_F5     0x54
#define KEY_F6     0x55
#define KEY_F7     0x56
#define KEY_F8     0x57
#define KEY_F9     0x58
#define KEY_F10    0x59

/* Scroll speed in reference-scale pixels per frame. One block is 32 of them. */
#define SCROLL_SLOW 3
#define SCROLL_FAST 12

/* Where the camera starts. Downtown Liberty City: a junction with buildings on
 * three sides, which is the view that shows whether the projection works. */
#define START_BX 64
#define START_BY 64

/* A stretch of the waterfront, for the second benchmark. Half water, half
 * quay - the case the renderer is slowest on. */
#define WATER_BX 96
#define WATER_BY 210

/* How many frames the unattended benchmark draws before the interactive loop
 * takes over. Long enough to average out one slow frame, short enough that a
 * test run is not a coffee break on a throttled 68020. */
#define BENCH_FRAMES 60

/* Frames the on-screen readout averages over. A per-frame number flickers too
 * fast to read, and this machine wobbles by about 10% between frames anyway. */
#define HUD_SAMPLE 10

/* THE SIMULATION RUNS AT A FIXED RATE AND THE RENDER DOES NOT.
 *
 * Until now a tick WAS a frame, so the player walked faster downtown (36 fps)
 * than he did zoomed out (20), and on a slower machine the whole game would
 * simply have run in slow motion. Both are the same bug: game speed tied to
 * how long a frame happens to take.
 *
 * So the loop accumulates real elapsed microseconds and spends them in whole
 * SIM_US ticks, rendering once per pass. A machine that renders faster than
 * SIM_HZ runs some passes with no tick at all; one that renders slower runs
 * several ticks per pass, which is what "frameskip" means here - the game
 * keeps its speed and the picture gets coarser in time rather than the game
 * getting slower. That is the difference between an unplayable 030 and a slow
 * but correct one.
 *
 * 50 Hz, PAL's own rate, and above the frame rate the renderer reaches - so
 * the movement is sampled finer than it is drawn and nothing steps. 25 was
 * tried first and was visibly worse than the frame-tied version it replaced,
 * because the frame rate here is 40-50 and dropping the simulation to 25
 * halved the player's speed on the spot. The speeds in gta_player.c are
 * per-tick and must be rescaled with this. MAX_CATCHUP stops the death
 * spiral: if one pass ever costs more than four ticks' worth of time -
 * a disk access, a debugger stop - the arrears are dropped rather than paid,
 * because paying them means an even longer pass and then more arrears. */
#define SIM_HZ       50
#define SIM_US       (1000000L / SIM_HZ)

/* MAX_CATCHUP WAS 8 AND THAT WAS THE WHOLE OF THE "GAME GOT THREE TIMES
 * SLOWER" REPORT. Measured 2026-08-25 on gta-prof-slow (68020, throttle -900,
 * JIT off), same binary, same scene, only this number changed:
 *
 *      catchup 8   2.5 fps   sim 210216 us/frame (7.92 ticks a frame)
 *      catchup 2   4.2 fps   sim  50083 us/frame (1.98 ticks a frame)
 *
 * The trap is that the simulation is charged per unit of REAL time, so the
 * slower the frame, the MORE simulation each frame has to pay for - which
 * makes the frame slower still. It is a positive feedback, and 8 is far enough
 * up the curve that a machine which merely renders at 12 fps ends up pinned at
 * the cap, with the traffic taking more than half the frame. Below that point
 * it never recovers, and moving to somewhere with nothing on screen does not
 * help, because the cost is the fleet, not the view. That is exactly what the
 * developer reported: "jak tylko wychodze w miejsce gdzie nie jest rysowany
 * nadal jest ekstremalnie wolno".
 *
 * 3 keeps the world running at real speed on anything that renders faster than
 * about 17 fps - every machine this port is aimed at - and on a slower one the
 * world runs in slow motion instead of the picture collapsing. That is also
 * what the ORIGINAL does: its logic is tied to the frame, so a slow DOS machine
 * played a slow game. Costs nothing where it is not needed: on the 020 test
 * machine catchup 2 and catchup 8 both measured 29.3 fps, back to back.
 *
 * `catchup <n>` in opts.txt sets it for a measurement without a rebuild. */
#define MAX_CATCHUP  3

/* The frame cap. Without one the loop renders as fast as the machine allows,
 * which on the test machine means burning the whole CPU to produce frames
 * nobody asked for, and on any machine means the tearing is unpredictable.
 *
 * 60 rather than 50 so a PAL machine is limited by its own display and not by
 * this. It is a busy-wait on the microsecond clock: crude, but it is exact and
 * it needs nothing from the platform layer. WaitTOF() would hand the CPU back
 * to the system between frames and is the better answer once there is anything
 * else that wants it. */
#define FRAME_CAP_HZ 60
#define FRAME_CAP_US (1000000L / FRAME_CAP_HZ)

static void log_line(const char *s)
{
    printf("%s\n", s);
    fflush(stdout);
}

/* --- render modes ---------------------------------------------------------
 *
 * Two independent knobs, both on function keys, because they cost different
 * things and a weak machine may want one, the other, or both:
 *
 *   PROJECTION (F5)   2.5D or flat. Flat is a projection change, not a second
 *                     renderer: every grid level at the same pitch. It removes
 *                     every wall from the frame, makes every lid a
 *                     constant-size opaque copy, and lets the layer loop start
 *                     at the topmost opaque lid instead of at zero. Measured
 *                     on the host at block (90,70): 675 column-visits down to
 *                     306.
 *
 *   RESOLUTION (F1/F2) full, or half in both axes and blown back up.
 *
 * HALF RESOLUTION KEEPS THE FIELD OF VIEW, which is why the zoom handed to the
 * renderer is the displayed zoom divided by the scale. That has a consequence
 * worth stating plainly, because it is the opposite of what one expects:
 * **half resolution does not reduce the traversal at all**. The same blocks
 * are on screen, each drawn with a quarter of the pixels. Measured, flat at
 * (90,70): 306 column-visits at full resolution and 306 at half. What halves
 * is the blitting; the walk is untouched.
 *
 * ONLY 2x2, not 2x1 or 1x2. Those were asked for and are not here, and the
 * reason is structural rather than laziness: this renderer's tiles are SQUARE
 * everywhere - one `step` per grid level used for both axes, and a lid cache
 * whose entries are w by w. Halving one axis alone means either a stretched
 * world (a circle becomes an ellipse) or splitting step into step_x and step_y
 * through the whole of draw_block AND reworking the cache to hold non-square
 * entries. The second is a real change and the measurement above says the
 * prize is small: with the field of view preserved, an anisotropic mode saves
 * blits on one axis and nothing else. the notes carries it as an item rather
 * than a decision. */
/* File scope, not a local: the fleet is about 700 bytes and the Amiga's stack
 * is a fixed allocation made in the startup code, not something that grows.
 * gta_view is already the big local in here and there is no reason to find out
 * the hard way where the limit is. */
/* Where the renderer draws and how wide a row is. File-scope because the
 * scripted tour and the scripted walk use them too, and because open_display()
 * below is the single place they are ever assigned - see the note there. */
static unsigned char *g_chunky;
static int g_pitch;

/* Set whenever the planar screen may hold something outside the current
 * picture - a new screen, or a change of render width. The next frame then
 * converts the whole 320 columns instead of just the picture. Declared up here
 * rather than beside the render modes because open_display() sets it. */
static int bars_dirty = 1;

/* Microseconds spent in chunky-to-planar, accumulated by present_frame(). */
static unsigned long bench_blit_us;

/* THE FRAME PROFILE, printed every PROF_FRAMES frames of the INTERACTIVE loop.
 *
 * The benchmarks at startup measure the renderer with no traffic and no
 * player, which is exactly the part of the frame that was never in doubt. The
 * report that started this - "15-18 fps on the 040/40, now 4-6" - is about the
 * interactive loop, where three costs are added: the simulation (which runs up
 * to MAX_CATCHUP times per frame, so it grows as the frame rate falls), the
 * reservation overlay, and the cars themselves.
 *
 * Four extra clock reads a frame. amiga_uclock_us() is one library call plus a
 * 64-bit divide, which is about 20 us on a 68020 - a tenth of a percent of a
 * frame at these speeds, and the numbers below are worth far more than that. */
#define PROF_FRAMES 100
static unsigned long prof_sim_us, prof_ren_us, prof_pre_us, prof_c2p_us;
static long prof_ticks, prof_frames;
static unsigned long prof_t0;

/* A/B SWITCHES READ FROM opts.txt beside the binary, one `word value` per
 * line.
 *
 * Same idea as backend.txt: the emulator runs unattended, so a comparison
 * has to be selectable without a rebuild, and two binaries built at different
 * moments are not comparable.
 *
 *   overlay 0|1   the reservation overlay          (default 1, as shipped)
 *   fleet   <n>   cars in the fleet, 0 for none    (default: the traffic's own)
 *   traffic 0|1   run the simulation at all        (default 1)
 *   catchup <n>   simulation ticks a frame may pay (default MAX_CATCHUP)
 *   benchframes <n>  frames per startup benchmark  (default BENCH_FRAMES)
 */
/* The reservation overlay is DEACTIVATED by default since 2026-08-26 - the
 * developer's call once traffic was accepted ("zdezaktywuj je, nie wywalaj
 * bo moze jeszcze sie przydadza"). The code stays; `overlay 1` in
 * opts.txt brings it back for the next traffic investigation. */
static int opt_overlay = 0;
static int opt_traffic = 1;
static int opt_fleet   = -1;
static int opt_catchup = MAX_CATCHUP;
/* How many frames each startup benchmark averages over. 60 is the number
 * every recorded figure in the notes was taken with, so it is the default
 * and a run that changes it is not comparable with them. It exists because a
 * heavily throttled machine spends ten minutes in the benchmarks before it
 * ever reaches the interactive loop, which is where the traffic questions
 * live: `benchframes 5` turns a ten-minute round trip into a one-minute one. */
static int opt_benchf  = BENCH_FRAMES;
/* Interactive picture width for an unattended A/B: 256 (the shipped start)
 * or 320. 0 = leave the default. */
static int opt_width   = 0;
/* Starting camera height in quarter levels (32 = C8 shipped, 64 = C16, the
 * pre-24.08 look) - measurement only; F7/F8 still move it live. 0 = default. */
static int opt_camh    = 0;

static gta_traffic traffic;
static gta_peds peds;

/* THE SELF-DRIVING TEST - autodrive.txt beside the binary, read at startup.
 * Host input synthesis is banned, so this is how an agent verifies that
 * entering a car and driving it works at all: the file is a queue of
 * orders the interactive tick consumes in place of the keyboard.
 *     wait <ticks>                    do nothing (let the fleet spawn)
 *     enter                           press RETURN once
 *     run <ticks> <thr> <brk> <steer> <hb>
 *     dump                            write frame_live.raw
 * Missing file means no script - the keyboard is live as always. */
#define AUTODRIVE_MAX 64
static struct { int op, t, thr, brk, st, hb; } adq[AUTODRIVE_MAX];
static int adq_n, adq_i, adq_left;
/* The fleet's odometer at the last five-second report; see the report itself. */
static long traffic_moved_last;
static gta_nav nav;

/* --- the screen, and the Workbench title bar ------------------------------
 *
 * WHY THE BAR IS ON BY DEFAULT
 * The developer runs this the way every other Amiga port of this series is
 * run: on its own public screen, with Intuition's own title bar at the top, so
 * the screen's depth gadget is there and the machine can be multitasked in and
 * out of the game. That is not decoration - it is how the Amiga is used.
 *
 * THE GAME AREA DOES NOT SHRINK. The platform layer opens the screen BarHeight
 * lines TALLER and puts the full 320x200 below the bar, so nothing of the game
 * is lost and the renderer never learns the bar exists. That behaviour is
 * inherited from the OpenXcom port, which needed it for the same reason: a
 * fixed-size game area that cannot reflow. The bar is Intuition's own, never a
 * drawn imitation of one.
 *
 * TOGGLING IT MEANS CLOSING AND REOPENING THE SCREEN, which is why this is a
 * function rather than two lines at start-up: the chunky buffer is freed and
 * reallocated, so `g_chunky`, `g_pitch` and the renderer's target ALL have to
 * be rebound afterwards. Leaving one of them stale is exactly the HALT1 this
 * file already carries a note about - a silent renderer and then a fatal HUD
 * write to address zero. Everything the screen owns is re-established here, in
 * one place, so there is nowhere for a fourth thing to be forgotten. */
/* The version goes on the screen's title bar, where a tester can read it
 * without a log. Bump it here and nowhere else. */
#define GTA_VERSION "v0.0.2"
#define GAME_TITLE  "AmiGTA 68K " GTA_VERSION

#ifdef GTA_SCALE2X
/* The renderer's own buffer. The screen's chunky bitmap is twice this in each
 * axis and is only ever written by scale2x_present() below. */
static unsigned char g_render_buf[RENDER_W * RENDER_H];

/* Double `h` rows of `w` pixels from src into dst, which is `dpitch` wide.
 *
 * Two writes per source pixel across, then the row is copied whole to make the
 * second of the pair - a memcpy of an already-built row is far cheaper than
 * building it twice, and it is what makes this affordable on a 68020. */
static void scale2x_rows(const unsigned char *src, int spitch,
                         unsigned char *dst, int dpitch, int w, int h)
{
    int y;

    for (y = 0; y < h; y++) {
        const unsigned char *s = src + (long)y * spitch;
        unsigned char *d = dst + (long)(y * 2) * dpitch;
        int x;

        for (x = 0; x < w; x++) {
            unsigned char c = s[x];
            d[x * 2]     = c;
            d[x * 2 + 1] = c;
        }
        memcpy(d + dpitch, d, (size_t)(w * 2));
    }
}
#endif

static int g_show_bar = 1;
static int g_backend_used = GTA_DEFAULT_BACKEND;
static const unsigned char *g_palette;

/* Which of GTA's 256 colours Intuition draws the title bar with.
 *
 * A screen pen is an INDEX, and the platform layer's defaults were chosen
 * against OpenTTD's palette, where 15 is white and 17 a dark blue-grey. GTA's
 * palette is a different 256 colours, so those two indices land on whatever
 * the artists happened to put there - and the first run of the bar drew it
 * black on black. It was there and it was invisible, which is a worse failure
 * than a missing one because it looks like the feature did not work.
 *
 * So they are picked out of the palette that is actually loaded: the brightest
 * entry for the text, the darkest for the trim line under the bar, and for the
 * fill the entry nearest a dark neutral grey - nearest by squared distance,
 * the same measure the tile downscaler uses. Each city has its own palette and
 * this runs on whichever one was baked. */
static void choose_bar_pens(const unsigned char *pal)
{
    int i, text = 15, fill = 17, trim = 0;
    long best_bright = -1, best_dark = -1, best_grey = -1;

    for (i = 0; i < 256; i++) {
        int r = pal[i * 3 + 0], g = pal[i * 3 + 1], b = pal[i * 3 + 2];
        long sum = (long)r + g + b;
        long dr = r - 72, dg = g - 76, db = b - 88;
        long dist = dr * dr + dg * dg + db * db;

        if (best_bright < 0 || sum > best_bright) { best_bright = sum; text = i; }
        if (best_dark   < 0 || sum < best_dark)   { best_dark   = sum; trim = i; }
        if (best_grey   < 0 || dist < best_grey)  { best_grey   = dist; fill = i; }
    }

    printf("gta: title bar pens - text %d (%d,%d,%d), fill %d (%d,%d,%d), "
           "trim %d (%d,%d,%d)\n",
           text, pal[text * 3], pal[text * 3 + 1], pal[text * 3 + 2],
           fill, pal[fill * 3], pal[fill * 3 + 1], pal[fill * 3 + 2],
           trim, pal[trim * 3], pal[trim * 3 + 1], pal[trim * 3 + 2]);
    fflush(stdout);
    amigagfx_set_bar_pens(text, fill, trim);
}

static int open_display(gta_view *v, int show_bar)
{
    unsigned char *chunky;

    if (amigagfx_open(GTA_SCREEN_W, GTA_SCREEN_H, show_bar,
                      g_backend_used) != 0)
        return 0;

    amigagfx_set_screen_title(GAME_TITLE);
    if (g_palette)
        amigagfx_set_palette(g_palette, 0, 256);
    /* The game never uses the mouse, and the Intuition pointer parks itself in
     * the top-left corner - exactly where the readout goes. */
    amigagfx_set_hide_system_pointer(1);

    chunky = amigagfx_chunky();
    if (!chunky)
        return 0;
    g_chunky = chunky;
    g_pitch  = amigagfx_pitch();
#ifdef GTA_SCALE2X
    /* The renderer never touches the screen directly in this build. */
    g_chunky = g_render_buf;
    g_pitch  = RENDER_W;
#endif
    if (v)
        gta_render_target(v, g_chunky, SCREEN_W, SCREEN_H, g_pitch);
    /* A brand new planar screen holds nothing at all, so the bars have to be
     * converted once before the narrow blit starts skipping them. */
    bars_dirty = 1;
    return 1;
}

/* Returns 0 only if the screen could not be brought back at all, in which case
 * the caller must stop: there is no display left to draw on. A refused TOGGLE
 * is not fatal - the old setting is simply restored and said so. */
static int toggle_bar(gta_view *v)
{
    int want = !g_show_bar;

    amigagfx_close();
    if (open_display(v, want)) {
        g_show_bar = want;
        printf("gta: title bar %s\n", g_show_bar ? "ON" : "OFF");
        fflush(stdout);
        return 1;
    }

    amigagfx_close();
    if (open_display(v, g_show_bar)) {
        printf("gta: title bar %s REFUSED - kept %s\n",
               want ? "ON" : "OFF", g_show_bar ? "ON" : "OFF");
        fflush(stdout);
        return 1;
    }
    log_line("gta: the screen could not be reopened - stopping");
    return 0;
}

static unsigned char low_buffer[(SCREEN_W / 2) * (SCREEN_H / 2)];

#define LOW_W (SCREEN_W / 2)
#define LOW_H (SCREEN_H / 2)

static int mode_scale = 1;      /* 1 = full 320x200, 2 = 160x100 blown up */
static int mode_flat;           /* 0 = 2.5D, 1 = flat top-down */

/* WHICH OF THE THREE PROJECTIONS F5 IS SHOWING.
 *
 * `mode_flat` is what the renderer actually reads and stays a plain flag; this
 * is the user-facing cycle on top of it, because there are three things worth
 * offering and only two of them differ in the renderer:
 *
 *   PROJ_FULL   2.5D at the shipped camera height - the full perspective
 *   PROJ_LIGHT  the same renderer at GTA_CAM_H_LIGHT - buildings still have
 *               sides, about 16% of the frame back
 *   PROJ_FLAT   flat top-down, no walls at all
 *
 * The middle one exists because it was asked for: "the faster mode we had,
 * that gave a feeling of light 3D without the heavy slowdown". That mode was
 * never a different rasteriser - it was this renderer before the camera came
 * down from sixteen levels to eight. So it is a camera preset, not a second
 * code path, and saying so is the honest version.
 *
 * The preset is applied when the mode is ENTERED, not held every frame, so
 * F7/F8 still work afterwards and the HUD's C reading stays the truth. */
#define PROJ_FULL  0
#define PROJ_LIGHT 1
#define PROJ_FLAT  2
#define PROJ_COUNT 3
static int mode_proj = PROJ_FULL;
static int zoom_display = GTA_TILE_DIM;   /* what the player asked for */
static int frame_cap = 1;
static int game_speed = 100;   /* percent of real time, F9/F10, 10..100 */

/* Point the renderer at whatever the current mode wants, and give it the zoom
 * that makes the field of view come out right. Cheap enough to call every
 * frame, which means there is no "mode changed" flag to forget to set. */
/* --- narrower renders, with black bars ------------------------------------
 *
 * 320x200 is what the Amiga shows and what a CRT stretches back to 4:3 with
 * non-square pixels. A narrow mode renders fewer columns and leaves the rest
 * of the row black, which takes work off the renderer: the traversal and the
 * blits both scale with the width.
 *
 * WHETHER IT ALSO TAKES WORK OFF THE C2P DEPENDS ENTIRELY ON THE 32-PIXEL
 * GRID.
 *
 * THERE USED TO BE A THIRD MODE, 266 WIDE, AND IT IS GONE (2026-08-24).
 * Two reasons, either of which is enough:
 *
 *   - it never saved any c2p. Kalms' c2p converts 32-pixel columns, so
 *     amigagfx_blit() snaps a rectangle outwards to that grid; 266 centred at
 *     x=27 snaps straight back out to 0..320 and every bar pixel is converted
 *     anyway. Measured: 6163 us against full width's 6160. All it bought was
 *     6% of the renderer, for two black bars.
 *
 *   - AND ITS WHOLE JUSTIFICATION IS NOW VOID. 266 was "4:3 with SQUARE
 *     pixels": at 320x200 square, the picture is 8:5, so 4:3 wants 266
 *     columns. But the renderer now applies the original's 5/6 vertical
 *     squash (gta_view.stepy), which exists precisely because the pixels are
 *     NOT square - it assumes 320x200 displayed as 4:3, i.e. pixels 6:5 tall.
 *     Correcting the aspect twice, once by narrowing and once by squashing, is
 *     simply wrong. With the squash in place, full width IS the 4:3 field of
 *     view.
 *
 * So F4 is back to the two modes it was asked for: full, and the c2p-friendly
 * one.
 *
 * 256 at x=32 is the widest picture that is BOTH centred and on the grid:
 * (320-w)/2 is a multiple of 32 only for 320, 256, 192, 128. The blit is then
 * 32..288 with nothing snapped outwards, so a fifth of the c2p goes away as
 * well as a fifth of the renderer. It is 5:4 rather than 4:3 - ten pixels of
 * shape traded for 64 columns of chunky-to-planar.
 *
 * Only the picture is blitted, so the bars are converted ONCE - on the frame
 * after a mode change, flagged by bars_dirty. Without that the planar screen
 * would keep whatever the previous, wider mode had left standing in them.
 * They are still cleared in the chunky buffer every frame, because the
 * renderer's own clear covers only its target rectangle and a zoom or a mode
 * change can leave anything behind. */
#define FAST_W   256
#define FAST_X   ((SCREEN_W - FAST_W) / 2)        /* 32 - centred AND on it   */

static const struct {
    int w, x;
    const char *name;
} view_modes[] = {
    { SCREEN_W, 0,        "full 320"                 },
    { FAST_W,   FAST_X,   "5:4 256 (c2p-aligned)"    }
};

/* Named indices and a count taken from the table itself. Anything that picks a
 * mode uses these; nothing counts entries by hand. */
#define VIEW_MODES ((int)(sizeof view_modes / sizeof view_modes[0]))
#define VIEW_FULL  0
#define VIEW_FAST  (VIEW_MODES - 1)

static int mode_narrow;                  /* index into view_modes */
static int applied_mode = -1;

/* BOUNDS-CHECKED, because an out-of-range mode index cost an evening: it does
 * not crash, it hands the renderer a nonsense width and the frame loop never
 * finishes. Clamping turns that into a wrong-looking picture, which is a bug
 * anyone can see in a second. */
static int view_mode(void)
{
    return (mode_narrow >= 0 && mode_narrow < VIEW_MODES) ? mode_narrow : 0;
}
static int render_w(void) { return view_modes[view_mode()].w; }
static int render_x(void) { return view_modes[view_mode()].x; }

/* Where the picture actually lands in the chunky buffer, which at half
 * resolution is NOT render_x(): the low buffer is expanded by whole factors,
 * so an odd offset or width is rounded down twice over. Deriving the bars, the
 * readout and the blit from these instead of from render_x() is what makes the
 * narrow modes come out right at half resolution - the first version did not,
 * and left a column of the previous frame standing at the right-hand edge. */
static int present_x(void) { return (render_x() / mode_scale) * mode_scale; }
static int present_w(void) { return (render_w() / mode_scale) * mode_scale; }

static void mode_apply(gta_view *v)
{
    int w = render_w() / mode_scale;
    int h = SCREEN_H / mode_scale;

    if (applied_mode != mode_narrow) {
        applied_mode = mode_narrow;
        bars_dirty = 1;
    }

    if (mode_scale == 2)
        gta_render_target(v, low_buffer + render_x() / mode_scale,
                          w, h, LOW_W);
    else
        gta_render_target(v, g_chunky + render_x(),
                          w, h, g_pitch);
    v->flat_2d = mode_flat;
    gta_render_set_zoom(v, zoom_display / mode_scale);
}

/* --- the on-screen readout ------------------------------------------------
 *
 * Drawn after EVERY frame this program produces - the benchmarks, the scripted
 * tour and the interactive loop alike. The first version only drew it in the
 * interactive loop, which meant it did not appear until the benchmark and the
 * 45-second tour had finished, and to anyone watching that is simply a missing
 * feature.
 *
 * It costs about 600 pixel writes a frame against the renderer's ~300 000, so
 * roughly 0.2% - two orders of magnitude below the 3 fps run-to-run spread of
 * the benchmark it sits next to. Leaving it on during the benchmarks is what
 * makes the number on screen and the number in the log the same measurement. */
static int hud_frames;
static long hud_fps10;
static unsigned long hud_t0;

static void hud_draw(const gta_view *v, unsigned char *chunky, int pitch)
{
    unsigned long now = amiga_uclock_us();
    char line[24];
    char *p;

    if (++hud_frames >= HUD_SAMPLE) {
        unsigned long us = now - hud_t0;
        /* fps * 10 as an integer: no float ever reaches the ROM here. */
        hud_fps10 = us ? (long)((HUD_SAMPLE * 10000000UL) / us) : 0;
        hud_t0 = now;
        hud_frames = 0;
    }

    p = gta_hud_tenths(line, hud_fps10);
    *p++ = ' '; *p++ = 'F'; *p++ = 'P'; *p++ = 'S'; *p = 0;
    /* Inside the picture, not inside the bar. The readout used to be at x=2
     * absolute, which in a narrow mode is 25 pixels out in the black - the
     * text was still on the screen and no longer on the game. */
    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H, present_x() + 2, 2, line);

    /* The DISPLAYED zoom, not v->zoom_px - at half resolution the renderer is
     * told half of it, and a readout that jumped from 32 to 16 when the
     * resolution key was pressed would read as the camera having moved. */
    p = gta_hud_int(line, zoom_display);
    *p++ = 'P'; *p++ = 'X'; *p++ = ' ';
    /* One glyph for the projection: 3 full, L light, 2 flat. */
    *p++ = (mode_proj == PROJ_FLAT) ? '2'
         : (mode_proj == PROJ_LIGHT) ? 'L' : '3';
    *p++ = 'D';
    if (mode_scale == 2) { *p++ = ' '; *p++ = 'H'; }   /* Half resolution */
    /* AND THE CAMERA HEIGHT, so a screenshot carries the setting it was taken
     * at. F7/F8 move it and the whole point is comparing shots against the DOS
     * original - a picture whose projection cannot be identified afterwards is
     * not evidence. */
    *p++ = ' '; *p++ = 'C';
    p = gta_hud_int(p, v->cam_h);
    *p = 0;
    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H, present_x() + 2, 10, line);
}

/* The player's own line, under the frame rate. Only the walking mode draws it.
 *
 * It is the ground type that matters here rather than the coordinates: the
 * collision rule IS the ground type (gta_player.h), so a player who will not
 * walk somewhere is explained on screen instead of in a log nobody is reading
 * while they hold the key down. The 3x5 font has no lower case, so the types
 * are one letter each: Road, Pavement, Field, Water, Building, Nothing. */
static void hud_player(const gta_player *p, unsigned char *chunky, int pitch)
{
    static const char ground_letter[8] = { 'N', 'W', 'R', 'P', 'F', 'B', '-', '-' };
    char line[24];
    char *q;

    q = gta_hud_int(line, p->layer);
    *q++ = ' ';
    *q++ = ground_letter[p->ground & 7];
    *q++ = ' ';
    q = gta_hud_int(q, p->angle);
    if (p->blocked_x || p->blocked_y) { *q++ = ' '; *q++ = 'X'; }
    *q = 0;
    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H, present_x() + 2, 18, line);
}

/* Expand if the mode needs it, draw the readout, and put the frame on screen.
 *
 * THE HUD IS DRAWN AFTER THE EXPANSION, into the full-resolution buffer. That
 * is the whole answer to "do fonts and text drop to the low resolution too":
 * they do not, because text never goes through the reduced buffer at all. A
 * 3x5 font blown up 2x is unreadable; drawn at full size on top of a chunky
 * world it costs the same few hundred pixels it always did. */
static void present_frame(gta_view *v, const gta_player *pl, int with_player)
{
    if (mode_scale == 2)
        gta_render_expand(low_buffer, LOW_W, LOW_H, LOW_W,
                          g_chunky, g_pitch, 2, 2);
    if (mode_narrow) {
        /* The bars. Cleared every frame: the renderer only clears its own
         * rectangle, so a zoom or a mode change can leave the previous frame's
         * edges lying in them. */
        int y, rx = present_x(), rw = present_w();
        for (y = 0; y < SCREEN_H; y++) {
            unsigned char *row = g_chunky + (long)y * g_pitch;
            memset(row, 0, rx);
            memset(row + rx + rw, 0, SCREEN_W - rx - rw);
        }
    }
    hud_draw(v, g_chunky, g_pitch);
    if (with_player)
        hud_player(pl, g_chunky, g_pitch);

    /* Convert the picture only. The bars are converted on the frame after a
     * mode change and never again - they are black and they stay black, and
     * c2p is the one part of the frame a narrower picture would otherwise not
     * make any cheaper. amigagfx_blit() snaps to the 32-pixel grid itself, so
     * a mode that is not on it simply gets its bars converted too. */
    {
        unsigned long tb = amiga_uclock_us();

#ifdef GTA_SCALE2X
        /* Double the whole picture into the screen, then blit all of it. The
         * narrow-blit optimisation does not apply here: the doubling has
         * already touched every byte, so there is nothing left to save. */
        scale2x_rows(g_render_buf, RENDER_W, amigagfx_chunky(),
                     amigagfx_pitch(), RENDER_W, RENDER_H);
        amigagfx_blit(0, 0, RENDER_W * 2, RENDER_H * 2);
        bars_dirty = 0;
#else
        if (bars_dirty) {
            amigagfx_blit(0, 0, SCREEN_W, SCREEN_H);
            bars_dirty = 0;
        } else {
            amigagfx_blit(present_x(), 0, present_w(), SCREEN_H);
        }
#endif
        /* Accumulated for the benchmark, always, because the c2p cost has to
         * be measured WHERE IT HAPPENS. A separate back-to-back blit loop was
         * tried first and reported more than twice the in-frame figure - the
         * same call, in a different context, is not the same measurement. */
        bench_blit_us += amiga_uclock_us() - tb;
    }
}


/* Write the chunky buffer and its palette to frame.raw so the host can
 * look at exactly what the 68020 drew.
 *
 * This exists because PrintWindow screenshots of the WinUAE window come back
 * black while the game is blitting - the emulated display is a DirectDraw
 * surface, not something in the window's GDI device context. Fighting that
 * would prove less anyway: a dump straight out of the framebuffer separates
 * "the renderer is wrong" from "the screenshot is wrong", and it is the same
 * bytes chunky-to-planar is about to consume.
 *
 * Format is deliberately trivial - 768 bytes of RGB palette then w*h indices,
 * no header - because tools/bin/raw2png.py is the only thing that reads it. */
static void dump_frame(const char *path, const unsigned char *chunky,
                       int pitch, int w, int h, const unsigned char *palette)
{
    FILE *f = fopen(path, "wb");
    int y;

    if (!f) {
        printf("gta: cannot write %s\n", path);
        fflush(stdout);
        return;
    }
    fwrite(palette, 1, 768, f);
    for (y = 0; y < h; y++)
        fwrite(chunky + (long)y * pitch, 1, (size_t)w, f);
    fclose(f);
    printf("gta: wrote %s (%dx%d)\n", path, w, h);
    fflush(stdout);
}

/* Frames per second to two decimals, without ever dividing floats - there are
 * no floats in this program and there must not be (see CLAUDE.md on
 * mathieeesingbas). us is microseconds for `frames` frames. */
static void log_fps(const char *label, int frames, unsigned long us)
{
    unsigned long ms = us / 1000UL;
    unsigned long f = (unsigned long)frames;
    unsigned long fps100;

    if (frames <= 0 || ms == 0UL) {
        printf("%s: %d frames in no measurable time\n", label, frames);
        fflush(stdout);
        return;
    }

    /* fps*100 is frames * 100000 / ms, and it is the NUMERATOR that overflows:
     * `unsigned long` is 32 bits on m68k-amigaos, so 43000 frames is already
     * too many. The first version of this used microseconds and printed
     * "0.00 fps" next to a perfectly correct 34442 us/frame - a reminder that a
     * diagnostic can lie as loudly as the thing it measures. Scale both sides
     * down instead of reaching for a 64-bit divide the 68020 does not have. */
    while (f > 40000UL) {
        f /= 10UL;
        ms /= 10UL;
        if (ms == 0UL) ms = 1UL;
    }
    fps100 = (f * 100000UL) / ms;

    printf("%s: %d frames in %lu us = %lu.%02lu fps (%lu us/frame)\n",
           label, frames, us, fps100 / 100UL, fps100 % 100UL,
           us / (unsigned long)frames);
    fflush(stdout);
}

/* Read a signed decimal from *p, advancing it. Returns 0 if there was no
 * number left on the line.
 *
 * Hand-rolled rather than sscanf because this libc's printf family has already
 * been caught lying once - sprintf produces nonsense here (CLAUDE.md) - and a
 * parser this small is not worth trusting a suspect library for. */
static int scan_int(const char **p, int *out)
{
    const char *s = *p;
    int sign = 1, got = 0, val = 0;

    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        s++;
        got = 1;
    }
    if (!got) return 0;
    *p = s;
    *out = val * sign;
    return 1;
}

/* Replay a scripted camera path from autoinput.txt beside the binary.
 *
 * Driving the game from INSIDE the guest is this project's rule, not a
 * convenience: synthesising mouse or keyboard events on the host is banned
 * because WinUAE drops the input trap silently and the events then land in
 * whatever the user has on screen - it once posted a half-written forum
 * message from their browser. So an unattended test of "does the city scroll,
 * and does it survive the edges of the map" has to come from a file the Amiga
 * reads itself.
 *
 * One movement per line: dx dy frames. Blank lines and lines starting with
 * ';' or '#' are ignored. Missing file means no script, which is not an error.
 * Returns the number of frames drawn. */
static int autoinput_run(gta_view *v, int w, int h,
                         unsigned char *chunky, int pitch,
                         const unsigned char *palette)
{
    FILE *f = fopen(GTA_DIR "autoinput.txt", "r");
    char line[128];
    int total = 0;
    int leg = 0;

    if (!f)
        return 0;

    log_line("gta: autoinput script found");
    while (fgets(line, (int)sizeof line, f)) {
        const char *p = line;
        int dx, dy, n, i;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == ';' || *p == '#' || *p == '\n' || *p == '\r' || *p == 0)
            continue;
        if (!scan_int(&p, &dx) || !scan_int(&p, &dy) || !scan_int(&p, &n))
            continue;
        if (n < 0) n = 0;
        if (n > 2000) n = 2000;

        printf("gta: auto %d,%d for %d frames\n", dx, dy, n);
        fflush(stdout);
        for (i = 0; i < n; i++) {
            gta_render_move(v, dx, dy);
            mode_apply(v);
            gta_render_frame(v);
            present_frame(v, NULL, 0);
            total++;
        }

        /* One frame per line of the script, so a tour that crosses the city
         * leaves a dozen views on the host to look through. Hunting a
         * rendering artefact by re-rendering one spot over and over finds only
         * the artefacts that spot happens to have. */
        if (leg < 99) {
            char path[64];
            snprintf(path, sizeof path, GTA_DIR "tour%02d.raw", leg);
            dump_frame(path, chunky, pitch, w, h, palette);
            leg++;
        }
    }
    fclose(f);
    return total;
}

/* Replay a scripted WALK from autowalk.txt beside the binary.
 *
 * Deliberately the same file format as the host harness (gtadump walk), so a
 * script that reproduces a problem on the PC can be dropped straight into
 * the game drawer and run on the 68020 without editing. One line per leg:
 *
 *     start <bx> <by>
 *     turn forward walk ticks
 *
 * turn is -1/0/+1, forward -1/0/+1, walk 0 or 1 - and 0 RUNS, because that is
 * the original's default and the game's. Missing file means no script,
 * which is not an error. Returns the number of frames drawn. */
static int autowalk_run(gta_view *v, gta_player *p, const gta_map *m,
                        int w, int h, unsigned char *chunky, int pitch,
                        const unsigned char *palette)
{
    FILE *f = fopen(GTA_DIR "autowalk.txt", "r");
    char line[128];
    int total = 0, leg = 0;

    if (!f)
        return 0;

    log_line("gta: autowalk script found");
    while (fgets(line, (int)sizeof line, f)) {
        const char *q = line;
        int turn, fwd, walk, n, i;

        while (*q == ' ' || *q == '\t') q++;
        if (*q == ';' || *q == '#' || *q == '\n' || *q == '\r' || *q == 0)
            continue;

        if (q[0] == 's' && q[1] == 't' && q[2] == 'a' && q[3] == 'r' &&
            q[4] == 't') {
            int sx, sy;
            q += 5;
            if (scan_int(&q, &sx) && scan_int(&q, &sy)) {
                if (!gta_player_init(p, m, v->tiles, sx, sy)) {
                    printf("gta: autowalk start (%d,%d) has no walkable "
                           "layer\n", sx, sy);
                    fflush(stdout);
                }
                printf("gta: autowalk start (%d,%d) layer %d ground %d\n",
                       sx, sy, p->layer, p->ground);
                fflush(stdout);
            }
            continue;
        }

        if (!scan_int(&q, &turn) || !scan_int(&q, &fwd) ||
            !scan_int(&q, &walk) || !scan_int(&q, &n))
            continue;
        if (n < 0) n = 0;
        if (n > 2000) n = 2000;

        for (i = 0; i < n; i++) {
            gta_player_update(p, m, turn, fwd, walk);
            gta_traffic_tick(&traffic, m, v->cam_x, v->cam_y);
            v->cam_x = p->x;
            v->cam_y = p->y;
            gta_render_add_sprite(v, p->x, p->y, p->layer, gta_player_grid(p),
                                  gta_player_sprite(p),
                                  gta_player_draw_angle(p));
            /* Cars here too, not only in the interactive loop. This is the
             * SCRIPTED path - the one that runs unattended and leaves the
             * walkNN.raw dumps behind - so leaving traffic out of it means the
             * evidence a later session looks at has no cars in it while the
             * game does. That is exactly how a regression hides. */
            gta_traffic_draw(&traffic, v);
            mode_apply(v);
            gta_render_frame(v);
            present_frame(v, p, 1);
            total++;
        }

        printf("gta: walk %2d,%2d,%d x%3d -> block (%ld,%ld) layer %d "
               "angle %d ground %d%s\n",
               turn, fwd, walk, n, p->x >> 21, p->y >> 21, p->layer,
               p->angle, p->ground,
               (p->blocked_x || p->blocked_y) ? " BLOCKED" : "");
        fflush(stdout);

        if (leg < 99) {
            char path[64];
            snprintf(path, sizeof path, GTA_DIR "walk%02d.raw", leg);
            dump_frame(path, chunky, pitch, w, h, palette);
            leg++;
        }
    }
    fclose(f);
    return total;
}

/* THE THREE BIG ONES ARE AT FILE SCOPE, NOT ON THE STACK, AND THAT IS NOT
 * TIDINESS - IT IS A BUG THAT WAS PAID FOR.
 *
 * The note above the fleet already said it: "the Amiga's stack is a fixed
 * allocation made in the startup code, not something that grows. gta_view is
 * already the big local in here and there is no reason to find out the hard
 * way where the limit is." On 2026-08-24 we found out the hard way.
 *
 * gta_view grew by about 4.4 KB in one afternoon - GTA_RECIP_MAX went from 384
 * to 1024 (+2560 bytes) so a wall quad at a low camera could index its own
 * reciprocal, and lc_vrow added 7 x 256 for the lid row map - on top of the
 * 8.4 KB of col_h and col_top it already carried. That took the frame off the
 * end of the stack.
 *
 * IT DID NOT CRASH, WHICH IS WHY IT COST AN EVENING. There was no Guru and no
 * CPU TRAP line: the overflow scribbled on gta_view itself, `step[0]` came back
 * as garbage, and `R = (dst_w << 15) / step[0]` came out astronomically large -
 * so the ring loop in gta_render_frame simply never finished. The game reached
 * "interactive", printed its key list, and froze on the first interactive
 * frame, every single time, while the emulator sat there burning CPU.
 *
 * Localised with four log lines around the frame body: it reached "sim done,
 * about to render" and never reached "render done".
 *
 * So they live here. gta_map and gta_tiles are only hundreds of bytes but they
 * are moved too, because the next thing to grow will not announce itself
 * either. If any of them needs to grow again it now costs BSS, which the
 * linker accounts for, instead of stack, which nothing does. */
static gta_map   map;
static gta_tiles tiles;
static gta_view  view;
static gta_player player;

/* WHERE THE DRIVER'S DOOR IS, in world 16.16, for a car at (cx,cy) facing
 * `face`. The style file's door record is a hinge offset from the car's centre
 * in SOURCE pixels - `rpx` along the body, `rpy` across it - the same table
 * `gtadump carinfo -v` prints. Halved to world scale, rotated by the car's
 * heading, and pushed a little further out so the person stands BESIDE the car
 * rather than inside it.
 *
 * A vehicle with no door record at all (a bike) gets a point off its left
 * flank, which is the side a rider mounts from. */
static void car_door_point(const gta_car_info *ci, long cx, long cy, int face,
                           long *dx, long *dy)
{
    long along, across;
    long fx = gta_sin(face), fy = -gta_cos(face);
    long rx = gta_cos(face), ry = gta_sin(face);

    /* THE TABLE GIVES THE HINGE, AND A HINGE IS ON THE BODYWORK.
     *
     * Taking the record literally put the player six pixels off the centre
     * line of a car eight pixels wide - i.e. inside it, where the car sprite
     * is drawn over him and he is simply not there. He was on screen the whole
     * time and invisible, which looked exactly like "getting out does nothing".
     *
     * So the record decides WHICH SIDE the door is on and how far along the
     * body it sits; how far OUT is the car's own half-width plus room for a
     * person. `rpy` of zero (some records have it) means the left. */
    if (ci->n_doors > 0) {
        along  = (long)ci->doors[0].rpx / 2;
        across = (long)ci->doors[0].rpy;
    } else {
        along  = 0;
        across = -1;
    }
    {
        long out = (long)gta_car_world_wid(ci) / 2 + 5;
        across = (across < 0) ? -out : out;
    }

    *dx = cx + (fx * along + rx * across) * 4;
    *dy = cy + (fy * along + ry * across) * 4;
}


int main(void)
{
    unsigned char *chunky;
    int pitch;
    AmigaGfxEvent ev;
    int running = 1;
    int walk_mode = 1;
    /* IN A CAR - Phase 5 item 3b. While in_car the arrows drive the
     * vehicle physics (gta_vehphys), the player sprite is hidden, SPACE is
     * the handbrake instead of the frame dump, and RETURN steps out. */
    int in_car = 0;
    int enter_req = 0;
    /* GETTING IN AND OUT IS A STATE, not an instant. 0 = neither, 1 = getting
     * in, 2 = getting out; the rest is what has to survive the animation. */
    int enter_anim = 0, enter_step = 0, enter_tick = 0;
    int enter_model = 0, enter_face = 0, enter_remap = -1, enter_damage = 0;
    long enter_cx = 0, enter_cy = 0;
    int handbrake = 0;
    int veh_slide_ticks = 0;    /* how much of the last report the car slid */
    gta_veh veh;
    int backend = GTA_DEFAULT_BACKEND;
    int up = 0, down = 0, left = 0, right = 0, fast = 0;
    unsigned long sim_accum = 0, sim_last = 0, frame_t0 = 0;
    long sim_ticks = 0;
    int unknown_keys = 0;
    int zoom_in = 0, zoom_out = 0, last_zoom = 0;
    int frames = 0;
    unsigned long t0, t1;

    log_line("gta: start");

    if (gta_tiles_load(TILES_PATH, &tiles) != 0) {
        log_line("gta: FAILED to load " TILES_PATH
                 " - run tools/bin/deploy.sh");
        return 20;
    }
    gta_tiles_describe(&tiles, stdout);
    fflush(stdout);

    if (gta_map_load(MAP_PATH, &map) != 0) {
        log_line("gta: FAILED to load " MAP_PATH);
        gta_tiles_free(&tiles);
        return 20;
    }
    gta_map_describe(&map, stdout);
    fflush(stdout);

    /* WHICH DISPLAY BACKEND, read from a one-line file rather than compiled in.
     *
     * The same binary has to run on the AGA machine and on the RTG one,
     * because the whole point of measuring RTG is to compare it against AGA -
     * and two binaries built at different moments are not comparable. Both
     * WinUAE configs mount the same drawer, so the switch is a file, exactly
     * like autoinput.txt and autowalk.txt.
     *
     * Missing file means AGA, which is the target machine. */
    {
        FILE *bf = fopen(GTA_DIR "backend.txt", "r");
        if (bf) {
            char word[16];
            if (fscanf(bf, "%15s", word) == 1) {
                if (word[0] == 'r' || word[0] == 'R')
                    backend = AMIGAGFX_BACKEND_RTG;
                else if (word[0] == 'w' || word[0] == 'W')
                    backend = AMIGAGFX_BACKEND_WB;
            }
            fclose(bf);
        }
        printf("gta: backend requested %s\n",
               backend == AMIGAGFX_BACKEND_RTG ? "RTG" :
               backend == AMIGAGFX_BACKEND_WB  ? "WB"  : "AGA");
        fflush(stdout);
    }

    /* The A/B switches, same shape as the backend file above. */
    {
        FILE *of = fopen(GTA_DIR "opts.txt", "r");
        if (of) {
            char word[16];
            long val;
            while (fscanf(of, "%15s %ld", word, &val) == 2) {
                if (strcmp(word, "overlay") == 0)      opt_overlay = (int)val;
                else if (strcmp(word, "traffic") == 0) opt_traffic = (int)val;
                else if (strcmp(word, "fleet") == 0)   opt_fleet   = (int)val;
                else if (strcmp(word, "catchup") == 0) opt_catchup = (int)val;
                else if (strcmp(word, "benchframes") == 0) opt_benchf = (int)val;
                else if (strcmp(word, "width") == 0)   opt_width   = (int)val;
                else if (strcmp(word, "camh") == 0)    opt_camh    = (int)val;
            }
            fclose(of);
        }
        if (opt_catchup < 1) opt_catchup = 1;
        if (opt_benchf < 1) opt_benchf = 1;
        printf("gta: opts - overlay %d, traffic %d, fleet %d, catchup %d, "
               "benchframes %d%s\n",
               opt_overlay, opt_traffic, opt_fleet, opt_catchup, opt_benchf,
               opt_benchf != BENCH_FRAMES
                   ? "   *** NOT 60 - not comparable with the notes ***" : "");
        fflush(stdout);
    }

    /* The palette has to be known before the screen opens, because
     * open_display() re-applies it on every reopen and a toggle must not come
     * back with the wrong colours. */
    g_palette = tiles.palette;
    g_backend_used = backend;
    /* Before the screen opens: after it, the pens are already baked into it. */
    choose_bar_pens(tiles.palette);
    if (!open_display(NULL, g_show_bar)) {
        log_line("gta: amigagfx_open failed");
        gta_map_free(&map);
        gta_tiles_free(&tiles);
        return 20;
    }
    /* What actually opened, so a later reopen asks for the same thing rather
     * than retrying a backend that already refused once. */
    g_backend_used = amigagfx_backend();
    /* amigagfx_open() falls back to AGA silently if RTG will not open, so the
     * one thing a measurement run must not do is assume it got what it asked
     * for. A run that quietly fell back would be reported as "RTG is exactly
     * as fast as AGA", which is true and useless. */
    printf("gta: screen open, backend actually %s%s\n",
           amigagfx_backend() == AMIGAGFX_BACKEND_RTG ? "RTG" :
           amigagfx_backend() == AMIGAGFX_BACKEND_WB  ? "WB"  : "AGA",
           (amigagfx_backend() != backend)
               ? "   *** NOT WHAT WAS ASKED FOR - fell back ***" : "");
    fflush(stdout);

    /* GTA's own palette, carried through the bake unchanged and already scaled
     * from 6-bit VGA to 8-bit by gta_style_load on the host. The screen's copy
     * of it is set by open_display(); this is the HUD's. */
    gta_hud_init(tiles.palette);
    hud_t0 = amiga_uclock_us();

    /* open_display() has already bound these - see the note on it for why
     * every one of them lives in one place now. */
    chunky = g_chunky;
    pitch  = g_pitch;
    printf("gta: chunky %p pitch %d, title bar %s\n",
           (void *)chunky, pitch, g_show_bar ? "ON" : "OFF");
    fflush(stdout);

    gta_render_init(&view, &map, &tiles);
    gta_render_target(&view, chunky, SCREEN_W, SCREEN_H, pitch);
    gta_render_look_at_block(&view, START_BX, START_BY);

    /* The player goes on the street the camera starts over. If that column has
     * nothing walkable in it the log says so rather than the player silently
     * standing inside a building - which is a difference that costs an
     * afternoon to find from a picture alone. */
    if (!gta_player_init(&player, &map, &tiles, START_BX, START_BY))
        log_line("gta: WARNING - the start block has no walkable layer");
    printf("gta: player on block (%d,%d) layer %d ground %d, "
           "ped sprites %d from %d\n",
           START_BX, START_BY, player.layer, player.ground,
           player.ped_count, player.ped_base);
    fflush(stdout);

    /* Parked cars around the start. They do not drive yet - this is the
     * placement and the drawing, which is the same order the player was built
     * in. The seed is fixed so two runs of the same build put the same cars in
     * the same street, which is what makes a screenshot comparable. */
    gta_traffic_init(&traffic, &tiles, 12345UL);
    if (opt_fleet >= 0)
        traffic.fleet_cap = opt_fleet;
    /* The tick times its own phases on the E-clock - see prof_us in
     * gta_traffic.h. Host tools leave the pointer NULL and pay nothing. */
    traffic.prof_clock = amiga_uclock_us;
    gta_peds_init(&peds, &tiles, 777UL);
    /* The reservation overlay, on by default while traffic is debugged -
     * the developer reads the bookings straight off the screen. F9. */
    gta_render_set_overlay(&view, &traffic, opt_overlay);

    /* THE NAVIGATION GRID, 384 KB of it, and the traffic's routes need it.
     *
     * It is the original's own structure (gta_nav.h) and it is the reason the
     * map itself is still left compressed: this is a twelfth of what expanding
     * the map would cost and it answers the only question the AI ever asks.
     * If the machine cannot spare it the game runs anyway - cars then follow
     * the arrows block by block instead of driving anywhere in particular -
     * so it is a printed warning and not a refusal to start. */
    if (gta_nav_build(&nav, &map) == 0) {
        gta_traffic_set_nav(&traffic, &nav);
        gta_peds_set_nav(&peds, &nav);
        printf("gta: navigation grid %ld KB\n", (long)(GTA_NAV_BYTES / 1024));
    } else {
        log_line("gta: no memory for the navigation grid - traffic will not "
                 "drive routes");
    }
    fflush(stdout);
    {
        int parked = gta_traffic_park(&traffic, &map, START_BX, START_BY,
                                      8, GTA_MAX_CARS);
        printf("gta: %d cars parked around (%d,%d)\n",
               parked, START_BX, START_BY);
        fflush(stdout);
    }

    /* One frame first, on its own, so a crash in the renderer is a crash in a
     * known place rather than somewhere inside a timing loop. */
    mode_apply(&view);
    gta_render_frame(&view);
    present_frame(&view, &player, 0);
    printf("gta: first frame - %ld columns, %ld lids, %ld walls\n",
           view.columns_visited, view.lids_drawn, view.walls_drawn);
    fflush(stdout);
    dump_frame(GTA_DIR "frame.raw", chunky, pitch, SCREEN_W, SCREEN_H,
               tiles.palette);

    /* SELF-TEST OF THE F3 PATH, once, before anything depends on it.
     *
     * Toggling the title bar closes and reopens the screen, which frees and
     * reallocates the chunky buffer - so it is the one control in the game
     * that can leave `g_chunky`, `g_pitch` and the renderer's target pointing
     * at freed memory. That failure is a HALT1, not a wrong picture, and it
     * cannot be reached from the host or from a scripted run: driving the game
     * needs a key, and synthesising host input is banned in this project.
     *
     * So the path is exercised here instead. Off, then on again, ending in the
     * state it started in, with a frame drawn and dumped afterwards - if the
     * rebinding were wrong, that frame would be the crash. */
    log_line("gta: self-test - toggling the title bar off and back on");
    if (toggle_bar(&view) && toggle_bar(&view)) {
        chunky = g_chunky;
        pitch  = g_pitch;
        gta_render_frame(&view);
        present_frame(&view, &player, 0);
        printf("gta: self-test passed - chunky %p pitch %d, bar %s\n",
               (void *)g_chunky, g_pitch, g_show_bar ? "ON" : "OFF");
        dump_frame(GTA_DIR "frame_bar.raw", g_chunky, g_pitch,
                   SCREEN_W, SCREEN_H, tiles.palette);
    } else {
        log_line("gta: self-test FAILED - the screen could not be reopened");
    }
    fflush(stdout);

    /* The unattended benchmark. It scrolls while it measures, because a static
     * camera would let a future dirty-rectangle optimisation flatter itself;
     * the number this prints has to mean "the city is moving". */
    log_line("gta: benchmark");

    /* WHAT THE TRAFFIC COSTS, which stopped being an idle question when the
     * cars started following routes: a breadth-first search over a few
     * thousand blocks is not free on a 68020, and the whole design rests on
     * running at most one of them per tick. The simulation runs at 50 Hz, so
     * anything much over 200 us a tick is a tenth of the machine. */
    {
        unsigned long ta, tb;
        int t;

        ta = amiga_uclock_us();
        for (t = 0; t < 200; t++)
            gta_traffic_tick(&traffic, &map, view.cam_x, view.cam_y);
        tb = amiga_uclock_us();
        printf("gta: traffic - %lu us per tick over 200 ticks, %d cars, "
               "%ld routes found (%ld failed)\n",
               (tb - ta) / 200, traffic.n, traffic.routes_ok,
               traffic.routes_failed);
        fflush(stdout);
    }

    /* Split the frame into renderer and chunky-to-planar before optimising
     * either. Everything on the Phase 7 list - lookup tables, cached scaled
     * tiles, assembly inner loops - only touches the renderer half, so the c2p
     * figure is the ceiling on all of it put together. Guessing which half is
     * bigger would be exactly the "hand-optimise ahead of a measurement" this
     * project forbids. */
    {
        unsigned long render_us = 0, blit_us = 0, ta, tb;

        t0 = amiga_uclock_us();
        for (frames = 0; frames < opt_benchf; frames++) {
            gta_render_move(&view, SCROLL_SLOW, 0);
            ta = amiga_uclock_us();
            gta_render_frame(&view);
            hud_draw(&view, chunky, pitch);
            tb = amiga_uclock_us();
#ifdef GTA_SCALE2X
            scale2x_rows(g_render_buf, RENDER_W, amigagfx_chunky(),
                         amigagfx_pitch(), RENDER_W, RENDER_H);
            amigagfx_blit(0, 0, RENDER_W * 2, RENDER_H * 2);
#else
            amigagfx_blit(0, 0, SCREEN_W, SCREEN_H);
#endif
            render_us += tb - ta;
            blit_us += amiga_uclock_us() - tb;
        }
        t1 = amiga_uclock_us();
        log_fps("gta: scrolling downtown", opt_benchf, t1 - t0);
        printf("gta: split - render %lu us/frame, c2p %lu us/frame\n",
               render_us / opt_benchf, blit_us / opt_benchf);
        fflush(stdout);

        /* And inside the renderer: how much is pushing pixels, and how much is
         * the walk that decides which pixels? The no-blit frame does every map
         * lookup, projection and clip and then throws the blit away. */
        ta = amiga_uclock_us();
        for (frames = 0; frames < 20; frames++)
            memset(chunky, 0, (size_t)pitch * SCREEN_H);
        tb = amiga_uclock_us();
        printf("gta: clear   %lu us/frame (%d bytes)\n",
               (tb - ta) / 20UL, pitch * SCREEN_H);
        /* THE MEASUREMENT THAT DECIDES WHETHER A PLANAR BLITTER IS WORTH
         * WRITING.
         *
         * c2p costs about 8.6 ms a frame on AGA, a third of the whole frame,
         * and it is the one cost no renderer change touches. The obvious
         * escape is to bake the tiles in PLANAR form and blit them straight
         * into the bitplanes, so there is no conversion at all - GTA's flat
         * mode draws nothing but constant-size axis-aligned squares, which is
         * exactly what planar blitting is good at.
         *
         * That only pays if c2p's cost is the BIT SHUFFLE. If instead it is
         * simply slow to write 64000 bytes into Chip RAM, then a planar
         * blitter moves the same bytes to the same memory in a different order
         * and saves nothing - and the whole planar / hardware-scroll design is
         * dead before a line of it is written.
         *
         * So: the same memset, the same byte count, three destinations. Fast
         * RAM above, Chip RAM here, and the c2p figure printed a few lines up
         * does read-shuffle-write. Subtracting gives the split.
         *
         * It writes to the VISIBLE bitplanes on purpose - real display memory
         * with display DMA competing for the same bus, not a quiet buffer
         * somewhere else. The screen goes black for a moment; the frame after
         * this repaints it. */
        {
            unsigned char *planes = amigagfx_planes();
            long plane_bytes = amigagfx_planes_bytes();

            if (planes && plane_bytes > 0) {
                ta = amiga_uclock_us();
                for (frames = 0; frames < 20; frames++)
                    memset(planes, 0, (size_t)plane_bytes);
                tb = amiga_uclock_us();
                printf("gta: chipwr  %lu us/frame (%ld bytes into Chip RAM)\n",
                       (tb - ta) / 20UL, plane_bytes);
                /* That memset went straight into the VISIBLE bitplanes, which
                 * is what makes it an honest measurement - and it also wiped
                 * the Workbench title bar, because the bar is pixels in the
                 * same planes. Intuition does not know and will not redraw it
                 * until something else makes it, so the bar stayed black until
                 * the screen was clicked. Put it back. */
                amigagfx_refresh_titlebar();
            } else {
                printf("gta: chipwr  n/a (no bitplanes - RTG)\n");
            }
            fflush(stdout);
        }

        /* fflush after EVERY benchmark line, not just the last of a group.
         * The RTG machine stopped somewhere after the split line and the log
         * could not say where, because this print and the one below shared a
         * single flush at the end - so two loops were suspects where one would
         * have done. An unflushed diagnostic is not a diagnostic. */
        fflush(stdout);

        view.debug_no_blits = 1;
        ta = amiga_uclock_us();
        for (frames = 0; frames < 20; frames++) {
            gta_render_move(&view, SCROLL_SLOW, 0);
            gta_render_frame(&view);
            hud_draw(&view, chunky, pitch);
        }
        tb = amiga_uclock_us();
        view.debug_no_blits = 0;
        printf("gta: walk    %lu us/frame (clear included)\n",
               (tb - ta) / 20UL);
        printf("gta: lid cache - %ld tiles scaled, %lu of %lu bytes, "
               "%ld overflow\n", view.lc_fills, view.lc_used,
               (unsigned long)GTA_LIDCACHE_BYTES, view.lc_full);
        fflush(stdout);
    }

    /* And again over the water, because that is where it is slowest and a
     * report of "slower over there" is worth nothing until it is a number.
     *
     * The likely reason is structural rather than mysterious: water is the lid
     * of layer 0, which sits on grid 1, and only GTA_GREF - the street, grid 2
     * - is drawn at exactly 32 pixels. Every other level goes through the
     * scaled path instead of the memcpy fast path in blit_lid(), so a screen
     * full of water is a screen full of scaled blits. Fixing that belongs in
     * Phase 7, with this number to beat. */
    gta_render_look_at_block(&view, WATER_BX, WATER_BY);
    gta_render_frame(&view);
    hud_draw(&view, chunky, pitch);
    t0 = amiga_uclock_us();
    for (frames = 0; frames < opt_benchf; frames++) {
        gta_render_move(&view, SCROLL_SLOW, 0);
        gta_render_frame(&view);
        hud_draw(&view, chunky, pitch);
        amigagfx_blit(0, 0, SCREEN_W, SCREEN_H);
    }
    t1 = amiga_uclock_us();
    log_fps("gta: scrolling over water", opt_benchf, t1 - t0);

    /* And once zoomed all the way out. This is the expensive case and it is
     * expensive twice over: the visible region is four times as many blocks,
     * and no tile is 32 pixels any more, so even the street loses the memcpy
     * fast path in blit_lid(). Measured rather than warned about. */
    gta_render_look_at_block(&view, START_BX, START_BY);
    gta_render_set_zoom(&view, 16);
    gta_render_frame(&view);
    hud_draw(&view, chunky, pitch);
    t0 = amiga_uclock_us();
    for (frames = 0; frames < opt_benchf; frames++) {
        gta_render_move(&view, SCROLL_SLOW, 0);
        gta_render_frame(&view);
        hud_draw(&view, chunky, pitch);
        amigagfx_blit(0, 0, SCREEN_W, SCREEN_H);
    }
    t1 = amiga_uclock_us();
    log_fps("gta: scrolling zoomed out (16 px)", opt_benchf, t1 - t0);

    /* And with the zoom SLIDING, a pixel a frame, which is what holding a zoom
     * key does. Every step changes the per-level tile sizes, so the pre-scaled
     * lid cache is thrown away and refilled from scratch on every one of these
     * frames. This is the worst case that continuous zoom introduced and the
     * question it raises - "what does the cache cost when it can never settle"
     * - deserves a number rather than an assurance.
     *
     * It oscillates by ONE pixel, 32 and 33, on purpose: that keeps the amount
     * of geometry drawn the same as the static benchmark above, so the
     * difference between the two is the cache rebuild and almost nothing else.
     * Sliding over a wide range would also lose the street its 32-pixel memcpy
     * path, and then the number would be measuring two things at once. */
    gta_render_set_zoom(&view, GTA_TILE_DIM);
    gta_render_frame(&view);
    t0 = amiga_uclock_us();
    for (frames = 0; frames < opt_benchf; frames++) {
        gta_render_zoom(&view, (frames & 1) ? -1 : 1);
        gta_render_frame(&view);
        hud_draw(&view, chunky, pitch);
        amigagfx_blit(0, 0, SCREEN_W, SCREEN_H);
    }
    t1 = amiga_uclock_us();
    log_fps("gta: zoom sliding 32-33 (cache rebuilt every frame)",
            opt_benchf, t1 - t0);
    gta_render_set_zoom(&view, GTA_TILE_DIM);

    /* THE FOUR RENDER MODES, MEASURED IN ONE RUN.
     *
     * The point of measuring them together is that the run-to-run spread on
     * this benchmark is about 3 fps (the notes), which is more than some of
     * the differences being looked for. Four numbers from one binary, one boot
     * and one camera path are comparable with each other in a way that four
     * numbers from four runs are not.
     *
     * Uncapped on purpose - the frame cap belongs to the interactive loop and
     * would turn every one of these into "60". */
    {
        static const struct { int flat, scale, camh; const char *name; } modes[6] = {
            { 0, 1, GTA_CAM_H,       "gta: mode 2.5D    full 320x200" },
            { 0, 2, GTA_CAM_H,       "gta: mode 2.5D    half 160x100" },
            { 0, 1, GTA_CAM_H_LIGHT, "gta: mode 2.5D-lt full 320x200" },
            { 0, 2, GTA_CAM_H_LIGHT, "gta: mode 2.5D-lt half 160x100" },
            { 1, 1, GTA_CAM_H,       "gta: mode flat-2D full 320x200" },
            { 1, 2, GTA_CAM_H,       "gta: mode flat-2D half 160x100" }
        };
        int m;

        for (m = 0; m < 6; m++) {
            mode_flat  = modes[m].flat;
            gta_render_set_cam_h(&view, modes[m].camh);
            mode_scale = modes[m].scale;
            zoom_display = GTA_TILE_DIM;
            gta_render_look_at_block(&view, START_BX, START_BY);
            mode_apply(&view);
            gta_render_frame(&view);          /* warm the lid cache */

            t0 = amiga_uclock_us();
            for (frames = 0; frames < opt_benchf; frames++) {
                gta_render_move(&view, SCROLL_SLOW, 0);
                mode_apply(&view);
                gta_render_frame(&view);
                present_frame(&view, &player, 0);
            }
            t1 = amiga_uclock_us();
            log_fps(modes[m].name, opt_benchf, t1 - t0);
            printf("       %ld columns, %ld lids, %ld walls, "
                   "%ld tiles in cache\n",
                   view.columns_visited, view.lids_drawn, view.walls_drawn,
                   view.lc_fills);
            fflush(stdout);

            /* A frame of each, because a mode that RUNS is not a mode that
             * DRAWS. The half-resolution ones go through gta_render_expand()
             * into the chunky buffer, and the only way to know that landed
             * correctly is to look at the bytes c2p is about to consume. */
            {
                char path[64];
                snprintf(path, sizeof path, GTA_DIR "mode%d.raw", m);
                dump_frame(path, chunky, pitch, SCREEN_W, SCREEN_H,
                           tiles.palette);
            }
        }
        mode_flat = 0;
        mode_scale = 1;
        zoom_display = GTA_TILE_DIM;
        gta_render_set_cam_h(&view, GTA_CAM_H);
        /* AND PUT IT BACK ON THE VIEW. Setting the mode variables without
         * applying them left v->flat_2d holding the last benchmark's value,
         * which made the camera-height sweep below report the same 317 columns
         * at every height - flat 2D ignores the camera entirely, so the sweep
         * was measuring nothing. */
        mode_apply(&view);

        /* WHAT THE PERSPECTIVE COSTS, as a number rather than an argument.
         *
         * The camera height is the biggest single lever on the frame in this
         * renderer and it is not obvious why: it does not change the
         * arithmetic per pixel at all, it changes how much CITY is on screen.
         * A low camera splays the grid outward, so more blocks reach the frame
         * and, far more expensively, the walls between them get taller - and
         * walls are the whole of the per-pixel work once the lids are memcpy.
         *
         * At sixteen grid levels a downtown frame has about 670 columns and 30
         * walls in it; at the shipped eight it has 930 and 104. That is the
         * cost of having perspective at all, it is not a fault, and F7/F8 move
         * it live - so anyone who wants the frames back can have them and see
         * exactly what they are trading.
         *
         * Printed for every height the game will actually sit at. */
        {
            static const int heights[] = { 25, 32, 48, 64, 96 };
            int hi;

            gta_render_look_at_block(&view, START_BX, START_BY);
            for (hi = 0; hi < (int)(sizeof heights / sizeof heights[0]); hi++) {
                unsigned long ta, tb;
                int f;

                gta_render_set_cam_h(&view, heights[hi]);
                gta_render_frame(&view);          /* rebuild the lid cache */

                ta = amiga_uclock_us();
                for (f = 0; f < opt_benchf; f++) {
                    gta_render_frame(&view);
                    present_frame(&view, &player, 0);
                }
                tb = amiga_uclock_us();

                printf("gta: camera %2d.%02d levels: %lu us/frame, "
                       "%ld columns, %ld lids, %ld walls\n",
                       heights[hi] / 4, (heights[hi] % 4) * 25,
                       (tb - ta) / opt_benchf,
                       view.columns_visited, view.lids_drawn,
                       view.walls_drawn);
                fflush(stdout);
            }
            gta_render_set_cam_h(&view, GTA_CAM_H);
            gta_render_frame(&view);
        }

        /* THE NARROW MODES, measured and DUMPED in the same pass - every one
         * of them, at BOTH resolutions.
         *
         * They cannot be reached any other way from an unattended run: they
         * are on F4, and synthesising a keypress on the host is banned in this
         * project. The first version of this measured 266 at full resolution
         * only, and the combination it did not cover - narrow AND half
         * resolution - is precisely the one the developer then found broken on
         * screen. Six lines of loop is the whole fix for that class of gap.
         *
         * The pure-c2p loop after each one is what makes the "does a narrower
         * picture make chunky-to-planar any cheaper" question answerable with
         * a number instead of an argument: it blits the same rectangle the
         * frame does, and nothing else. */
        {
            int mi, si;

            for (mi = 0; mi < VIEW_MODES; mi++) {
                for (si = 1; si <= 2; si++) {
                    unsigned long ta, tb;
                    char path[64];
                    int f;

                    mode_narrow = mi;
                    mode_scale  = si;
                    mode_apply(&view);

                    /* The SAME view for all six, so the only difference
                     * between the lines is the mode. A scrolling camera is
                     * right for the headline benchmark and wrong here. */
                    gta_render_look_at_block(&view, START_BX, START_BY);

                    bench_blit_us = 0;
                    ta = amiga_uclock_us();
                    for (f = 0; f < opt_benchf; f++) {
                        gta_render_frame(&view);
                        present_frame(&view, &player, 0);
                    }
                    tb = amiga_uclock_us();

                    printf("gta: width %d %s res: %lu us/frame, "
                           "c2p %lu us/frame over %d px, %ld columns\n",
                           render_w(), (si == 2) ? "half" : "full",
                           (tb - ta) / opt_benchf,
                           bench_blit_us / opt_benchf, present_w(),
                           view.columns_visited);
                    fflush(stdout);

                    snprintf(path, sizeof path, GTA_DIR "w%d%s.raw",
                             render_w(), (si == 2) ? "h" : "");
                    dump_frame(path, chunky, pitch, SCREEN_W, SCREEN_H,
                               tiles.palette);
                }
            }
            mode_narrow = 0;
            mode_scale  = 1;
            mode_apply(&view);
        }
    }

    gta_render_look_at_block(&view, START_BX, START_BY);
    mode_apply(&view);
    gta_render_frame(&view);
    present_frame(&view, &player, 0);

    /* The scripted tour, if there is one. It runs before the interactive loop
     * so an unattended test proves the camera can be driven across the city
     * and into the edges of the map without a Guru - which is the part that
     * pressing an arrow key by hand would prove, and nobody is here to. */
    frames = autoinput_run(&view, SCREEN_W, SCREEN_H, chunky, pitch,
                           tiles.palette);
    if (frames > 0) {
        printf("gta: autoinput drew %d frames, camera now at block (%ld,%ld)\n",
               frames, view.cam_x >> 21, view.cam_y >> 21);
        fflush(stdout);
        dump_frame(GTA_DIR "frame_end.raw", chunky, pitch, SCREEN_W, SCREEN_H,
                   tiles.palette);
    }

    /* The scripted walk, for the same reason as the scripted tour above: an
     * unattended run has to be able to prove that the player moves, collides
     * and is drawn, without anybody pressing a key. Same file format as the
     * host harness (gtadump walk), so one script runs in both places. */
    {
        FILE *adf = fopen(GTA_DIR "autodrive.txt", "r");
        if (adf) {
            char ln[96];
            while (adq_n < AUTODRIVE_MAX && fgets(ln, sizeof ln, adf)) {
                int t, a, b, c, d;
                if (sscanf(ln, "wait %d", &t) == 1) {
                    adq[adq_n].op = 0; adq[adq_n].t = t; adq_n++;
                } else if (strncmp(ln, "enter", 5) == 0) {
                    adq[adq_n].op = 1; adq[adq_n].t = 1; adq_n++;
                } else if (sscanf(ln, "run %d %d %d %d %d",
                                  &t, &a, &b, &c, &d) == 5) {
                    adq[adq_n].op = 2; adq[adq_n].t = t;
                    adq[adq_n].thr = a; adq[adq_n].brk = b;
                    adq[adq_n].st = c; adq[adq_n].hb = d; adq_n++;
                } else if (strncmp(ln, "dump", 4) == 0) {
                    adq[adq_n].op = 3; adq[adq_n].t = 1; adq_n++;
                }
            }
            fclose(adf);
            if (adq_n) {
                adq_i = 0;
                adq_left = adq[0].t;
                printf("gta: autodrive script - %d orders\n", adq_n);
                fflush(stdout);
            }
        }
    }

    frames = autowalk_run(&view, &player, &map, SCREEN_W, SCREEN_H,
                          chunky, pitch, tiles.palette);
    if (frames > 0) {
        printf("gta: autowalk drew %d frames, player at block (%ld,%ld) "
               "layer %d\n", frames, player.x >> 21, player.y >> 21,
               player.layer);
        fflush(stdout);
        dump_frame(GTA_DIR "walk_end.raw", chunky, pitch, SCREEN_W, SCREEN_H,
                   tiles.palette);
    }

    /* THE GAME IS PLAYED AT 256 WIDE, not at 320.
     *
     * Everything above - the benchmarks, the camera tour, the scripted walk -
     * runs at full width so that the numbers and the frame dumps stay
     * comparable with every run before this one. The interactive session
     * starts in the mode that is actually the best deal: 256 is the only
     * narrow width the c2p can help with (it is the widest picture that is
     * both centred and on the 32-pixel grid), and it is worth 12% of the
     * frame. F4 cycles back to 320.
     *
     * BY NAME, NOT BY NUMBER, and that is not style. This line said
     * `mode_narrow = 2` while view_modes had three entries; dropping the
     * 266-wide mode on 2026-08-24 made 2 one past the end, so render_w()
     * returned whatever followed the array - 788737794 - and gta_render_frame
     * sized its ring loop from it and never came back. The game reached
     * "interactive", printed its key list and froze on the FIRST interactive
     * frame, every run, with no Guru and no CPU TRAP because nothing was
     * dereferenced: it was simply an integer nobody could see.
     *
     * VIEW_FAST is defined next to the table, so the two cannot drift apart
     * again, and the index is bounds-checked as well - a mode that does not
     * exist should fall back to full width, not to a number. */
    mode_narrow = VIEW_FAST;
    if (opt_width == 320) mode_narrow = VIEW_FULL;
    if (opt_width == 256) mode_narrow = VIEW_FAST;
    if (opt_camh >= 25 && opt_camh <= 96)
        gta_render_set_cam_h(&view, opt_camh);
    mode_apply(&view);

    hud_t0 = amiga_uclock_us();
    sim_last = amiga_uclock_us();
    frame_t0 = sim_last;
    log_line("gta: interactive - ON FOOT: arrows run and turn, shift walks, "
             "TAB frees the camera, -/= zoom, SPACE dumps a frame, ESC quits");
    log_line("gta:   F1 full res  F2 half res  F3 title bar  "
             "F4 width 320/256  F5 2.5D / 2.5D-light / flat  F6 frame cap  F7/F8 camera");
    frames = 0;
    t0 = amiga_uclock_us();
    prof_t0 = t0;

    while (running) {
        int dx = 0, dy = 0, speed;

        while (amigagfx_poll(&ev)) {
            if (ev.type == AMIGAGFX_EV_QUIT) {
                running = 0;
            } else if (ev.type == AMIGAGFX_EV_KEY) {
                int code = ev.code & 0x7F;
                int held = (ev.code & 0x80) ? 0 : 1;
                switch (code) {
                case KEY_UP:     up = held;    break;
                case KEY_DOWN:   down = held;  break;
                case KEY_LEFT:   left = held;  break;
                case KEY_RIGHT:  right = held; break;
                case KEY_LSHIFT:
                case KEY_RSHIFT: fast = held;  break;
                case KEY_ESC:    if (!held) running = 0; break;
                case KEY_TAB:
                    /* Two modes on one set of arrow keys. Walking is the
                     * default because that is now the game; the free camera
                     * stays because every renderer bug so far was reported by
                     * someone driving the camera to it and pressing SPACE. */
                    if (!held) {
                        walk_mode = !walk_mode;
                        if (walk_mode) {
                            view.cam_x = player.x;
                            view.cam_y = player.y;
                        }
                        printf("gta: %s mode\n", walk_mode ? "walking" : "camera");
                        fflush(stdout);
                    }
                    break;
                case KEY_F1:
                case KEY_F2:
                    /* Render resolution. F1 full, F2 half in both axes.
                     * 2x1 and 1x2 are not offered - see the note above
                     * mode_apply() for why, and it is a structural reason
                     * rather than an omission. */
                    if (!held) {
                        mode_scale = (code == KEY_F2) ? 2 : 1;
                        printf("gta: resolution %dx%d\n",
                               SCREEN_W / mode_scale, SCREEN_H / mode_scale);
                        fflush(stdout);
                    }
                    break;
                case KEY_F3:
                    /* The Workbench title bar. It costs nothing to draw - the
                     * screen is opened taller and the game area is untouched -
                     * but it is worth switching off for a clean screenshot, and
                     * on a real machine the bar's own refresh is not free.
                     *
                     * This closes and reopens the screen, so it is the one key
                     * that can fail. If it does, the previous setting is put
                     * back; if even that fails there is no display left and the
                     * loop stops rather than drawing into freed memory. */
                    if (!held) {
                        if (!toggle_bar(&view))
                            running = 0;
                    }
                    break;
                case KEY_F4:
                    /* Cycles full 320 -> 266 (4:3, square pixels) -> 256
                     * (5:4, and on the c2p's 32-pixel grid). See view_modes:
                     * the 266 one cannot make the c2p any cheaper, the 256 one
                     * takes a fifth off it as well as off the renderer. */
                    if (!held) {
                        mode_narrow = (mode_narrow + 1) % VIEW_MODES;
                        printf("gta: render width %d - %s\n", render_w(),
                               view_modes[view_mode()].name);
                        fflush(stdout);
                    }
                    break;
                case KEY_F5:
                    if (!held) {
                        static const char *pn[PROJ_COUNT] = {
                            "2.5D full", "2.5D light", "flat 2D"
                        };
                        mode_proj = (mode_proj + 1) % PROJ_COUNT;
                        mode_flat = (mode_proj == PROJ_FLAT);
                        if (mode_proj == PROJ_FULL)
                            gta_render_set_cam_h(&view, GTA_CAM_H);
                        else if (mode_proj == PROJ_LIGHT)
                            gta_render_set_cam_h(&view, GTA_CAM_H_LIGHT);
                        printf("gta: projection %s (camera %d)\n",
                               pn[mode_proj], view.cam_h);
                        fflush(stdout);
                    }
                    break;
                case KEY_F6:
                    /* The cap has to be switchable or the frame rate on screen
                     * stops being a measurement and becomes the cap. */
                    if (!held) {
                        frame_cap = !frame_cap;
                        printf("gta: frame cap %s\n",
                               frame_cap ? "60 fps" : "off");
                        fflush(stdout);
                    }
                    break;
                case KEY_F9:
                case KEY_F10:
                    /* SLOW MOTION, so the developer can watch the overlay
                     * breathe: F9 slows, F10 speeds back up, 0.1..1.0 in
                     * steps of 0.1. The multiplier scales the SIM clock, so
                     * player, traffic and the reservation lifecycle all slow
                     * together while input and rendering stay live. */
                    if (!held) {
                        game_speed += (code == KEY_F10) ? 10 : -10;
                        if (game_speed < 0)   game_speed = 0;
                        if (game_speed > 100) game_speed = 100;
                        printf("gta: game speed %d.%d\n",
                               game_speed / 100, (game_speed / 10) % 10);
                        fflush(stdout);
                    }
                    break;
                case KEY_F7:
                case KEY_F8:
                    /* THE CAMERA HEIGHT, LIVE. F7 brings it down - stronger
                     * perspective, taller building walls; F8 lifts it back
                     * towards a flat map.
                     *
                     * It is on a key because the developer has the DOS original
                     * running beside this and can see which value matches it,
                     * where two attempts to derive the number here were both
                     * wrong (the notes, "THE PROJECTION IS WRONG"). The value
                     * is printed on every press so the answer can be read off
                     * the log rather than remembered. It is also on the HUD, so
                     * a screenshot carries its own setting. */
                    if (held) {
                        int h = gta_render_cam_h(&view,
                                                 (code == KEY_F7) ? -1 : 1);
                        printf("gta: camera height %d  (walls %s)\n", h,
                               (code == KEY_F7) ? "taller" : "flatter");
                        fflush(stdout);
                    }
                    break;
                case KEY_MINUS:
                case KEY_NUMMINUS: zoom_out = held; break;
                case KEY_EQUALS:
                case KEY_NUMPLUS:  zoom_in = held;  break;
                case KEY_SPACE:
                    /* While driving, SPACE is the handbrake - the frame dump
                     * moves aside because both hands are on the game. */
                    if (in_car) {
                        handbrake = held;
                        break;
                    }
                    /* Dump whatever is on screen right now.
                     *
                     * There is no other way to get at the view a person is
                     * actually looking at: host input synthesis is banned, so
                     * an agent cannot drive the camera to the thing being
                     * reported, and a rebuild puts the camera back at the
                     * start. One keypress turns "there is a cut-out block over
                     * here" into a file that can be measured. */
                    if (!held) {
                        dump_frame(GTA_DIR "frame_live.raw", chunky, pitch,
                                   SCREEN_W, SCREEN_H, tiles.palette);
                        printf("gta: camera at block (%ld,%ld)\n",
                               view.cam_x >> 21, view.cam_y >> 21);
                        fflush(stdout);
                    }
                    break;
                case KEY_RETURN:
                    if (!held)
                        enter_req = 1;
                    break;
                default:
                    /* The cursor-key codes below are constants of the Amiga
                     * keyboard, not something this program can verify on its
                     * own - nobody is at the emulator during an agent-driven
                     * run, and synthesising a keypress on the host is banned.
                     * Logging the first few unrecognised codes means that when
                     * a person does press something, the log says what it
                     * was, instead of the key silently doing nothing. */
                    if (held && unknown_keys < 8) {
                        printf("gta: unhandled key code $%02x\n", code);
                        fflush(stdout);
                        unknown_keys++;
                    }
                    break;
                }
            }
        }
        if (!running)
            break;

        /* Zoom slides while the key is down, a pixel per frame, the way GTA's
         * camera pulls back with speed - not in steps. It moves the DISPLAYED
         * zoom; mode_apply() divides it down for the renderer when the
         * resolution is halved. Stepping by mode_scale keeps the displayed
         * zoom a whole multiple of it, so the division loses nothing. */
        if (zoom_in != zoom_out) {
            zoom_display += zoom_in ? mode_scale : -mode_scale;
            if (zoom_display < GTA_ZOOM_MIN * mode_scale)
                zoom_display = GTA_ZOOM_MIN * mode_scale;
            if (zoom_display > GTA_ZOOM_MAX)
                zoom_display = GTA_ZOOM_MAX;
            if (zoom_display != last_zoom) {
                printf("gta: zoom %d px per block\n", zoom_display);
                fflush(stdout);
                last_zoom = zoom_display;
            }
        }

        /* THE SIMULATION IS SPENT IN WHOLE TICKS OF REAL TIME, not once per
         * frame. See the note on SIM_HZ. A pass may run no ticks at all (the
         * machine is faster than 25 Hz) or several (it is slower); either way
         * the player covers the same ground per second. */
        {
            unsigned long now = amiga_uclock_us();
            unsigned long dt = now - sim_last;
            int ticks = 0;

            sim_last = now;
            if (dt > (unsigned long)(SIM_US * opt_catchup))
                dt = (unsigned long)(SIM_US * opt_catchup);
            sim_accum += ((unsigned long)dt * (unsigned long)game_speed) / 100UL;

            while (sim_accum >= (unsigned long)SIM_US && ticks < opt_catchup) {
                /* The autodrive queue stands in for the keyboard. */
                if (adq_i < adq_n) {
                    switch (adq[adq_i].op) {
                    case 0: up = down = left = right = 0; handbrake = 0; break;
                    case 1: enter_req = 1; break;
                    case 2:
                        up    = adq[adq_i].thr;
                        down  = adq[adq_i].brk;
                        right = adq[adq_i].st > 0;
                        left  = adq[adq_i].st < 0;
                        handbrake = adq[adq_i].hb;
                        break;
                    case 3:
                        dump_frame(GTA_DIR "frame_live.raw", chunky, pitch,
                                   SCREEN_W, SCREEN_H, tiles.palette);
                        break;
                    }
                    if (--adq_left <= 0) {
                        adq_i++;
                        adq_left = adq_i < adq_n ? adq[adq_i].t : 0;
                        if (adq_i >= adq_n) {
                            up = down = left = right = 0; handbrake = 0;
                            printf("gta: autodrive done\n");
                            fflush(stdout);
                        }
                    }
                }
                /* ENTERING AND LEAVING A CAR - handled inside the tick so a
                 * grab and the fleet's own compaction cannot interleave. */
                if (enter_req && !enter_anim) {
                    enter_req = 0;
                    if (!in_car) {
                        int m_, f_, rm_, dmg_, drv_; long cx_, cy_;
                        /* A person must walk up to a door; the autodrive
                         * script cannot walk, so while it runs the reach is
                         * the whole street. */
                        if (gta_traffic_grab_car(&traffic, player.x, player.y,
                                                 player.layer,
                                                 adq_i < adq_n ? 320 : 48,
                                                 &m_, &cx_, &cy_, &f_,
                                                 &rm_, &dmg_, &drv_)) {
                            const gta_car_info *ci_ = &tiles.cars[m_];
                            long dx_, dy_;

                            car_door_point(ci_, cx_, cy_, f_, &dx_, &dy_);
                            /* THE PLAYER GOES TO THE DOOR, not into the seat.
                             * Teleporting straight into the middle of the car
                             * was the "we teleport into the centre instantly"
                             * report; he now stands at the handle and plays
                             * the ten-step get-in animation from there. */
                            player.x = dx_;
                            player.y = dy_;
                            player.angle = f_;
                            player.anim = GTA_ANIM_ENTER_CAR;
                            player.frame = 0;
                            player.frame_tick = 0;
                            enter_anim = 1;
                            enter_step = 0;
                            enter_tick = 0;
                            enter_model = m_;
                            enter_face = f_;
                            enter_cx = cx_;
                            enter_cy = cy_;
                            enter_remap = rm_;
                            enter_damage = dmg_;
                            /* AND THE DRIVER IS DRAGGED OUT. In the original
                             * the occupant is pulled from the seat and left
                             * stunned on the road - they are not deleted with
                             * the car. An abandoned car has nobody in it, so
                             * nobody comes out of it. */
                            if (drv_) {
                                gta_peds_drop(&peds, dx_, dy_, player.layer,
                                              (f_ + 128) & 255, -1,
                                              GTA_STUN_TICKS);
                                printf("gta: dragged the driver out\n");
                                fflush(stdout);
                            }
                        } else {
                            printf("gta: no car within reach\n");
                            fflush(stdout);
                        }
                    } else {
                        /* OUT AT THE DOOR, and the car stays behind. */
                        const gta_car_info *ci_ = &tiles.cars[veh.model];
                        long dx_, dy_;
                        int a_ = gta_veh_angle(&veh);

                        car_door_point(ci_, veh.ox, veh.oy, a_, &dx_, &dy_);
                        player.x = dx_;
                        player.y = dy_;
                        player.angle = a_;
                        player.anim = GTA_ANIM_EXIT_CAR;
                        player.frame = 0;
                        player.frame_tick = 0;
                        enter_anim = 2;
                        enter_step = 0;
                        enter_tick = 0;
                        in_car = 0;
                        handbrake = 0;
                        up = down = left = right = 0;
                        /* THE CAR DOES NOT VANISH. It goes back into the
                         * fleet as an abandoned one - drawn, solid, and
                         * enterable again - which is what "when we get out
                         * the car disappears" was about. */
                        if (!gta_traffic_abandon(&traffic, veh.model,
                                                 veh.ox, veh.oy, a_,
                                                 player.layer, veh.remap,
                                                 veh.damage))
                            printf("gta: fleet full, car lost\n");
                        printf("gta: getting out\n");
                        fflush(stdout);
                    }
                }
                /* THE ANIMATION ITSELF - one step every GTA_ENTER_TICKS, with
                 * the controls dead while it runs. Getting in ends with the
                 * player in the seat; getting out ends on his feet. */
                if (enter_anim) {
                    int steps = (enter_anim == 1) ? GTA_PED_ENTER_STEPS
                                                  : GTA_PED_EXITCAR_FRAMES;
                    int per   = (enter_anim == 1) ? GTA_ENTER_TICKS
                                                  : GTA_EXIT_TICKS;
                    player.frame = enter_step;
                    if (++enter_tick >= per) {
                        enter_tick = 0;
                        enter_step++;
                    }
                    if (enter_step >= steps) {
                        if (enter_anim == 1) {
                            gta_veh_init(&veh, &tiles, enter_model,
                                         enter_cx, enter_cy, enter_face);
                            veh.remap = enter_remap;
                            veh.damage = enter_damage;
                            in_car = 1;
                            walk_mode = 1;
                            printf("gta: in car - model %d at (%ld,%ld)\n",
                                   enter_model, enter_cx >> 21,
                                   enter_cy >> 21);
                        } else {
                            printf("gta: on foot\n");
                        }
                        player.anim = GTA_ANIM_STAND;
                        player.frame = 0;
                        enter_anim = 0;
                        fflush(stdout);
                    }
                    up = down = left = right = 0;
                }
                if (in_car) {
                    /* The car: up throttle, down brake/reverse, space the
                     * handbrake. Then the world - the nose must stay on
                     * ground a car can be on (road, pavement, the odd
                     * field); water and buildings are a wall. The bounce is
                     * a quarter of the speed, backwards: enough to feel the
                     * hit, not enough to be a toy. */
                    long wx0_, wy0_, wox0_, woy0_, wang0_;
                    int road_, wdmg_;
                    /* Is the block under the car a road? The original's
                     * road-snap assist needs to know, and only the caller
                     * has the map. Ground type 2 is the original's own test
                     * (`nav & 0x70 == 0x20`). */
                    road_ = gta_nav_ground(gta_nav_at_m((&nav),
                                (int)(veh.ox >> 21), (int)(veh.oy >> 21),
                                player.layer)) == 2;
                    wx0_ = veh.x;   wy0_ = veh.y;
                    wox0_ = veh.ox; woy0_ = veh.oy;
                    wang0_ = veh.ang16;
                    gta_veh_step(&veh, up ? 1 : 0, down ? 1 : 0,
                                 (right ? 1 : 0) - (left ? 1 : 0),
                                 handbrake, road_);
                    if (veh.sliding) veh_slide_ticks++;
                    /* THE WHOLE BODY, not the nose - see gta_veh_wall(). The
                     * nose test could not see a car reversing into a wall at
                     * all, and a bus is longer than the blocks it drives
                     * between. */
                    wdmg_ = gta_veh_wall(&veh, &nav, player.layer,
                                         wx0_, wy0_, wox0_, woy0_, wang0_);
                    if (wdmg_) {
                        /* Item 3c's other half: a wall costs bodywork too.
                         * The charge is the impact speed in whole pixels
                         * per tick, less a grace pixel - a nudge at
                         * parking speed is free, a full-speed wall is
                         * eight points. */
                        /* A wall costs bodywork too. The charge is the
                         * impact speed in whole pixels per tick, less a
                         * grace pixel - a nudge at parking speed is free,
                         * a full-speed wall is eight points. Backing the
                         * body out and bouncing it is gta_veh_wall's job. */
                        int dmg = wdmg_ - 1;
                        if (dmg > 0) {
                            veh.damage += dmg;
                            printf("gta: wall hit at %d px/tick - "
                                   "damage %d\n", wdmg_, veh.damage);
                            fflush(stdout);
                        }
                    }
                    /* THE RAM - item 3c. The fleet takes its share inside
                     * gta_traffic_ram (speed cut, shove, damage); the
                     * player's share comes back as a velocity delta and a
                     * yaw kick.
                     *
                     * THE IMPULSE AND THE BODYWORK ARE SEPARATE. Any
                     * contact pushes - that is what keeps two cars from
                     * grinding through each other - but only a real impact
                     * is charged, and the return value counts those alone.
                     * Leaning on a parked car with the throttle down used
                     * to bill a point a tick, for ever. */
                    {
                        long rvx, rvy, ryaw, rpx, rpy;
                        int nhit = gta_traffic_ram(&traffic, veh.ox, veh.oy,
                                       gta_veh_angle(&veh),
                                       veh.len / 2, veh.wid / 2,
                                       veh.vx, veh.vy, veh.mass,
                                       player.layer, &rvx, &rvy, &ryaw,
                                       &rpx, &rpy);
                        if (rvx || rvy || ryaw) {
                            veh.vx += rvx;
                            veh.vy += rvy;
                            veh.ang16 = (veh.ang16 + ryaw) & 0xFFFFFFL;
                        }
                        /* The overlap that is left after the impulse is undone
                         * by moving the body, and BOTH centres move together -
                         * the car has not rotated, so the centre of mass and
                         * the geometric centre travel the same distance. */
                        if (rpx || rpy) {
                            veh.x += rpx;  veh.ox += rpx;
                            veh.y += rpy;  veh.oy += rpy;
                        }
                        if (nhit) {
                            veh.damage += nhit;
                            printf("gta: ram x%d - player dv (%ld,%ld) "
                                   "damage %d\n", nhit,
                                   rvx >> 16, rvy >> 16, veh.damage);
                            fflush(stdout);
                        }
                    }
                    player.x = veh.ox;
                    player.y = veh.oy;
                } else if (walk_mode) {
                    /* GTA's own on-foot controls: forward and back on the
                     * up/down keys, left and right TURN rather than strafe,
                     * and the player RUNS by default - the original has no
                     * walk key at all. Shift is the exception, not the
                     * accelerator. */
                    gta_player_update(&player, &map,
                                      (right ? 1 : 0) - (left ? 1 : 0),
                                      (up ? 1 : 0) - (down ? 1 : 0),
                                      fast);
                }
                /* The traffic runs on the SAME tick as the player and outside
                 * the walk_mode test, so the city keeps moving while the free
                 * camera is being flown around it.
                 *
                 * IT IS TOLD HOW MUCH CITY IS ON SCREEN, every tick, because
                 * the zoom slides continuously: the fleet is kept and spawned
                 * around the view rather than around a constant, and without
                 * this cars vanish and pop into existence in plain sight the
                 * moment the camera pulls back. */
                if (opt_traffic) {
                    if (in_car) {
                        long sp = veh.vx < 0 ? -veh.vx : veh.vx;
                        long sq = veh.vy < 0 ? -veh.vy : veh.vy;
                        gta_traffic_set_player(&traffic, 1, veh.ox, veh.oy,
                                               sp > sq ? sp : sq,
                                               gta_veh_angle(&veh),
                                               player.layer,
                                               veh.len / 2, veh.wid / 2);
                    } else {
                        gta_traffic_set_player(&traffic, 0, 0, 0, 0, 0, 0,
                                               0, 0);
                    }
                    gta_traffic_set_view_blocks(&traffic,
                                            (render_w() / 2) / zoom_display + 1);
                    gta_traffic_tick(&traffic, &map, view.cam_x, view.cam_y);
                }
                gta_peds_tick(&peds, &map, view.cam_x, view.cam_y);
                if (in_car) {
                    int ph = gta_peds_ram(&peds, veh.ox, veh.oy,
                                          gta_veh_angle(&veh),
                                          veh.len / 2, veh.wid / 2,
                                          player.layer);
                    if (ph) {
                        printf("gta: ran over %d - %ld so far\n",
                               ph, peds.stat_runover);
                        fflush(stdout);
                    }
                }
                sim_accum -= (unsigned long)SIM_US;
                ticks++;
                sim_ticks++;

                /* AND SAY WHAT THE TRAFFIC IS DOING, every five seconds.
                 *
                 * Two screenshots of the emulator a minute apart came back
                 * pixel-identical - the fleet was frozen solid - while every
                 * host test said 87% of it was moving, and the log went silent
                 * the moment the game turned interactive. There was nothing to
                 * check. This is the line that answers it: how many cars, how
                 * many moving, how far the whole fleet has travelled since the
                 * last report, and why the stopped ones are stopped. */
                if ((sim_ticks % 250) == 0) {
                    char ln[160];
                    long moved = traffic.stat_moved - traffic_moved_last;

                    /* The driven car's own five seconds, when there is one.
                     * `sliding` is the original's tyre-mark test and this is
                     * the only place it is visible until something draws
                     * them; `damage` above 25 is already costing engine. */
                    if (in_car) {
                        snprintf(ln, sizeof ln,
                                 "gta: car - %ld px/step, sliding %d of 250 "
                                 "ticks, damage %d%s",
                                 (long)((veh.vx < 0 ? -veh.vx : veh.vx)
                                      + (veh.vy < 0 ? -veh.vy : veh.vy)) >> 16,
                                 veh_slide_ticks, veh.damage,
                                 veh.damage > 25 ? " (engine derated)" : "");
                        log_line(ln);
                    }
                    veh_slide_ticks = 0;

                    traffic_moved_last = traffic.stat_moved;
                    /* snprintf, never sprintf - see the toolchain notes; the
                     * one that is broken on this libc is the one without a
                     * size. */
                    snprintf(ln, sizeof ln,
                             "gta: traffic %d/%d moving, %ld blocks in 5s, "
                             "held queue %ld light %ld box %ld merge %ld "
                             "dead %ld road %ld gap %ld",
                             traffic.stat_moving, traffic.n, moved / 32,
                             traffic.stat_hold[GTA_HOLD_QUEUE],
                             traffic.stat_hold[GTA_HOLD_LIGHT],
                             traffic.stat_hold[GTA_HOLD_BOX],
                             traffic.stat_hold[GTA_HOLD_MERGE],
                             traffic.stat_hold[GTA_HOLD_DEADEND],
                             traffic.stat_hold[GTA_HOLD_ROAD],
                             traffic.stat_hold[GTA_HOLD_GAP]);
                    log_line(ln);
                    {
                        unsigned long tot = traffic.prof_us[0]
                            + traffic.prof_us[1] + traffic.prof_us[2]
                            + traffic.prof_us[3] + traffic.prof_us[4];
                        snprintf(ln, sizeof ln,
                                 "gta: tickprof %lu us/tick - release %lu, "
                                 "occ %lu, drive %lu, route %lu, spawn %lu",
                                 tot / 250UL,
                                 traffic.prof_us[0] / 250UL,
                                 traffic.prof_us[1] / 250UL,
                                 traffic.prof_us[2] / 250UL,
                                 traffic.prof_us[3] / 250UL,
                                 traffic.prof_us[4] / 250UL);
                        log_line(ln);
                        {
                            int pk;
                            for (pk = 0; pk < 5; pk++)
                                traffic.prof_us[pk] = 0;
                        }
                    }
                    /* THE TWO FAULTS THE HOST TEST CANNOT SEE FROM HERE.
                     *
                     * The drive test holds its camera still for the whole run;
                     * the game's never stops moving, and the reports that keep
                     * coming back - cars on the pavement, cars standing on it
                     * and then vanishing - are about the ground under a car
                     * near the player. So the running game counts them itself,
                     * and this line is the evidence for or against. */
                    snprintf(ln, sizeof ln,
                             "gta: traffic pavement %ld excursions (%ld "
                             "car-ticks, %ld deep), last at (%d,%d), "
                             "recovered %ld, abandoned %ld",
                             traffic.stat_offroad_events, traffic.stat_offroad,
                             traffic.stat_offroad_deep,
                             traffic.stat_offroad_x, traffic.stat_offroad_y,
                             traffic.stat_offroad_recovered,
                             traffic.stat_abandoned);
                    log_line(ln);
                }

                /* AND WHETHER CARS LEAVE JUNCTIONS ON THE LINE THEY CAME IN
                 * ON, every ten seconds. This is the developer's own
                 * instrument and it is here as well as in the host sweep
                 * because the report that produced it was made from the
                 * emulator screen, not from a test. Straight-through and
                 * turning are counted apart: a car going straight must not
                 * move at all, a turning one is judged against the centre of
                 * the lane it joins. */
                if ((sim_ticks % 500) == 0) {
                    char cl[160];
                    long st = traffic.stat_cross_straight[0]
                            + traffic.stat_cross_straight[1]
                            + traffic.stat_cross_straight[2]
                            + traffic.stat_cross_straight[3];
                    long tu = traffic.stat_cross_turned[0]
                            + traffic.stat_cross_turned[1]
                            + traffic.stat_cross_turned[2]
                            + traffic.stat_cross_turned[3];
                    snprintf(cl, sizeof cl,
                             "gta: junctions - straight %ld (%ld%% on line, "
                             "%ld changed lane), turned %ld (%ld%% on line)",
                             st, st ? traffic.stat_cross_straight[0] * 100 / st : 0,
                             traffic.stat_cross_straight[3],
                             tu, tu ? traffic.stat_cross_turned[0] * 100 / tu : 0);
                    log_line(cl);

                    /* AND THE DEADLOCK, which is the one the developer can see
                     * from the pavement: three or more cars stopped inside one
                     * crossing, blocking each other. Anything but zero here is
                     * the fault being reported, and it is printed even when it
                     * is zero so that a quiet log means "measured and clear"
                     * rather than "nobody looked". */
                    snprintf(cl, sizeof cl,
                             "gta: box deadlock - %ld car-ticks, worst %d cars "
                             "at (%d,%d)",
                             traffic.stat_boxlock, traffic.stat_boxlock_worst,
                             traffic.stat_boxlock_x, traffic.stat_boxlock_y);
                    log_line(cl);

                    /* AND THE RADIUS THE CORNERS ACTUALLY GET, because
                     * GTA_TURN_RADIUS is a ceiling rather than a radius and the
                     * host says nothing reaches it. Printed on the target so
                     * the two can be compared - this port has had host and
                     * Amiga disagree before, and the whole lookahead question
                     * rests on this number. */
                    if (traffic.stat_aim_r_n)
                        snprintf(cl, sizeof cl,
                                 "gta: turn radius - %ld px average of a %d "
                                 "ceiling over %ld turns, %ld%% reach it; "
                                 "%ld ticks a corner",
                                 traffic.stat_aim_r_sum / traffic.stat_aim_r_n,
                                 GTA_TURN_RADIUS, traffic.stat_aim_r_n,
                                 traffic.stat_aim_r_capped * 100
                                     / traffic.stat_aim_r_n,
                                 traffic.stat_turn_ticks_n
                                     ? traffic.stat_turn_ticks_sum
                                       / traffic.stat_turn_ticks_n : 0);
                    else
                        snprintf(cl, sizeof cl, "gta: turn radius - no turns yet");
                    log_line(cl);
                }

                /* AND A PICTURE TO GO WITH THE NUMBERS, every ten seconds.
                 *
                 * The traffic report exists because two screenshots of the
                 * emulator a minute apart disagreed with every host test. It
                 * settles "is the fleet moving"; it cannot settle "is that car
                 * in the right lane", which is the other half of what gets
                 * reported. SPACE dumps a frame, but host input synthesis is
                 * banned and the autoinput script has run out long before the
                 * interesting part - a jam takes 30 to 120 seconds to form -
                 * so nobody can press it during an unattended run.
                 *
                 * This is the same dump on a timer, so the newest
                 * frame_live.raw is always the city as it looks NOW.
                 * Interactive only, so it cannot touch the benchmarks, and
                 * once every ten seconds so the write cannot matter. */
                if ((sim_ticks % 500) == 0)
                    dump_frame(GTA_DIR "frame_live.raw", chunky, pitch,
                               SCREEN_W, SCREEN_H, tiles.palette);
            }
            /* `now` was read before any of the simulation ran, so this covers
             * the player, the traffic and the periodic reports together. */
            prof_sim_us += amiga_uclock_us() - now;
            prof_ticks  += ticks;
        }

        if (walk_mode) {
            view.cam_x = player.x;
            view.cam_y = player.y;
        } else {
            speed = fast ? SCROLL_FAST : SCROLL_SLOW;
            if (up)    dy -= speed;
            if (down)  dy += speed;
            if (left)  dx -= speed;
            if (right) dx += speed;
            if (dx || dy)
                gta_render_move(&view, dx, dy);
        }

        /* The player is queued every frame, in both modes - in camera mode he
         * stays where he was left, which is what makes "walk there, then look
         * at it from above" possible. */
        gta_peds_draw(&peds, &view);
        if (in_car) {
            const gta_car_info *pi = &tiles.cars[veh.model];
            if (pi->sprite_index >= 0)
                gta_render_add_sprite_r(&view, veh.ox, veh.oy, player.layer,
                                      player.layer, pi->sprite_index,
                                      (gta_veh_angle(&veh)
                                       + GTA_SPRITE_ART_SOUTH) & 255,
                                      veh.remap >= 0
                                          && veh.remap < GTA_CAR_REMAPS
                                          ? (int)pi->remap8[veh.remap] : 0);
        } else
        gta_render_add_sprite(&view, player.x, player.y, player.layer,
                              gta_player_grid(&player),
                              gta_player_sprite(&player),
                              gta_player_draw_angle(&player));

        /* Cars after the player, so that if the sprite list ever fills the
         * thing that gets dropped is a parked car and not the man the
         * keyboard is driving. */
        if (opt_traffic)
            gta_traffic_draw(&traffic, &view);

        /* Redraw every pass, moving or not. Holding the frame when nothing has
         * changed would be free frames per second and a dishonest measurement;
         * the place to earn that back is Phase 7, with a dirty-rectangle
         * scheme that is measured rather than assumed. */
        {
            unsigned long pa = amiga_uclock_us();
            unsigned long c0 = bench_blit_us, pb;

            mode_apply(&view);
            gta_render_frame(&view);
            pb = amiga_uclock_us();
            present_frame(&view, &player, walk_mode);
            prof_ren_us += pb - pa;
            prof_pre_us += amiga_uclock_us() - pb;
            prof_c2p_us += bench_blit_us - c0;
        }
        frames++;

        /* WHERE THE FRAME ACTUALLY GOES, every PROF_FRAMES frames.
         *
         * `sim` is the whole simulation for the window and `us/tick` is what
         * one 50 Hz tick costs; `ticks/f` is how many of them a frame is
         * paying for, which RISES as the frame rate falls (MAX_CATCHUP is 8),
         * so a slow frame makes itself slower. That feedback is why this line
         * prints the two separately instead of one simulation percentage. */
        if (++prof_frames >= PROF_FRAMES) {
            unsigned long win = amiga_uclock_us() - prof_t0;
            /* fps * 10 as an integer - 100 frames * 10^7 stays inside a
             * 32-bit unsigned, and no float ever reaches the ROM. */
            unsigned long fps10 = win
                ? (unsigned long)prof_frames * 10000000UL / win : 0UL;
            char pl[192];

            snprintf(pl, sizeof pl,
                     "gta: profile %ld frames %lu us = %lu.%lu fps | "
                     "sim %lu us/f (%ld.%02ld ticks/f, %lu us/tick) | "
                     "render %lu | present %lu (c2p %lu) | cols %ld walls %ld "
                     "spr %ld | w%d z%d",
                     (long)prof_frames, win, fps10 / 10UL, fps10 % 10UL,
                     prof_sim_us / (unsigned long)prof_frames,
                     prof_ticks / prof_frames,
                     (prof_ticks * 100 / prof_frames) % 100,
                     prof_ticks ? prof_sim_us / (unsigned long)prof_ticks : 0UL,
                     prof_ren_us / (unsigned long)prof_frames,
                     prof_pre_us / (unsigned long)prof_frames,
                     prof_c2p_us / (unsigned long)prof_frames,
                     (long)view.columns_visited, (long)view.walls_drawn,
                     (long)view.sprites_drawn, render_w(), zoom_display);
            log_line(pl);
            prof_frames = 0; prof_ticks = 0;
            prof_sim_us = prof_ren_us = prof_pre_us = prof_c2p_us = 0;
            prof_t0 = amiga_uclock_us();
        }

        /* The cap. A busy-wait on the microsecond clock, and deliberately
         * AFTER the blit so a capped frame and an uncapped one differ only in
         * where the spare time goes. The unsigned subtraction is wrap-safe,
         * which matters because amiga_uclock_us() is 32 bits and turns over
         * every 71 minutes - a session longer than that would otherwise
         * freeze here for the rest of the wrap. */
        if (frame_cap) {
            while ((unsigned long)(amiga_uclock_us() - frame_t0)
                       < (unsigned long)FRAME_CAP_US)
                ;
        }
        frame_t0 = amiga_uclock_us();
    }

    t1 = amiga_uclock_us();
    if (frames > 0)
        log_fps("gta: interactive", frames, t1 - t0);

    printf("gta: lid cache - %ld tiles scaled, %lu of %lu bytes, %ld overflow\n",
           view.lc_fills, view.lc_used,
           (unsigned long)GTA_LIDCACHE_BYTES, view.lc_full);
    fflush(stdout);

    amigagfx_close();
    gta_render_free(&view);
    gta_map_free(&map);
    gta_tiles_free(&tiles);
    log_line("gta: clean exit");
    return 0;
}
