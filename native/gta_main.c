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
#include "amiga_watchdog.h"
#include "gta_map.h"
#include "gta_tiles.h"
#include "gta_render.h"
#include "gta_hud.h"
#include "gta_player.h"
#include "gta_traffic.h"
#include "gta_vehphys.h"
#include "gta_peds.h"
#include "gta_weapon.h"
#include "gta_score.h"
#include "gta_pickup.h"
#include "gta_font.h"
#include "gta_text.h"
#include "gta_prefs.h"
#include "gta_sfx.h"

/* THE SCREEN, AND WHY IT IS A RUNTIME CHOICE AGAIN.
 *
 * 320x200 is what an AGA screen gives and what every measurement in the notes
 * was taken at. RTG can do better, and a player should get the screen their
 * machine can show.
 *
 * Up to v0.0.3 that meant THREE BINARIES - gta-aga, gta-rtg240, gta-rtg480 -
 * differing in three -D flags on this one file and in nothing else. That was
 * three copies of the same game in the archive and a choice the player had to
 * make by picking an icon, with no way to tell from the names which one their
 * machine wanted. Since gtaprefs exists and already chooses the display path,
 * it chooses the size too, and there is one binary:
 *
 *   Auto      320x200, or 320x240 when the gfx setting says RTG
 *   320x200   the reference; every timing in the notes
 *   320x240   the same picture, more of the city on screen
 *   640x480   rendered at 320x240 and doubled on the way out - a 68020
 *             cannot rasterise 640x480 at a playable rate
 *
 * The defines below are what the game opens with when there is no gta.prefs
 * at all, and they are still overridable at build time for a one-off A/B
 * measurement. `backend.txt` still overrides the backend the same way.
 *
 * WHAT THE RENDERER DRAWS IS NOT ALWAYS WHAT THE SCREEN SHOWS. With the
 * doubling on, the renderer works into g_render_buf at half the screen in
 * each axis. Everything upstream - renderer, HUD, frame dumps, every timing
 * in the notes - works in SCREEN_W/SCREEN_H, which are the RENDERED size, so
 * none of it has to know. */
#ifndef GTA_SCREEN_W
#define GTA_SCREEN_W 320
#endif
#ifndef GTA_SCREEN_H
#define GTA_SCREEN_H 200
#endif
#ifndef GTA_DEFAULT_BACKEND
#define GTA_DEFAULT_BACKEND AMIGAGFX_BACKEND_AGA
#endif

/* THE RENDERED SIZE. Decided once at start-up from the settings and never
 * changed afterwards; every SCREEN_W / SCREEN_H below is one of these.
 *
 * 640x480 comes in two shapes and they are NOT the same picture:
 *
 *   native   the renderer really rasterises 640x480. One stored art pixel
 *            to one screen pixel, twice as much of the city on screen, four
 *            times the rasterising.
 *   doubled  the renderer draws 320x240 and scale2x_rows() doubles it. The
 *            same picture as 320x240, four times the area, almost free.
 *
 * The doubled one was the only one v0.0.3 had, and drawing lores into a hires
 * screen and calling it 640x480 is a fair thing to object to - which is what
 * happened. Both are offered now and the setting says which is which. */
#define RENDER_MAX_W 640
#define RENDER_MAX_H 480

#ifdef GTA_SCALE2X
static int g_render_w = GTA_SCREEN_W / 2;
static int g_render_h = GTA_SCREEN_H / 2;
#else
static int g_render_w = GTA_SCREEN_W;
static int g_render_h = GTA_SCREEN_H;
#endif
#define SCREEN_W g_render_w
#define SCREEN_H g_render_h

/* The DISPLAY: what amigagfx_open() is asked for. Equal to the rendered size
 * unless g_scale2x, when it is twice it in each axis. */
static int g_screen_w = GTA_SCREEN_W;
static int g_screen_h = GTA_SCREEN_H;
#ifdef GTA_SCALE2X
static int g_scale2x = 1;
#else
static int g_scale2x = 0;
#endif

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
#define INI_PATH   GTA_DIR "GTADATA/mission.ini"
#define FXT_PATH   GTA_DIR "GTADATA/english.fxt"
#define FONT_PAGER GTA_DIR "GTADATA/pager1.fon"
#define FONT_SCORE GTA_DIR "GTADATA/score1.fon"
#define FONT_BIG   GTA_DIR "GTADATA/big1.fon"
/* THE SOUND BANK IS OPTIONAL, and that is not laziness.
 *
 * Nothing plays it yet, the shipped archive has no game data at all, and a
 * player who has only converted the art must still get a running game rather
 * than an error about a file they were never told to make. Absent means
 * silent. When there is a player, it will still mean silent. */
#define SFX_PATH   GTA_DIR "GTADATA/level001.snd"

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
#define KEY_CTRL   0x63     /* fire - the original's Left Ctrl, a latch */
#define KEY_X      0x32     /* next weapon */
#define KEY_Z      0x31     /* previous weapon */

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
 *   selftest 0|1  close and reopen the screen once at startup (default 0)
 */
/* The reservation overlay is DEACTIVATED by default since 2026-08-26 - the
 * developer's call once traffic was accepted ("zdezaktywuj je, nie wywalaj
 * bo moze jeszcze sie przydadza"). The code stays; `overlay 1` in
 * opts.txt brings it back for the next traffic investigation. */
static int opt_overlay = 0;
static int opt_traffic = 1;
static int opt_fleet   = -1;
static int opt_lights  = -1;    /* -1 = the module's default (on) */
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
/* THE SCREEN HEIGHT, for an unattended A/B between the three sizes that used
 * to be three separate binaries. 200, 240 or 480 (which means 640x480 with
 * the picture doubled); 0 leaves whatever gta.prefs asked for.
 *
 * It is here and not only in gta.prefs because the rig must be able to switch
 * screen size the way it switches everything else - by writing one line into
 * opts.txt - without driving an Intuition GUI from inside the emulator. */
static int opt_screen  = 0;
/* With `screen 480`, whether the picture is DOUBLED (render 320x240, scale2x
 * on the way out) or RASTERISED at 640x480. Two different pictures and two
 * very different costs - see the note on RENDER_MAX_W. */
static int opt_screen2x = 0;
/* THE STARTUP SELF-TEST IS OFF FOR PLAYERS AND ON FOR THE TEST RIG.
 *
 * It closes and reopens the screen twice, immediately after the first frame,
 * to prove the F3 path rebinds its pointers (see the self-test itself for why
 * that check is worth having). But a full screen teardown and rebuild before
 * the player has done anything is a liability on any system where reopening a
 * display is not the cheap, well-trodden operation it is on 68k AmigaOS -
 * reported from MorphOS as "draws one frame and dies", which is exactly where
 * this sits in the startup order.
 *
 * So it is opt-in. `deploy.sh` writes `selftest 1` into the emulator's
 * opts.txt, so every unattended run still exercises it and the regression
 * cover is unchanged; a shipped archive has no opts.txt and skips it. */
static int opt_selftest = 0;

/* WHAT THE PLAYER CHOSE FOR SOUND, read from gta.prefs by gtaprefs.
 *
 * NOTHING PLAYS YET. There is no audio in this port: amiga_audio.c is in the
 * tree, carried over from openttd_amiga_68k, and build.sh does not compile it.
 * The setting is read and reported anyway, because the choice has to be
 * settled before a sound layer is written and not after - the reason it
 * exists is that Paula is unreachable on MorphOS, and that constraint belongs
 * in the design of the audio layer rather than being discovered by it.
 *
 * Whoever adds sound: this is the variable to branch on. GTA_AUDIO_AUTO means
 * Paula where the chipset is real and AHI where it is not; the test for "real
 * chipset" is GfxBase->ChipRevBits0 & GFXF_AA_ALICE, which tools/gtaprefs.c
 * already does in have_aga(). */
static int opt_audio = GTA_AUDIO_AUTO;

static gta_traffic traffic;
static gta_peds peds;

/* The pedestrians ask the traffic module about the lights through this. */
static int ped_light_green(void *ctx, int bx, int by, int along_x)
{
    return gta_traffic_light_green((const gta_traffic *)ctx, bx, by, along_x);
}
static gta_pickups pickups;

/* THE ORIGINAL'S FONTS AND TEXTS (Phase 5 item 7, the first piece). The
 * pager font draws the briefs along the bottom, the score font the score,
 * the big font the cards. Each is optional: when a file is missing the
 * port's own 3x5 font draws that part, as before. */
static gta_font pager_font, score_font, big_font;
static int have_pager, have_score_font, have_big;
static gta_text texts;
/* THE PAGER: one line of text along the bottom, shown for pager_ticks
 * ticks. The original scrolls the brief through a pager; this shows as
 * much of it as fits, then the rest, a page every PAGER_PAGE_TICKS. */
static char pager_text[400];
static int  pager_ticks, pager_page;
#define PAGER_PAGE_TICKS 200

static void pager_show(const char *s)
{
    int i;
    for (i = 0; s[i] && i < (int)sizeof pager_text - 1; i++)
        pager_text[i] = s[i];
    pager_text[i] = 0;
    pager_ticks = 1;
    pager_page = 0;
}

/* The brief with numeric key `key` from the texts, if there is one. */
static void pager_brief(int key)
{
    const char *s = gta_text_get(&texts, key);
    if (s) pager_show(s);
    else printf("gta: pager - no text %d\n", key);
}
static int jail_free;           /* the get-out-of-jail-free card */

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
/* Set when Work:reload.txt was seen - see the poll in the tick loop. */
static int g_reload;
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
#define GTA_VERSION "v0.2.0"
#define GAME_TITLE  "AmiGTA 68K " GTA_VERSION

/* The renderer's own buffer, used ONLY when the picture is doubled: the
 * screen's chunky bitmap is then twice this in each axis and is written by
 * nothing but scale2x_rows() below. Native 640x480 does not come through
 * here at all - it renders straight into the screen, like every other size.
 *
 * 320x240 is therefore the largest thing that can land in it, and it is
 * allocated whether or not the doubling is on: the choice is made at run time
 * now, and 76 KB of BSS is cheaper than the malloc-and-check it would
 * otherwise need. BSS, not initialised data, so it costs nothing in the
 * executable. */
#define DOUBLED_MAX_W 320
#define DOUBLED_MAX_H 240
static unsigned char g_render_buf[DOUBLED_MAX_W * DOUBLED_MAX_H];

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

    if (amigagfx_open(g_screen_w, g_screen_h, show_bar,
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
    if (g_scale2x) {
        /* The renderer never touches the screen when the picture is doubled. */
        g_chunky = g_render_buf;
        g_pitch  = SCREEN_W;
    }
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

/* Half-resolution (F2). Sized for the tallest render, like g_render_buf and
 * for the same reason: the height is not known until the settings are read. */
static unsigned char low_buffer[(RENDER_MAX_W / 2) * (RENDER_MAX_H / 2)];

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
/* FOUR FIFTHS OF THE WIDTH, ROUNDED DOWN TO THE 32-PIXEL GRID, and centred on
 * it. 320 gives 256 at x=32; 640 gives 512 at x=64. Both are multiples of 32
 * in both the width and the offset, which is the whole point - anything else
 * gets snapped outwards by amigagfx_blit() and converts the black bars too.
 *
 * NOT a constant any more, because the rendered width is a setting now. It is
 * filled in by view_modes_init() before anything reads it. */
static struct {
    int w, x;
    const char *name;
} view_modes[] = {
    { 320, 0,  "full width"            },
    { 256, 32, "5:4 (c2p-aligned)"     }
};

static void view_modes_init(void)
{
    int fast = (SCREEN_W * 4 / 5) & ~31;
    if (fast < 32) fast = 32;
    view_modes[0].w = SCREEN_W;
    view_modes[0].x = 0;
    view_modes[1].w = fast;
    view_modes[1].x = ((SCREEN_W - fast) / 2) & ~31;
}

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
static gta_score   score;
/* WHAT THE CORNER SHOWS. The weapon and its ammunition live in the main
 * loop, where the keyboard is; the readout is drawn from a function that
 * runs in five other places as well (the tour, the benchmark), so the two
 * numbers are mirrored here rather than passed down through all of them. */
static int hud_weapon;
static int hud_ammo;
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

/* THE SCORE AND THE GUN, in the top right corner.
 *
 * The original puts the score there - nine digits, right-aligned, rolling
 * like an odometer - with the multiplier and the lives under it and the
 * wanted level's flashing cop heads across the top middle. This is the same
 * corner and the same right alignment, in the port's own 3x5 font: the score
 * on the first line, the weapon in hand and its ammunition on the second.
 * The roll, the multiplier and the heads arrive with the wanted level.
 *
 * RIGHT-ALIGNED, which is why gta_hud_width() exists: a left-aligned score
 * slides sideways every time it gains a digit, and the eye reads that as the
 * whole readout moving. */
static const char *const weapon_name[5] = {
    "FIST", "PISTOL", "MG", "ROCKET", "FLAME"
};

/* BUSTED - the original's 50-frame card. While it stands the player has no
 * control; when it ends he is put down outside the nearest police station
 * on foot, with his weapons gone and the multiplier halved. */
static int bust_timer;
#define BUST_TICKS 150
/* WHAT THE CARD SAYS: 1 BUSTED, 2 WASTED, 3 GAME OVER. */
static int card_kind;
static const char *const card_text[4] = { "", "BUSTED", "WASTED", "GAME OVER" };

/* THE PLAYER'S LIFE. Health 100, four lives at the start as the original
 * gives; armour is the pickup's three hits; a burning player loses a point
 * a tick for a hundred ticks. Nothing heals but the hospital. */
static int player_health = 100;
static int player_armour;
static int player_lives = 4;
static int player_burning;

static void hud_score(unsigned char *chunky, int pitch)
{
    char line[24];
    char *p;
    int right = present_x() + render_w() - 2;

    p = gta_hud_int(line, score.score);
    *p = 0;
    if (have_score_font) {
        /* score1.fon: the ten digits, '0' first - the font's characters
         * start at '!', so the digits are drawn through a copy offset by
         * '0' - '!'. */
        char digits[24];
        int k;
        for (k = 0; line[k]; k++) digits[k] = (char)(line[k] - '0' + '!');
        digits[k] = 0;
        gta_font_draw(&score_font, chunky, pitch, SCREEN_W, SCREEN_H,
                      right - gta_font_width(&score_font, digits), 1, digits);
    } else
    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                 right - gta_hud_width(line), 2, line);

    {
        const char *n = weapon_name[hud_weapon >= 0 && hud_weapon < 5
                                    ? hud_weapon : 0];
        const char *q;
        p = line;
        for (q = n; *q; q++) *p++ = *q;
        /* Fists have no ammunition, and a zero next to them reads as an
         * empty gun. */
        if (hud_weapon != 0) {
            *p++ = ' ';
            p = gta_hud_int(p, hud_ammo);
        }
        *p = 0;
    }
    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                 right - gta_hud_width(line), have_score_font ? 14 : 10, line);

    /* The third line: lives and health (ours - the original hides the
     * health and shows the lives with a small glyph). */
    p = line;
    *p++ = 'L'; *p++ = 'I'; *p++ = 'V'; *p++ = 'E'; *p++ = 'S'; *p++ = ' ';
    p = gta_hud_int(p, player_lives);
    *p++ = ' '; *p++ = 'H'; *p++ = 'P'; *p++ = ' ';
    p = gta_hud_int(p, player_health);
    if (player_armour > 0) { *p++ = ' '; *p++ = 'A'; p = gta_hud_int(p, player_armour); }
    *p = 0;
    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                 right - gta_hud_width(line), have_score_font ? 22 : 18, line);

    /* THE PAGER LINE, along the bottom, in the original's pager font. A
     * page is what fits in the width; the text advances a page every
     * PAGER_PAGE_TICKS and goes away after the last. */
    if (pager_ticks > 0 && pager_text[0]) {
        int pw = render_w() - 8;
        int start = 0, page = 0, len = 0, k;
        /* find the start of the current page by measuring words */
        while (pager_text[start] && page < pager_page) {
            int e = start, last = start;
            while (pager_text[e]) {
                int q = e;
                char save;
                while (pager_text[q] && pager_text[q] != ' ') q++;
                save = pager_text[q]; pager_text[q] = 0;
                if ((have_pager ? gta_font_width(&pager_font, pager_text + start)
                                : gta_hud_width(pager_text + start)) > pw) {
                    pager_text[q] = save;
                    break;
                }
                pager_text[q] = save;
                last = q;
                if (!pager_text[q]) { last = q; break; }
                e = q + 1;
            }
            if (last == start) break;
            start = last;
            while (pager_text[start] == ' ') start++;
            page++;
        }
        if (!pager_text[start]) {
            pager_ticks = 0;            /* past the end: gone */
        } else {
            char line2[200];
            int e = start, last = start;
            while (pager_text[e]) {
                int q = e;
                char save;
                while (pager_text[q] && pager_text[q] != ' ') q++;
                save = pager_text[q]; pager_text[q] = 0;
                if ((have_pager ? gta_font_width(&pager_font, pager_text + start)
                                : gta_hud_width(pager_text + start)) > pw) {
                    pager_text[q] = save;
                    break;
                }
                pager_text[q] = save;
                last = q;
                if (!pager_text[q]) break;
                e = q + 1;
            }
            len = last - start;
            if (len <= 0) len = 1;
            if (len > (int)sizeof line2 - 1) len = (int)sizeof line2 - 1;
            for (k = 0; k < len; k++) line2[k] = pager_text[start + k];
            line2[len] = 0;
            {
                int y = SCREEN_H - (have_pager ? pager_font.height : 7) - 3;
                int x = present_x() + 4;
                /* a dark plate under it, the height of the font */
                int by;
                for (by = y - 2; by < SCREEN_H; by++) {
                    unsigned char *d = chunky + (long)by * pitch;
                    int bx;
                    for (bx = present_x(); bx < present_x() + render_w(); bx++) d[bx] = 0;
                }
                if (have_pager)
                    gta_font_draw(&pager_font, chunky, pitch, SCREEN_W, SCREEN_H, x, y, line2);
                else
                    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H, x, y, line2);
            }
            if (++pager_ticks > PAGER_PAGE_TICKS) {
                pager_ticks = 1;
                pager_page++;
            }
        }
    }

    if (bust_timer > 0) {
        const char *t = card_text[card_kind & 3];
        int bw = have_big ? gta_font_width(&big_font, t) : gta_hud_width_big(t, 4);
        if (have_big)
            gta_font_draw(&big_font, chunky, pitch, SCREEN_W, SCREEN_H,
                          present_x() + (render_w() - bw) / 2,
                          SCREEN_H / 2 - big_font.height / 2, t);
        else
            gta_hud_text_big(chunky, pitch, SCREEN_W, SCREEN_H,
                             present_x() + (render_w() - bw) / 2,
                             SCREEN_H / 2 - 12, t, 4);
    }

    /* THE WANTED LEVEL: that many heads across the top middle, each one
     * flashing every other frame, as the original's are. Nothing at level
     * 0 - an empty row would be a readout of nothing. */
    if (score.level > 0) {
        static int flash;
        int n = score.level, k;
        int x = present_x() + render_w() / 2
              - (n * GTA_HUD_COP_W - 1) / 2;
        if ((++flash & 2) == 0)
            for (k = 0; k < n; k++)
                gta_hud_cop(chunky, pitch, SCREEN_W, SCREEN_H,
                            x + k * GTA_HUD_COP_W, 2);
    }
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

    /* WHERE HE IS, IN BLOCKS, FIRST.
     *
     * Added because a screenshot of something wrong is not a bug report
     * without it. Twice now a picture has arrived showing a car under a
     * bridge, and answering it meant guessing at which of Liberty City's
     * bridges from the shape of the girders. The map is 256x256 and every
     * question about the map - is there a ramp here, what is on the layer
     * above, which way does that slope go - starts with the block number. */
    q = gta_hud_int(line, (int)(p->x >> 21));
    *q++ = ',';
    q = gta_hud_int(q, (int)(p->y >> 21));
    *q++ = ' ';
    q = gta_hud_int(q, p->layer);
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
    hud_score(g_chunky, g_pitch);
    if (with_player)
        hud_player(pl, g_chunky, g_pitch);

    /* Convert the picture only. The bars are converted on the frame after a
     * mode change and never again - they are black and they stay black, and
     * c2p is the one part of the frame a narrower picture would otherwise not
     * make any cheaper. amigagfx_blit() snaps to the 32-pixel grid itself, so
     * a mode that is not on it simply gets its bars converted too. */
    {
        unsigned long tb = amiga_uclock_us();

        if (g_scale2x) {
            /* Double the whole picture into the screen, then blit all of it.
             * The narrow-blit optimisation does not apply here: the doubling
             * has already touched every byte, so there is nothing to save. */
            scale2x_rows(g_render_buf, SCREEN_W, amigagfx_chunky(),
                         amigagfx_pitch(), SCREEN_W, SCREEN_H);
            amigagfx_blit(0, 0, SCREEN_W * 2, SCREEN_H * 2);
            bars_dirty = 0;
        } else if (bars_dirty) {
            amigagfx_blit(0, 0, SCREEN_W, SCREEN_H);
            bars_dirty = 0;
        } else {
            amigagfx_blit(present_x(), 0, present_w(), SCREEN_H);
        }
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
/* Which numbered live frame comes next - see the `film` order in the
 * autodrive script. */
static int live_n = 0;

/* WHICH DOOR FRAME IS SHOWING, from a tick count.
 *
 * The door has its own clock at 10 frames a second - Carnage3D's
 * CAR_DELTA_ANIMS_SPEED - so at this port's 50 Hz simulation that is five
 * ticks a frame. Five frames out and the same five back: frame 0 is shut,
 * 1..4 are sprite delta records 6..9 (see gta_tiles.h for why those four and
 * not the car table's own field), then it closes through the same four.
 *
 * Fifty ticks in all against the get-in animation's forty, so the door is
 * still swinging shut when the player is already seated. That overlap is in
 * the original too - Carnage3D asks for the close as soon as the open
 * finishes, regardless of where the ped is up to. */
static int door_delta(int tick)
{
    int f;
    if (tick < 0) return -1;
    f = tick / 5;
    if (f <= 0 || f >= 8) return -1;        /* shut, at either end */
    if (f > 4) f = 8 - f;                   /* the closing half */
    return GTA_DELTA_DOOR1 + (f - 1);
}

/* ---- the two interpolators the get-in / get-out animation needs ---------
 *
 * 16.16 world coordinates in a 32-bit long. The naive a + (b-a)*t/T is safe at
 * the distances involved here (a block and a half is 3.1 million, times forty
 * ticks is still inside a long) but only just, and a longer animation or a
 * bigger grab radius would silently overflow it. Dividing first and carrying
 * the remainder separately costs one extra divide a tick and cannot. */
static long lerp_fp(long a, long b, int t, int T)
{
    long d;
    if (T <= 0 || t >= T) return b;
    if (t <= 0) return a;
    d = b - a;
    return a + (d / T) * t + ((d % T) * t) / T;
}

/* Angles are 0..255 and wrap, so interpolating them as plain numbers sends a
 * man turning from 250 to 6 the long way round - 244 units of spin instead of
 * 12. Take the signed short way. */
static int lerp_angle(int a, int b, int t, int T)
{
    int d;
    if (T <= 0 || t >= T) return b & 255;
    if (t <= 0) return a & 255;
    d = (b - a) & 255;
    if (d > 128) d -= 256;
    return (a + (d * t) / T) & 255;
}

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
/* A megabyte of samples. Static for the same reason the map is: it is memory
 * the linker accounts for rather than a surprise at the far end of a load. */
static gta_sfx   sfx;
static gta_weapons weapons;
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
        /* AND THE SIDE IS THE SIDE THE ART'S DOOR IS ON.
         *
         * The sign is INVERTED against the table's rpy, and that is not a
         * guess: the car sprite is drawn rotated by GTA_SPRITE_ART_SOUTH
         * because the art faces south, so the body's right in the picture is
         * the opposite of (rx,ry) here. With the sign taken literally the man
         * walked to the left flank while the door delta swung open on the
         * right - visible the moment the doors started animating
         * (PROGRESS.md 112, out/door_sheet.png).
         *
         * The original agrees with the ART, not with the raw sign: its in-car
         * steps put the ped at a POSITIVE lateral offset throughout
         * (half_wid-2 walking in, half_wid-{4,8,12,14} sliding across), so
         * the door art and the ped are on one side by construction. */
        long out = (long)gta_car_world_wid(ci) / 2 + 5;
        /* AND IT IS THE SAME SIDE FOR EVERY CAR. The table's rpy was read
         * as the side for months and it is not one: it is -6 on model 0
         * and +7 on model 1, for bodies 30 wide, so both hinges are INSIDE
         * the body a few pixels either side of the centre line - a walk-to
         * point, which is exactly what the original uses it for
         * (the original: car + cos[rot]*rpx + cos[rot+90]*rpy, sign and
         * all, and the ped overlaps the body while he walks up). Which
         * flank he then gets in from is the enter sequence's POSITIVE
         * lateral offset, the same on every model. Reading the sign as a
         * side put him at the passenger door of every car whose hinge
         * happened to sit left of centre - "ze zlej strony wsiadalismy" -
         * and PROGRESS.md 112 could not see it because it tested one
         * model (20, rpy +7). */
        (void)across;
        across = (ci->n_doors > 0) ? -out : out;
    }

    *dx = cx + (fx * along + rx * across) * 4;
    *dy = cy + (fy * along + ry * across) * 4;
}

/* WHICH FLANK THE DOOR IS ON, +1 for the body's right, -1 for its left.
 * Every car's door is on the same flank (car_door_point() says why the
 * table's rpy is not a side); a vehicle with no door record mounts from its
 * right. */
static int car_door_side(const gta_car_info *ci)
{
    /* One side for every car - see car_door_point(). -1 is the flank the
     * door art opens on (PROGRESS.md 112, model 20). */
    return ci->n_doors > 0 ? -1 : 1;
}

/* A POINT NEAR THE DOOR, in the car's own frame: `along_off` world px from
 * the hinge along the body, and `lat_off` px outside the body's edge on the
 * door side (negative = inside the body). This is how the original places
 * the ped through every state of the exit - car + cos[rot] * along +
 * cos[rot + 90] * lateral - and it is what those per-state tables need. */
static void car_door_pos(const gta_car_info *ci, long cx, long cy, int face,
                         long along_off, long lat_off, long *px, long *py)
{
    long fx = gta_sin(face), fy = -gta_cos(face);
    long rx = gta_cos(face), ry = gta_sin(face);
    long along  = (ci->n_doors > 0 ? (long)ci->doors[0].rpx / 2 : 0) + along_off;
    long across = car_door_side(ci)
                * ((long)gta_car_world_wid(ci) / 2 + lat_off);
    *px = cx + (fx * along + rx * across) * 4;
    *py = cy + (fy * along + ry * across) * 4;
}

/* THE EXIT, STATE BY STATE - the original's 0x11..0x19 (LEFTOFF.md, the
 * exit), nine states of GTA_EXIT_TICKS each,
 * sprite 16 + state. Per state: the door counter AFTER it (1..4 = delta
 * records 6..9, 0 = shut; it was stepped to 1 on the key press), and the
 * ped's offsets from the hinge in world px - the original's are in half
 * pixels: along -1 -> 0, -4 -> -2; lateral hw-4 -> hw-2, hw-3 -> hw-1,
 * hw+2 -> hw+1. The door is held open through states 3 and 4 while he
 * swings out, and closes over the last four while he stands beside it. */
static const signed char exit_door[GTA_PED_EXITCAR_FRAMES]  = { 2, 3, 4, 4, 4, 3, 2, 1, 0 };
static const signed char exit_along[GTA_PED_EXITCAR_FRAMES] = { 0, 0, 0, 0, 0, -2, -2, -2, -2 };
static const signed char exit_lat[GTA_PED_EXITCAR_FRAMES]   = { -2, -2, -2, -1, 1, 0, 0, 0, 1 };

/* ---- the vault's two questions about a car body ---------------------- */

/* Is the world point (px,py) inside the body of a car at (cx,cy) facing
 * `face`, half-length hl and half-width hw in world px, grown by `margin`
 * px all round? The original asks this with a 6x6-unit box 2 units ahead
 * of the ped every vault state ("is there still a car in front of me") and
 * with a 2x4 box 6 units ahead to fire the vault; a point with a margin is
 * the same test on a 68020 budget. */
static int car_body_hit(long cx, long cy, int face, int hl, int hw,
                        long px, long py, int margin)
{
    long fx = gta_sin(face), fy = -gta_cos(face);
    long rx = gta_cos(face), ry = gta_sin(face);
    long dx = (px - cx) >> 16, dy = (py - cy) >> 16;
    long along  = (dx * fx + dy * fy) >> 14;
    long across = (dx * rx + dy * ry) >> 14;
    if (along < 0)  along  = -along;
    if (across < 0) across = -across;
    return along <= hl + margin && across <= hw + margin;
}

/* The same question asked of every car in the fleet on the ped's layer.
 * Returns the fleet index or -1; `*low` says whether that vehicle is one
 * to leap over (GTA_VAULT_MAX_VERT) or one to slide under. */
static int fleet_car_at(const gta_traffic *tr, const gta_tiles *t,
                        long px, long py, int layer, int margin, int *low)
{
    int i;
    for (i = 0; i < tr->n; i++) {
        const gta_car *c = &tr->cars[i];
        const gta_car_info *ci;
        if (c->done || c->layer != layer)
            continue;
        ci = &t->cars[c->model];
        if (car_body_hit(c->x, c->y, c->face,
                         gta_car_world_len(ci) / 2, gta_car_world_wid(ci) / 2,
                         px, py, margin)) {
            if (low) *low = ci->vert < GTA_VAULT_MAX_VERT;
            return i;
        }
    }
    return -1;
}


/* THE ARMED LOOK, as the original draws it: while the fire
 * latch is held a standing player is drawn as 89 and a walking or running
 * one with the cycle's frame + 99; a running punch is the run frame + 0xad.
 * Only the sprite changes - the state machine knows nothing of it. */
static int armed_sprite(const gta_player *p, int punch_left, int armed)
{
    int s = gta_player_sprite(p);
    if (punch_left > 0 && p->anim == GTA_ANIM_RUN)
        return s + GTA_PED_RUNPUNCH_OFFSET;
    if (!armed)
        return s;
    if (p->anim == GTA_ANIM_WALK || p->anim == GTA_ANIM_RUN)
        return s + GTA_PED_PISTOL_OFFSET;
    if (p->anim == GTA_ANIM_STAND)
        return p->ped_base + GTA_PED_SHOOT_STAND;
    return s;
}
/* ...and when it applies: the latch held, a gun selected, on foot. */
#define ARMED_NOW (fire_held && weapon != 0 && !in_car && !enter_anim \
                   && !vault && !slide)

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
    int enter_cop = 0;              /* the car being entered is a cop car */
    long cops_killed_seen = 0;
    int enter_model = 0, enter_face = 0, enter_remap = -1, enter_damage = 0;
    /* A BIKE IS MOUNTED, NOT ENTERED: four frames, no door, and the rider
     * stays visible on top. Decided once from the vehicle class when the
     * animation starts, so the per-tick code does not re-read the table. */
    int enter_bike = 0;
    /* THE VAULT (LEFTOFF.md, the vault).
     *
     * 0 = none; 1 = part of getting in - he is on the wrong flank, runs at
     * the door, meets the body and goes over it; 2 = SPACE while running at
     * a car. Same six states either way, 4 ticks each, and every state
     * boundary asks "is there still a car one pixel ahead of me?" - the
     * first no lands him. vault_pending is the decision taken at RETURN,
     * waiting for the walk to reach the body; vault_dx/dy keep the real
     * door point while enter_dx/dy hold the point on the flank he runs at. */
    /* THE DRIVER TO BE DRAGGED OUT - remembered at RETURN, pulled when the
     * door is open, as the original's jacker state 0x1c does. */
    int  enter_driver = 0;
    int  vault = 0, vault_pending = 0, vault_step = 0, vault_tick = 0;
    int  vault_head = 0, vault_hold = 0;
    long vault_dx = 0, vault_dy = 0;
    /* And state 0x92: under a vehicle too tall to vault, sliding a pixel
     * every state while a car is still over him. */
    int  slide = 0, slide_tick = 0;
    int  jump_req = 0;
    /* THE WEAPONS - Phase 5 item 5(a). The fire key is a LATCH (held down)
     * and the cooldown meters the rate, as in the original; `weapon` 0 is
     * the fists, 1 the pistol; `ammo` per weapon. The pistol with a crate's
     * load is the start loadout until crates exist - the original starts
     * with fists and a crate nearby. `punch_left` counts the ticks of a
     * punch in flight (six states of GTA_PUNCH_TICKS). */
    /* Ticks the player's car was stopped short of another car by the
     * bisection rather than being pushed out of it afterwards. */
    long veh_contact_stops = 0;
    int  fire_held = 0, fire_cool = 0;
    int  weapon = GTA_WEAPON_PISTOL;
    /* THE START LOADOUT IS ALL FIVE, WITH A CRATE'S WORTH OF EACH, and that
     * is temporary: the original starts you with fists and leaves the guns
     * in crates around the city. Until the crates exist there would be no
     * way to reach the other four at all. `ammo_sub` is the five rounds a
     * machine gun or a flamethrower gets out of one unit. */
    int  ammo[GTA_WEAPON_COUNT] = { 0, GTA_AMMO_PISTOL, GTA_AMMO_MG,
                                    GTA_AMMO_ROCKET, GTA_AMMO_FLAME };
    int  ammo_sub[GTA_WEAPON_COUNT] = { 0, 0, GTA_AMMO_PER_UNIT, 0,
                                        GTA_AMMO_PER_UNIT };
    int  punch_left = 0;
    long enter_cx = 0, enter_cy = 0;
    /* THE THREE POINTS THE ANIMATION MOVES BETWEEN.
     *
     * He starts where he is standing, walks to the door handle, and ends in
     * the seat. Before 2026-09-01 he was TELEPORTED to the handle on the
     * RETURN tick and teleported again into the seat forty ticks later - two
     * camera jumps of up to a block and a half, with him standing motionless
     * in the road in between. The filmstrip is out/before_enter_sheet.png. */
    long enter_x0 = 0, enter_y0 = 0;    /* where he was standing */
    long enter_dx = 0, enter_dy = 0;    /* the door handle */
    int  enter_a0 = 0;                  /* the way he was facing */
    /* THE DOOR'S OWN CLOCK, or -1 when it is shut and staying shut. It runs
     * independently of the ten-step sequence because the door is a property
     * of the CAR, not of the man - see door_delta(). */
    int  door_tick = -1;
    /* THE APPROACH IS ITS OWN PHASE, before the ten-step sequence.
     *
     * All ten frames of gta_ped_enter_seq happen AT the car - 26 is reaching
     * for the handle, 25 is leaning into the doorway, 29..33 are legs over the
     * sill and down into the seat. None of them is a walk. So covering the
     * distance with them playing makes him glide sideways in a door-opening
     * pose. He walks first, on the ordinary walk cycle, and the sequence then
     * plays where it belongs. Length comes from the distance at walking pace,
     * so a car right next to him has almost no approach at all. */
    int  enter_walk_len = 0, enter_walk_t = 0;
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

    /* THE SOUND BANK - loaded, described, and then not used by anything.
     *
     * This is the data half of Phase 6 and it is deliberately landed on its
     * own: reading GTA's .SDT/.RAW pair and proving the bytes survive the trip
     * to the Amiga is a separate question from making Paula or AHI play them,
     * and mixing the two would leave no way to tell which half was wrong. What
     * plays it reads `opt_audio` - see gta_prefs.h.
     *
     * A missing bank is normal and silent, not an error: no archive ships game
     * data, and `gtabake -sfx` is a step a player has not been asked to take
     * until there is something to hear. */
    if (gta_sfx_load(SFX_PATH, &sfx) == 0)
        gta_sfx_describe(&sfx, stdout);
    else
        printf("sfx: no " SFX_PATH " - running silent (nothing plays yet "
               "in any case)\n");
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
    /* THE PLAYER'S OWN SETTINGS FIRST, then the override files on top.
     *
     * gta.prefs is what the external editor writes (tools/gtaprefs.c) and it
     * is the only one of the three a player is expected to have. backend.txt
     * and opts.txt stay exactly as they were and still win, because they are
     * the deliberate ones: the test rig writes them, every measurement in
     * PROGRESS.md was taken with them, and a settings file quietly overriding
     * a switch that was set for a measurement would invalidate the numbers.
     *
     * gtaprefs keeps backend.txt in step with what it saves, so the two
     * cannot contradict each other in a player's drawer - see
     * gta_prefs_save(). */
    {
        gta_prefs prefs;
        int had = gta_prefs_load(GTA_DIR, &prefs);
        opt_audio = prefs.audio;
        if (prefs.gfx == GTA_GFX_AGA)      backend = AMIGAGFX_BACKEND_AGA;
        else if (prefs.gfx == GTA_GFX_RTG) backend = AMIGAGFX_BACKEND_RTG;
        else if (prefs.gfx == GTA_GFX_WB)  backend = AMIGAGFX_BACKEND_WB;
        /* THE SCREEN SIZE, which used to be three separate binaries.
         *
         * Decided here, once, before anything has been opened or sized:
         * open_display() asks for g_screen_w/h, and every buffer downstream
         * is already dimensioned for the largest case. */
        gta_prefs_screen_size(prefs.screen, prefs.gfx,
                              &g_screen_w, &g_screen_h, &g_scale2x);
        g_render_w = g_scale2x ? g_screen_w / 2 : g_screen_w;
        g_render_h = g_scale2x ? g_screen_h / 2 : g_screen_h;
        printf("gta: prefs %s - audio %s, gfx %s, screen %s\n",
               had ? "read" : "(none, defaults)",
               gta_prefs_audio_name(prefs.audio),
               gta_prefs_gfx_name(prefs.gfx),
               gta_prefs_screen_name(prefs.screen));
        printf("gta: display %dx%d, rendering %dx%d%s\n",
               g_screen_w, g_screen_h, SCREEN_W, SCREEN_H,
               g_scale2x ? " and doubling it" : "");
        if (opt_audio != GTA_AUDIO_OFF)
            printf("gta: NO SOUND IS BUILT INTO THIS VERSION - the audio "
                   "setting is recorded, not used\n");
        fflush(stdout);
    }

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
                else if (strcmp(word, "lights") == 0)  opt_lights  = (int)val;
                else if (strcmp(word, "catchup") == 0) opt_catchup = (int)val;
                else if (strcmp(word, "benchframes") == 0) opt_benchf = (int)val;
                else if (strcmp(word, "width") == 0)   opt_width   = (int)val;
                else if (strcmp(word, "camh") == 0)    opt_camh    = (int)val;
                else if (strcmp(word, "screen") == 0)  opt_screen  = (int)val;
                else if (strcmp(word, "screen2x") == 0) opt_screen2x = (int)val;
                else if (strcmp(word, "selftest") == 0) opt_selftest = (int)val;
            }
            fclose(of);
        }
        if (opt_catchup < 1) opt_catchup = 1;
        if (opt_benchf < 1) opt_benchf = 1;
        /* The rig's screen-size override, applied on top of gta.prefs for
         * exactly the reason every other opts.txt switch wins: a measurement
         * was set up with it, and a settings file quietly changing the screen
         * under a measurement would invalidate the numbers. */
        if (opt_screen == 200 || opt_screen == 240 || opt_screen == 480) {
            g_screen_w = (opt_screen == 480) ? 640 : 320;
            g_screen_h = opt_screen;
            g_scale2x  = opt_screen2x ? 1 : 0;
            g_render_w = g_scale2x ? g_screen_w / 2 : g_screen_w;
            g_render_h = g_scale2x ? g_screen_h / 2 : g_screen_h;
            printf("gta: opts screen %d%s - display %dx%d, rendering %dx%d\n",
                   opt_screen, opt_screen2x ? " doubled" : "",
                   g_screen_w, g_screen_h, SCREEN_W, SCREEN_H);
        }
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
    /* The narrow-view table is derived from the rendered width, so it can only
     * be built once gta.prefs and opts.txt have both had their say. Before
     * this call view_modes[] still holds the 320-wide defaults, and nothing
     * reads it until the first frame. */
    view_modes_init();

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
    have_pager      = gta_font_load(&pager_font, FONT_PAGER, tiles.palette) == 0;
    have_score_font = gta_font_load(&score_font, FONT_SCORE, tiles.palette) == 0;
    have_big        = gta_font_load(&big_font,   FONT_BIG,   tiles.palette) == 0;
    gta_text_load(&texts, FXT_PATH);
    printf("gta: fonts - pager %s (%d px), score %s, big %s; %d texts\n",
           have_pager ? "yes" : "no", pager_font.height,
           have_score_font ? "yes" : "no", have_big ? "yes" : "no", texts.n);
    fflush(stdout);
    /* THE LEVEL'S OPENING BRIEF - the first MOBILE_BRIEF of the script,
     * 1001: "Answer the South Park phones to get jobs..." */
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
        gta_traffic_police_start(&traffic, &map);
        gta_traffic_lights_scan(&traffic, &map);
        printf("gta: map - %d districts, %d roadblock lists\n", map.n_districts,
               map.n_routes - map.n_police_routes);
        gta_peds_set_lights(&peds, ped_light_green, &traffic);
        if (opt_lights >= 0) traffic.opt_lights = opt_lights;
        /* THE CRATES, from the level script, and with them the original's
         * start: fists, and a crate nearby. Without the file the old
         * loadout stands (the pistol and a crate's worth of everything). */
        if (gta_pickups_load(&pickups, INI_PATH, 1, &nav, &tiles) > 0) {
            int k_;
            weapon = 0;
            for (k_ = 1; k_ < GTA_WEAPON_COUNT; k_++) ammo[k_] = 0;
            printf("gta: you start with your fists - the weapons are in the crates\n");
            fflush(stdout);
        }
        gta_peds_set_nav(&peds, &nav);
        gta_weapons_init(&weapons, &tiles);
        gta_weapons_set_pickups(&weapons, &pickups);
        gta_score_init(&score);
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
    if (!opt_selftest) {
        log_line("gta: self-test skipped (opts.txt `selftest 1` runs it)");
    } else {
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
            hud_score(chunky, pitch);
            tb = amiga_uclock_us();
            if (g_scale2x) {
                scale2x_rows(g_render_buf, SCREEN_W, amigagfx_chunky(),
                             amigagfx_pitch(), SCREEN_W, SCREEN_H);
                amigagfx_blit(0, 0, SCREEN_W * 2, SCREEN_H * 2);
            } else {
                amigagfx_blit(0, 0, SCREEN_W, SCREEN_H);
            }
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
            hud_score(chunky, pitch);
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
    hud_score(chunky, pitch);
    t0 = amiga_uclock_us();
    for (frames = 0; frames < opt_benchf; frames++) {
        gta_render_move(&view, SCROLL_SLOW, 0);
        gta_render_frame(&view);
        hud_draw(&view, chunky, pitch);
        hud_score(chunky, pitch);
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
    hud_score(chunky, pitch);
    t0 = amiga_uclock_us();
    for (frames = 0; frames < opt_benchf; frames++) {
        gta_render_move(&view, SCROLL_SLOW, 0);
        gta_render_frame(&view);
        hud_draw(&view, chunky, pitch);
        hud_score(chunky, pitch);
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
        hud_score(chunky, pitch);
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
        /* THE SIZE IS NOT IN THE NAME ANY MORE. It was - "full 320x200",
         * "half 160x100" - and the moment the screen became a setting those
         * strings started lying: a 640x480 run reported its numbers as
         * 320x200. The size is printed once, by the display line at start-up,
         * and every fps figure in a log belongs to whatever that line says. */
        static const struct { int flat, scale, camh; const char *name; } modes[6] = {
            { 0, 1, GTA_CAM_H,       "gta: mode 2.5D    full" },
            { 0, 2, GTA_CAM_H,       "gta: mode 2.5D    half" },
            { 0, 1, GTA_CAM_H_LIGHT, "gta: mode 2.5D-lt full" },
            { 0, 2, GTA_CAM_H_LIGHT, "gta: mode 2.5D-lt half" },
            { 1, 1, GTA_CAM_H,       "gta: mode flat-2D full" },
            { 1, 2, GTA_CAM_H,       "gta: mode flat-2D half" }
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
                } else if (sscanf(ln, "film %d", &t) == 1) {
                    /* A FILMSTRIP, not a snapshot. `dump` writes one frame to
                     * one name, so two dumps in a row leave only the second -
                     * which is useless for anything that happens OVER several
                     * ticks, and getting into a car is exactly that. `film 12`
                     * writes live00.raw..live11.raw, one a tick, and
                     * tools/bin/raw2png.py turns them into pictures you can
                     * look at side by side. */
                    adq[adq_n].op = 4; adq[adq_n].t = t < 1 ? 1 : t; adq_n++;
                } else if (sscanf(ln, "park %d %d %d %d %d",
                                  &a, &b, &c, &d, &t) == 5) {
                    /* ...and `park` with a fifth number: the car has a
                     * DRIVER in it (it is a fleet car that will set off, so
                     * `enter` straight after it). For the carjack. */
                    adq[adq_n].op = 8; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = b;
                    adq[adq_n].st = c; adq[adq_n].hb = d; adq_n++;
                } else if (sscanf(ln, "park %d %d %d %d",
                                  &a, &b, &c, &d) == 4) {
                    /* A TEST FIXTURE: park model `a` at (dx,dy) world px
                     * from the player, facing `d`, as an abandoned car in
                     * the fleet. The vault needs a car on a known flank at
                     * a known angle, and the fleet's own cars are wherever
                     * the seed put them. */
                    adq[adq_n].op = 5; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = b;
                    adq[adq_n].st = c; adq[adq_n].hb = d; adq_n++;
                } else if (sscanf(ln, "face %d", &a) == 1) {
                    adq[adq_n].op = 6; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq_n++;
                } else if (sscanf(ln, "fire %d", &t) == 1) {
                    /* Press the fire key: held until the next `wait`. */
                    adq[adq_n].op = 9; adq[adq_n].t = t < 1 ? 1 : t; adq_n++;
                } else if (sscanf(ln, "weapon %d %d", &a, &b) == 2) {
                    /* Select weapon a with b rounds. */
                    adq[adq_n].op = 10; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = b; adq_n++;
                } else if (sscanf(ln, "ped %d %d %d", &a, &b, &c) == 3) {
                    /* A TEST FIXTURE, the twin of `park`: put somebody at
                     * (dx,dy) world px from the player, facing `c`. A jet of
                     * flame eight pixels wide fired at a city that spawns its
                     * people at random hits nobody for a hundred ticks at a
                     * time, which proves nothing either way. */
                    adq[adq_n].op = 11; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = b;
                    adq[adq_n].st = c; adq_n++;
                } else if (sscanf(ln, "damage %d", &a) == 1) {
                    /* A TEST FIXTURE: put `a` points of damage on the car the
                     * player is in. Wrecking one honestly takes a dozen
                     * crashes at speed, and a script cannot drive like that;
                     * leaning on a wall costs nothing on purpose. */
                    adq[adq_n].op = 12; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq_n++;
                } else if (sscanf(ln, "brief %d", &a) == 1) {
                    /* A TEST FIXTURE: show text `a` on the pager. */
                    adq[adq_n].op = 16; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq_n++;
                } else if (sscanf(ln, "hurt %d", &a) == 1) {
                    /* A TEST FIXTURE: take `a` points of health. */
                    adq[adq_n].op = 15; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq_n++;
                } else if (sscanf(ln, "crate %d %d %d %d", &a, &b, &c, &d) == 4) {
                    /* A TEST FIXTURE: a crate of kind c with d in it at
                     * (dx,dy) from the player. */
                    adq[adq_n].op = 14; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = b;
                    adq[adq_n].st = c; adq[adq_n].hb = d; adq_n++;
                } else if (sscanf(ln, "copcar %d %d %d", &a, &b, &c) == 3) {
                    /* A TEST FIXTURE: a police car with its driver, on
                     * patrol, at (dx,dy) from the player facing c. For the
                     * carjack of a cop car and the lights. */
                    adq[adq_n].op = 13; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = b;
                    adq[adq_n].st = c; adq_n++;
                } else if (strncmp(ln, "jump", 4) == 0) {
                    adq[adq_n].op = 7; adq[adq_n].t = 1; adq_n++;
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
    amiga_watchdog_start();
    pager_brief(1001);          /* the opening brief, now that frames count */

    while (running) {
        int dx = 0, dy = 0, speed;

        amiga_wd_tick();
        amiga_wd_set(AMIGA_WD_PHASE_INPUT);
        while (amigagfx_poll(&ev)) {
            if (ev.type == AMIGAGFX_EV_QUIT) {
                running = 0;
            } else if (ev.type == AMIGAGFX_EV_KEY) {
                int code = ev.code & 0x7F;
                int held = (ev.code & 0x80) ? 0 : 1;
                /* WHILE AN AUTODRIVE SCRIPT RUNS THE KEYBOARD IS DEAD, bar
                 * ESC. The emulator window takes the focus when the harness
                 * starts it, and the developer is at the same keyboard
                 * writing to whoever is running the test: one RETURN from a
                 * chat message became "no car within reach" before the
                 * script had parked its car, and a TAB put the game in
                 * camera mode in the middle of a filmed wreck. A scripted
                 * run has to be a scripted run. */
                if (adq_i < adq_n && code != KEY_ESC)
                    continue;
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
                    /* On foot SPACE is the original's jump: running at a
                     * car, he goes over it or under it. With nothing ahead
                     * it still dumps the frame - the one way to get at the
                     * view a person is actually looking at, since host input
                     * synthesis is banned. Decided in the tick, where the
                     * fleet can be asked. */
                    if (!held)
                        jump_req = 1;
                    break;
                case KEY_RETURN:
                    if (!held)
                        enter_req = 1;
                    break;
                case KEY_CTRL:
                    /* The original's latch: set on press, cleared on
                     * release; holding it auto-fires at the cooldown. */
                    fire_held = held;
                    break;
                case KEY_X:
                case KEY_Z:
                    if (!held) {
                        /* fist -> pistol -> MG -> rocket -> flame -> fist,
                         * skipping empties; Z the other way round. */
                        int w = weapon, k;
                        for (k = 0; k < 5; k++) {
                            w = code == KEY_X ? (w + 1) % 5 : (w + 4) % 5;
                            if (w == 0 || ammo[w] > 0) break;
                        }
                        weapon = w;
                        printf("gta: weapon %d (ammo %d)\n", weapon,
                               ammo[weapon]);
                    }
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

            amiga_wd_set(AMIGA_WD_PHASE_SIM);
            while (sim_accum >= (unsigned long)SIM_US && ticks < opt_catchup) {
                if (bust_timer > 0) {
                    up = down = left = right = 0;
                    handbrake = 0;
                    fire_held = 0;
                    enter_req = 0;
                }
                /* The autodrive queue stands in for the keyboard. */
                if (adq_i < adq_n) {
                    switch (adq[adq_i].op) {
                    case 0: up = down = left = right = 0; handbrake = 0;
                            fire_held = 0; break;
                    case 1: enter_req = 1; break;
                    case 9: fire_held = 1; break;
                    case 12:
                        if (in_car) {
                            veh.damage += adq[adq_i].thr;
                            printf("gta: your car is on %d points\n",
                                   veh.damage);
                        } else {
                            printf("gta: damage - not in a car\n");
                        }
                        fflush(stdout);
                        break;
                    case 11:
                        /* The pool is twelve and the city keeps it full, so
                         * the fixture makes room: the man farthest from the
                         * camera goes, and the new one takes his slot. */
                        {
                            int fi, worst = -1;
                            long worstd = -1;
                            for (fi = 0; fi < GTA_MAX_PEDS; fi++) {
                                long dx, dy, d;
                                if (!peds.p[fi].alive) { worst = -1; break; }
                                dx = (peds.p[fi].x - player.x) >> 16;
                                dy = (peds.p[fi].y - player.y) >> 16;
                                d = dx * dx + dy * dy;
                                if (d > worstd) { worstd = d; worst = fi; }
                            }
                            if (worst >= 0)
                                peds.p[worst].alive = 0;
                        }
                        if (gta_peds_drop(&peds,
                                player.x + ((long)adq[adq_i].thr << 16),
                                player.y + ((long)adq[adq_i].brk << 16),
                                player.layer, adq[adq_i].st & 255, -1, 0))
                            printf("gta: dropped a ped at (%ld,%ld)\n",
                                   (player.x >> 16) + adq[adq_i].thr,
                                   (player.y >> 16) + adq[adq_i].brk);
                        else
                            printf("gta: ped drop - pool full\n");
                        fflush(stdout);
                        break;
                    case 10:
                        if (adq[adq_i].thr >= 0 && adq[adq_i].thr < 5) {
                            weapon = adq[adq_i].thr;
                            ammo[weapon] = adq[adq_i].brk;
                        }
                        break;
                    case 5:
                        if (!gta_traffic_abandon(&traffic, adq[adq_i].thr,
                                player.x + ((long)adq[adq_i].brk << 16),
                                player.y + ((long)adq[adq_i].st << 16),
                                adq[adq_i].hb & 255, player.layer, -1, 0))
                            printf("gta: park - fleet full\n");
                        else
                            printf("gta: parked model %d at (%ld,%ld) facing %d\n",
                                   adq[adq_i].thr,
                                   (player.x >> 16) + adq[adq_i].brk,
                                   (player.y >> 16) + adq[adq_i].st,
                                   adq[adq_i].hb & 255);
                        fflush(stdout);
                        break;
                    case 6: player.angle = adq[adq_i].thr & 255; break;
                    case 7: jump_req = 1; break;
                    case 16:
                        pager_brief(adq[adq_i].thr);
                        break;
                    case 15:
                        player_health -= adq[adq_i].thr;
                        printf("gta: hurt fixture - health %d\n", player_health);
                        fflush(stdout);
                        break;
                    case 14:
                        if (gta_pickups_add(&pickups,
                                player.x + ((long)adq[adq_i].thr << 16),
                                player.y + ((long)adq[adq_i].brk << 16),
                                player.layer, adq[adq_i].st, adq[adq_i].hb))
                            printf("gta: crate fixture - kind %d x%d at (%ld,%ld)\n",
                                   adq[adq_i].st, adq[adq_i].hb,
                                   (player.x >> 16) + adq[adq_i].thr,
                                   (player.y >> 16) + adq[adq_i].brk);
                        fflush(stdout);
                        break;
                    case 13: {
                        int cm = gta_traffic_cop_model(&traffic);
                        if (cm < 0 || !gta_traffic_abandon(&traffic, cm,
                                player.x + ((long)adq[adq_i].thr << 16),
                                player.y + ((long)adq[adq_i].brk << 16),
                                adq[adq_i].st & 255, player.layer, 0, 0)) {
                            printf("gta: copcar - fleet full or no model\n");
                        } else {
                            int fi;
                            for (fi = 0; fi < traffic.n; fi++)
                                if (traffic.cars[fi].serial == traffic.next_serial) {
                                    traffic.cars[fi].abandoned = 0;
                                    traffic.cars[fi].cop = 1;
                                    traffic.cars[fi].top = 0;   /* stays put for the test */
                                    traffic.cars[fi].want_route = 1;
                                }
                            printf("gta: copcar parked WITH ITS COP at"
                                   " (%ld,%ld) facing %d\n",
                                   (player.x >> 16) + adq[adq_i].thr,
                                   (player.y >> 16) + adq[adq_i].brk,
                                   adq[adq_i].st & 255);
                        }
                        fflush(stdout);
                        break;
                    }
                    case 8:
                        if (!gta_traffic_abandon(&traffic, adq[adq_i].thr,
                                player.x + ((long)adq[adq_i].brk << 16),
                                player.y + ((long)adq[adq_i].st << 16),
                                adq[adq_i].hb & 255, player.layer, -1, 0)) {
                            printf("gta: park - fleet full\n");
                        } else {
                            /* abandon() takes the last slot when there is
                             * one and EVICTS a far car when the fleet is
                             * full, so find ours by its serial - it is the
                             * newest. */
                            int fi;
                            for (fi = 0; fi < traffic.n; fi++)
                                if (traffic.cars[fi].serial == traffic.next_serial)
                                    traffic.cars[fi].abandoned = 0;
                            printf("gta: parked model %d WITH A DRIVER at"
                                   " (%ld,%ld) facing %d\n", adq[adq_i].thr,
                                   (player.x >> 16) + adq[adq_i].brk,
                                   (player.y >> 16) + adq[adq_i].st,
                                   adq[adq_i].hb & 255);
                        }
                        fflush(stdout);
                        break;
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
                    case 4: {
                        /* One numbered frame a tick - see `film` in the
                         * parser. snprintf, never sprintf: on this libc
                         * sprintf shifts its arguments and would quietly
                         * write every frame to the same wrong name. */
                        char lp[64];
                        snprintf(lp, sizeof lp, GTA_DIR "live%02d.raw", live_n);
                        dump_frame(lp, chunky, pitch,
                                   SCREEN_W, SCREEN_H, tiles.palette);
                        /* THE NUMBERS NEXT TO THE PICTURE. Two figures in a
                         * frame and a car forty pixels off could not be told
                         * apart from the film alone (PROGRESS.md 113); the
                         * player's and the camera's world position in pixels,
                         * one line a frame, settles which sprite is whom. */
                        printf("gta: live%02d player (%ld,%ld) a%d %s%d cam (%ld,%ld)"
                               " veh (%ld,%ld) in_car %d anim %d fire %d w%d"
                               " bullets %d punch %d\n",
                               live_n, player.x >> 16, player.y >> 16,
                               player.angle,
                               enter_anim == 1 ? "enter" :
                               enter_anim == 2 ? "exit" : "frame",
                               enter_anim ? enter_step : player.frame,
                               view.cam_x >> 16, view.cam_y >> 16,
                               veh.ox >> 16, veh.oy >> 16, in_car, enter_anim,
                               fire_held, weapon, gta_weapons_alive(&weapons),
                               punch_left);
                        {
                            /* And where the fleet thinks the parked car is,
                             * since the film says it is not where it was
                             * left. */
                            int fi;
                            for (fi = 0; fi < traffic.n; fi++) {
                                const gta_car *fc = &traffic.cars[fi];
                                if (!fc->abandoned) continue;
                                printf("gta:   fleet[%d] abandoned model %d "
                                       "at (%ld,%ld) layer %d face %d done %d"
                                       " of %d\n", fi, fc->model,
                                       fc->x >> 16, fc->y >> 16, fc->layer,
                                       fc->face, fc->done, traffic.n);
                            }
                        }
                        if (live_n < 99) live_n++;
                        break;
                    }
                    }
                    if (--adq_left <= 0) {
                        adq_i++;
                        adq_left = adq_i < adq_n ? adq[adq_i].t : 0;
                        if (adq_i >= adq_n) {
                            up = down = left = right = 0; handbrake = 0;
                            fire_held = 0;
                            printf("gta: autodrive done\n");
                            fflush(stdout);
                        }
                    }
                }
                /* The door runs on its own clock and keeps running after the
                 * player is seated, so it can finish swinging shut. */
                if (door_tick >= 0 && ++door_tick > 50)
                    door_tick = -1;

                /* ENTERING AND LEAVING A CAR - handled inside the tick so a
                 * grab and the fleet's own compaction cannot interleave. */
                /* A RETURN PRESSED DURING THE ANIMATION IS DROPPED, not
                 * queued. The flag used to be cleared only when the guard
                 * below fired, so a second press while getting in stayed
                 * latched and went off on the first tick after the animation
                 * ended - he climbed in and straight back out. The autodrive
                 * script made that certain rather than merely likely, since
                 * its `enter` order re-sets the flag on every tick it lasts. */
                if (enter_req && (enter_anim || vault || slide))
                    enter_req = 0;
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

                            enter_cop = gta_traffic_last_grab_cop(&traffic);
                            car_door_point(ci_, cx_, cy_, f_, &dx_, &dy_);
                            /* THE WRONG FLANK: HE GOES OVER THE CAR.
                             *
                             * The original never decides this at RETURN.
                             * Its ped runs at the one door point; when the
                             * car body is 6 units ahead the walker measures
                             * the angle between the car's heading and the
                             * ped's bearing, and inside 0x258..0x3a0 of
                             * 0x400 - centred on the flank OPPOSITE the door,
                             * 59 degrees either way - with a low car, a
                             * running ped and landing room beyond, it sets
                             * the heading perpendicular to the car and
                             * state 0x73. Head-on it steers round instead.
                             *
                             * Here the same three facts are read off the
                             * geometry once: which flank he is on, whether
                             * his run at the door crosses the body, and at
                             * what angle. If it does, the walk goes to the
                             * point where it meets the far flank, and the
                             * vault takes over there. */
                            vault_pending = 0;
                            if (ci_->vtype != GTA_VEH_BIKE
                                && ci_->vert < GTA_VAULT_MAX_VERT) {
                                long fx = gta_sin(f_), fy = -gta_cos(f_);
                                long rx = gta_cos(f_), ry = gta_sin(f_);
                                long pdx = (player.x - cx_) >> 16;
                                long pdy = (player.y - cy_) >> 16;
                                long ddx = (dx_ - cx_) >> 16;
                                long ddy = (dy_ - cy_) >> 16;
                                long along_p  = (pdx * fx + pdy * fy) >> 14;
                                long across_p = (pdx * rx + pdy * ry) >> 14;
                                long along_d  = (ddx * fx + ddy * fy) >> 14;
                                long across_d = (ddx * rx + ddy * ry) >> 14;
                                long aap = across_p < 0 ? -across_p : across_p;
                                long alp = along_p < 0 ? -along_p : along_p;
                                int hl = gta_car_world_len(ci_) / 2;
                                int hw = gta_car_world_wid(ci_) / 2;
                                /* opposite flanks, outside the body, and
                                 * within 59 degrees of square-on: tan(59)
                                 * is 5/3 */
                                if (((across_p < 0) != (across_d < 0))
                                    && aap > hw && alp * 3 <= aap * 5) {
                                    long s = across_p < 0 ? -hw : hw;
                                    long num = across_p - s;
                                    long den = across_p - across_d;
                                    long along_hit = along_p
                                        + ((along_d - along_p) * num) / den;
                                    if (along_hit <= hl && along_hit >= -hl) {
                                        /* two pixels short of the flank */
                                        long so = across_p < 0 ? s - 2 : s + 2;
                                        vault_dx = dx_;
                                        vault_dy = dy_;
                                        dx_ = cx_ + (fx * along_hit + rx * so) * 4;
                                        dy_ = cy_ + (fy * along_hit + ry * so) * 4;
                                        vault_head = (across_d < 0 ? f_ - 64
                                                                   : f_ + 64)
                                                     & 255;
                                        vault_pending = 1;
                                        printf("gta: far side - will vault"
                                               " heading %d, over %d px\n",
                                               vault_head, 2 * hw + 4);
                                    }
                                }
                            }
                            /* HE WALKS THERE. The three points are recorded
                             * and the animation interpolates between them;
                             * nothing is teleported. Setting player.x here was
                             * the second half of the "he appears in the car"
                             * fault - the first half being that the car
                             * stopped being drawn at the same instant. */
                            enter_x0 = player.x;
                            enter_y0 = player.y;
                            enter_a0 = player.angle;
                            enter_dx = dx_;
                            enter_dy = dy_;
                            {
                                /* How long the walk takes, at the pace he
                                 * actually moves: the port runs at 2.03 blocks
                                 * a second, which is 65 world pixels a second
                                 * and 1.3 a tick at 50 Hz - so three ticks
                                 * every four pixels. Manhattan distance
                                 * overestimates by up to 41%, which is a
                                 * slightly unhurried walk and not a defect.
                                 * Capped so a generous grab radius cannot
                                 * produce a minute-long stroll. */
                                long ax = dx_ - player.x, ay = dy_ - player.y;
                                long px;
                                if (ax < 0) ax = -ax;
                                if (ay < 0) ay = -ay;
                                px = (ax + ay) >> 16;
                                enter_walk_len = (int)((px * 3) / 4);
                                if (enter_walk_len > 60) enter_walk_len = 60;
                                if (enter_walk_len < 0) enter_walk_len = 0;
                                enter_walk_t = 0;
                            }
                            /* Already against the body: no walk, straight
                             * over. */
                            if (vault_pending && enter_walk_len == 0) {
                                vault = 1; vault_pending = 0;
                                vault_step = vault_tick = vault_hold = 0;
                            }
                            enter_bike = ci_->vtype == GTA_VEH_BIKE;
                            player.anim = enter_bike ? GTA_ANIM_ENTER_BIKE
                                                     : GTA_ANIM_ENTER_CAR;
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
                            /* AND THE DRIVER WILL BE DRAGGED OUT - not now,
                             * but when the door is open, the way the
                             * original's jacker does it at state 0x1c
                             * (LEFTOFF.md "THE CARJACK VICTIM"). An
                             * abandoned car has nobody in it, so nobody
                             * comes out of it. */
                            enter_driver = drv_;
                        } else {
                            printf("gta: no car within reach\n");
                            fflush(stdout);
                        }
                    } else {
                        /* OUT AT THE DOOR - the original's own exit,
                         * LEFTOFF.md, the exit. */
                        const gta_car_info *ci_ = &tiles.cars[veh.model];
                        long dx_, dy_, ex_, ey_, avx_, avy_, spd_;
                        int a_ = gta_veh_angle(&veh);
                        int sgn_ = car_door_side(ci_);
                        int blocked_ = 0;

                        /* REFUSED ABOVE SPEED 4 - nothing happens and the
                         * car drives on; the original prints nothing, this
                         * says so once for the log's sake. */
                        avx_ = veh.vx < 0 ? -veh.vx : veh.vx;
                        avy_ = veh.vy < 0 ? -veh.vy : veh.vy;
                        spd_ = avx_ > avy_ ? avx_ + avy_ / 2 : avy_ + avx_ / 2;
                        if (spd_ >= GTA_VEH_EXIT_MAX_SPEED) {
                            printf("gta: too fast to get out\n");
                            fflush(stdout);
                        } else {
                        car_door_point(ci_, veh.ox, veh.oy, a_, &dx_, &dy_);
                        /* THE DOOR SIDE BLOCKED? The original probes the spot
                         * 2 units outside the sill for a car, a building or
                         * air. A car or a building here; "air" at the ped's
                         * own layer is too easy to hit on a kerb to be
                         * trusted as a wall. */
                        car_door_pos(ci_, veh.ox, veh.oy, a_, 0, 2, &ex_, &ey_);
                        if (fleet_car_at(&traffic, &tiles, ex_, ey_,
                                         player.layer, 2, 0) >= 0)
                            blocked_ = 1;
                        else if (gta_nav_ground(gta_nav_at_m((&nav),
                                     (int)(ex_ >> 21), (int)(ey_ >> 21),
                                     player.layer)) == GTA_GROUND_BUILDING)
                            blocked_ = 2;
                        in_car = 0;
                        handbrake = 0;
                        up = down = left = right = 0;
                        if (blocked_) {
                            /* EJECTED OVER THE ROOF: put on the car half way
                             * along its front half, facing the flank the
                             * door is NOT on, and into the vault - the
                             * original's state 0x73 with speed 4. The car
                             * is parked first so the vault's probe finds
                             * it under him. */
                            long fx = gta_sin(a_), fy = -gta_cos(a_);
                            long hl = gta_car_world_len(ci_) / 2;
                            if (!gta_traffic_abandon(&traffic, veh.model,
                                                     veh.ox, veh.oy, a_,
                                                     player.layer, veh.remap,
                                                     veh.damage))
                                printf("gta: fleet full, car lost\n");
                            player.x = veh.ox + fx * hl * 2;
                            player.y = veh.oy + fy * hl * 2;
                            vault_head = (a_ - sgn_ * 64) & 255;
                            player.angle = vault_head;
                            vault = 2;
                            vault_step = vault_tick = vault_hold = 0;
                            printf("gta: door blocked by %s - over the roof,"
                                   " heading %d\n",
                                   blocked_ == 1 ? "a car" : "a building",
                                   vault_head);
                        } else {
                            enter_x0 = veh.ox;
                            enter_y0 = veh.oy;
                            enter_a0 = a_;
                            enter_dx = dx_;
                            enter_dy = dy_;
                            player.angle = a_;
                            enter_bike = ci_->vtype == GTA_VEH_BIKE;
                            player.anim = enter_bike ? GTA_ANIM_EXIT_BIKE
                                                     : GTA_ANIM_EXIT_CAR;
                            player.frame = 0;
                            player.frame_tick = 0;
                            enter_anim = 2;
                            enter_step = 0;
                            enter_tick = 0;
                            /* THE CAR STAYS EXACTLY WHERE IT IS, drawn by
                             * this code with its door swinging, and only
                             * when he is standing beside it does it become
                             * an abandoned fleet car. Handing it to the
                             * fleet on this tick was why the exit happened
                             * through a shut door (LEFTOFF, "STILL OPEN on
                             * the door"). */
                            printf("gta: getting out - car at (%ld,%ld) facing"
                                   " %d, door at (%ld,%ld), player was at"
                                   " (%ld,%ld)\n",
                                   veh.ox >> 16, veh.oy >> 16, a_,
                                   dx_ >> 16, dy_ >> 16,
                                   player.x >> 16, player.y >> 16);
                        }
                        fflush(stdout);
                        }
                    }
                }
                /* THE ANIMATION ITSELF - one step every GTA_ENTER_TICKS, with
                 * the controls dead while it runs. Getting in ends with the
                 * player in the seat; getting out ends on his feet. */
                if (enter_anim) {
                    int steps = enter_bike
                              ? (enter_anim == 1 ? GTA_PED_ENTER_BIKE_FRAMES
                                                 : GTA_PED_EXIT_BIKE_FRAMES)
                              : (enter_anim == 1 ? GTA_PED_ENTER_STEPS
                                                 : GTA_PED_EXITCAR_FRAMES);
                    int per   = (enter_anim == 1) ? GTA_ENTER_TICKS
                                                  : GTA_EXIT_TICKS;
                    int total = steps * per;
                    int t     = enter_step * per + enter_tick;

                    if (enter_anim == 1 && vault == 1) {
                        /* THE VAULT, ON THE WAY IN. Runs at his own pace
                         * along the frozen heading, sprite 91 + state; at
                         * every state boundary (states 0..3) the probe one
                         * pixel ahead decides: still car - next state; road
                         * - land. States 4 and 5 run out regardless. On
                         * landing the walk target is still the door, as it
                         * is in the original, so he walks the last few
                         * pixels and the get-in sequence follows. */
                        long fx = gta_sin(vault_head), fy = -gta_cos(vault_head);
                        player.x += (fx * GTA_RUN_SPEED_FP) >> 14;
                        player.y += (fy * GTA_RUN_SPEED_FP) >> 14;
                        player.angle = vault_head;
                        player.anim  = GTA_ANIM_VAULT;
                        player.frame = vault_step;
                        if (++vault_tick >= GTA_VAULT_TICKS) {
                            const gta_car_info *vi = &tiles.cars[enter_model];
                            int still = car_body_hit(enter_cx, enter_cy,
                                            enter_face,
                                            gta_car_world_len(vi) / 2,
                                            gta_car_world_wid(vi) / 2,
                                            player.x + (fx << 2),
                                            player.y + (fy << 2), 0);
                            vault_tick = 0;
                            /* 91, 92, 93 advance while a car is ahead; 94
                             * is HELD until it is not. Sprites 95 and 96
                             * never play: the original's states 0x77/0x78
                             * are written by nothing (PROGRESS.md 115). The
                             * hold has a ceiling here that the original
                             * lacks, so a probe that never clears cannot
                             * pin him on a roof forever. */
                            if (!still || ++vault_hold > GTA_VAULT_HOLD_MAX) {
                                long ax, ay, px;
                                vault = 0;
                                enter_x0 = player.x;
                                enter_y0 = player.y;
                                enter_a0 = player.angle;
                                enter_dx = vault_dx;
                                enter_dy = vault_dy;
                                ax = enter_dx - player.x; if (ax < 0) ax = -ax;
                                ay = enter_dy - player.y; if (ay < 0) ay = -ay;
                                px = (ax + ay) >> 16;
                                enter_walk_len = (int)((px * 3) / 4);
                                if (enter_walk_len > 60) enter_walk_len = 60;
                                enter_walk_t = 0;
                                printf("gta: landed after state %d at (%ld,%ld),"
                                       " %ld px from the door\n", vault_step,
                                       player.x >> 16, player.y >> 16, px);
                                fflush(stdout);
                            } else if (vault_step < 3) {
                                vault_step++;
                            }
                        }
                        up = down = left = right = 0;
                    } else if (enter_anim == 1 && enter_walk_t < enter_walk_len) {
                        /* PHASE 0 - HE WALKS TO THE DOOR, on the ordinary walk
                         * cycle, turning to face the car as he goes. The step
                         * counter does not advance here: the ten-step sequence
                         * has not started yet. */
                        player.anim  = GTA_ANIM_WALK;
                        player.frame = (enter_walk_t / 3) % GTA_PED_WALK_FRAMES;
                        player.x = lerp_fp(enter_x0, enter_dx,
                                           enter_walk_t, enter_walk_len);
                        player.y = lerp_fp(enter_y0, enter_dy,
                                           enter_walk_t, enter_walk_len);
                        player.angle = lerp_angle(enter_a0,
                                                  vault_pending ? vault_head
                                                                : enter_face,
                                                  enter_walk_t, enter_walk_len);
                        enter_walk_t++;
                        /* At the flank: the vault takes over from the walk. */
                        if (vault_pending && enter_walk_t >= enter_walk_len) {
                            vault = 1; vault_pending = 0;
                            vault_step = vault_tick = vault_hold = 0;
                        }
                        up = down = left = right = 0;
                    } else {
                    /* THE DOOR OPENS WHEN HE REACHES IT, not when he sets off
                     * for it - so its clock starts here, at the first tick of
                     * the sequence phase, and not back at the RETURN. */
                    if (enter_anim == 1 && door_tick < 0 && !enter_bike &&
                        enter_step == 0 && enter_tick == 0)
                        door_tick = 0;
                    /* THE DOOR IS OPEN: OUT COMES THE DRIVER. The original
                     * creates the victim in the seat at the jacker's state
                     * 0x1c, which follows the door-open wait, and walks him
                     * through 0x93..0x98 against the car. Here that is the
                     * tick the door clock reaches fully open (20 = four
                     * records at five ticks); a bike has no door, so its
                     * rider comes off at once. */
                    if (enter_anim == 1 && enter_driver
                        && (enter_bike || door_tick >= 20)) {
                        enter_driver = 0;
                        if (enter_bike
                            ? gta_peds_knock_off(&peds,
                                  enter_cx + (long)gta_cos(enter_face) * 12 * 4,
                                  enter_cy + (long)gta_sin(enter_face) * 12 * 4,
                                  player.layer, (enter_face + 64) & 255, -1)
                            : gta_peds_pull(&peds, enter_cx, enter_cy,
                                          enter_face, enter_model,
                                          player.layer, -1)) {
                            /* Taking a car OFF SOMEBODY scores; a parked one is
                             * worth nothing, in the original as here. */
                            long a = gta_score_event(&score, GTA_SCORE_TYPE_CAR, 0);
                            gta_score_crime(&score, GTA_CRIME_CARJACK);
                            if (enter_cop) {
                                /* A POLICE CAR TAKEN: its driver is a cop
                                 * and comes after him on foot, and the
                                 * level is exactly 1 if it was 0. */
                                gta_peds_make_cop(&peds, peds.last_index);
                                gta_score_force_level(&score, 1);
                                printf("gta: police - the player took a cop"
                                       " car; its cop is on foot\n");
                            }
                            printf("gta: dragged the driver out - %ld points"
                                   " (score %ld, heat %d)\n", a, score.score,
                                   score.heat);
                        } else {
                            printf("gta: driver lost - ped pool full\n");
                        }
                        fflush(stdout);
                    }
                    player.anim  = enter_bike
                                 ? (enter_anim == 1 ? GTA_ANIM_ENTER_BIKE
                                                    : GTA_ANIM_EXIT_BIKE)
                                 : (enter_anim == 1 ? GTA_ANIM_ENTER_CAR
                                                    : GTA_ANIM_EXIT_CAR);
                    player.frame = enter_step;

                    /* PHASE 1 - THE SEQUENCE ITSELF, at the car.
                     *
                     * Getting in: he stands at the handle for the first eight
                     * steps (26,26,26,25,25,29,30,31 - reaching, leaning, legs
                     * over the sill) and drops into the seat over the last two
                     * (32,33). Frame 33 is the sitting pose: the art agent
                     * found it is four pixels away from frame 97,
                     * `sitting_in_car`, which is what proves the sequence's
                     * direction and its endpoint.
                     *
                     * Getting out: seat to handle over all eight of 16..23,
                     * which is that same motion stored backwards.
                     *
                     * Carnage3D snaps to the door on entry and to the seat on
                     * the last frame with nothing in between (PROGRESS.md
                     * 110). It gets away with it because its car is drawn
                     * throughout and its ped animation actually plays - ours
                     * did neither. */
                    if (enter_anim == 1) {
                        int slide = (steps - 2) * per;
                        if (t <= slide) {
                            player.x = enter_dx;
                            player.y = enter_dy;
                        } else {
                            player.x = lerp_fp(enter_dx, enter_cx,
                                               t - slide, total - slide);
                            player.y = lerp_fp(enter_dy, enter_cy,
                                               t - slide, total - slide);
                        }
                        player.angle = enter_face;
                    } else if (enter_bike) {
                        player.x = lerp_fp(enter_x0, enter_dx, t, total);
                        player.y = lerp_fp(enter_y0, enter_dy, t, total);
                        player.angle = enter_a0;
                    } else {
                        /* GETTING OUT OF A CAR: the original SNAPS him to a
                         * place relative to the car every state - the
                         * exit tables - it never slides him. Inside the
                         * body's edge for the first three states, so the
                         * car drawn over him hides him until the door is
                         * open and he swings out at state 4. */
                        int s = enter_step < GTA_PED_EXITCAR_FRAMES
                              ? enter_step : GTA_PED_EXITCAR_FRAMES - 1;
                        car_door_pos(&tiles.cars[veh.model], veh.ox, veh.oy,
                                     enter_a0, exit_along[s], exit_lat[s],
                                     &player.x, &player.y);
                        player.angle = enter_a0;
                    }

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
                            /* ON HIS FEET beside the shut door, facing 45
                             * degrees off the car's heading towards the
                             * door side (the original's rot + 0x80), or
                             * square off a bike. And only NOW is the car an
                             * abandoned fleet car - where it stopped, with
                             * nobody in it, drawn, solid, enterable. */
                            const gta_car_info *xi = &tiles.cars[veh.model];
                            int sgn = car_door_side(xi);
                            if (!enter_bike)
                                car_door_pos(xi, veh.ox, veh.oy, enter_a0,
                                             -2, 1, &player.x, &player.y);
                            player.angle = (enter_a0
                                            + sgn * (enter_bike ? 64 : 32))
                                           & 255;
                            if (!gta_traffic_abandon(&traffic, veh.model,
                                                     veh.ox, veh.oy, enter_a0,
                                                     player.layer, veh.remap,
                                                     veh.damage))
                                printf("gta: fleet full, car lost\n");
                            printf("gta: on foot at (%ld,%ld) facing %d\n",
                                   player.x >> 16, player.y >> 16,
                                   player.angle);
                        }
                        player.anim = GTA_ANIM_STAND;
                        player.frame = 0;
                        enter_anim = 0;
                        fflush(stdout);
                    }
                    up = down = left = right = 0;
                    }
                }
                /* SPACE ON FOOT - the original's other way into the same
                 * states. Running at a car with the body within 3 px ahead:
                 * a low one is vaulted along his own heading, a tall one is
                 * slid under. Nothing ahead, and SPACE keeps its old job of
                 * dumping the frame, which no unattended run can do. */
                if (jump_req) {
                    jump_req = 0;
                    if (!in_car && !enter_anim && !vault && !slide) {
                        int low = 0, hit = -1;
                        if (player.anim == GTA_ANIM_RUN) {
                            long fx = gta_sin(player.angle);
                            long fy = -gta_cos(player.angle);
                            hit = fleet_car_at(&traffic, &tiles,
                                               player.x + fx * 12,
                                               player.y + fy * 12,
                                               player.layer, 1, &low);
                        }
                        if (hit >= 0 && low) {
                            vault = 2; vault_step = vault_tick = vault_hold = 0;
                            vault_head = player.angle;
                            printf("gta: jump - vaulting fleet car %d\n", hit);
                        } else if (hit >= 0) {
                            slide = 1; slide_tick = 0;
                            printf("gta: jump - sliding under fleet car %d\n",
                                   hit);
                        } else {
                            dump_frame(GTA_DIR "frame_live.raw", chunky, pitch,
                                       SCREEN_W, SCREEN_H, tiles.palette);
                            printf("gta: camera at block (%ld,%ld)\n",
                                   view.cam_x >> 21, view.cam_y >> 21);
                        }
                        fflush(stdout);
                    }
                }
                if (vault == 2) {
                    /* The free vault: same states, same probe against
                     * whatever fleet car is under him, and it lands on his
                     * feet with the controls back. */
                    long fx = gta_sin(vault_head), fy = -gta_cos(vault_head);
                    player.x += (fx * GTA_RUN_SPEED_FP) >> 14;
                    player.y += (fy * GTA_RUN_SPEED_FP) >> 14;
                    player.angle = vault_head;
                    player.anim  = GTA_ANIM_VAULT;
                    player.frame = vault_step;
                    if (++vault_tick >= GTA_VAULT_TICKS) {
                        int still = fleet_car_at(&traffic, &tiles,
                                                 player.x + (fx << 2),
                                                 player.y + (fy << 2),
                                                 player.layer, 0, 0) >= 0;
                        vault_tick = 0;
                        if (!still || ++vault_hold > GTA_VAULT_HOLD_MAX) {
                            vault = 0;
                            player.anim = GTA_ANIM_STAND;
                            player.frame = 0;
                            printf("gta: landed after state %d\n", vault_step);
                            fflush(stdout);
                        } else if (vault_step < 3) {
                            vault_step++;
                        }
                    }
                    up = down = left = right = 0;
                }
                if (slide) {
                    /* State 0x92: while a car is still over him, a pixel
                     * along the heading every state; then he stands up. */
                    player.anim  = GTA_ANIM_SLIDE_UNDER;
                    player.frame = 0;
                    if (++slide_tick >= GTA_VAULT_TICKS) {
                        slide_tick = 0;
                        if (fleet_car_at(&traffic, &tiles, player.x, player.y,
                                         player.layer, 1, 0) >= 0) {
                            player.x += gta_sin(player.angle) << 2;
                            player.y -= gta_cos(player.angle) << 2;
                        } else {
                            slide = 0;
                            player.anim = GTA_ANIM_STAND;
                            printf("gta: out from under\n");
                            fflush(stdout);
                        }
                    }
                    up = down = left = right = 0;
                }
                /* ...AND WHILE HE IS GETTING OUT the car still has physics:
                 * the original refuses the exit above speed 4 but lets a
                 * slower car roll on with the door open and re-places the
                 * ped against it every state. The controls are dead by
                 * then, so this is drag, walls and the fleet, nothing more. */
                if (in_car || enter_anim == 2) {
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
                    /* AND THE CAR CLIMBS. Until now the layer under a driven
                     * car was frozen at whatever the player was standing on
                     * when he got in, because nothing but gta_player_update()
                     * ever moved it - so a ramp led nowhere and every bridge
                     * was something you drove UNDER. See gta_veh_layer().
                     *
                     * Resolved BEFORE the wall test, so the test runs on the
                     * layer the car has arrived at. */
                    {
                        long nx0_, ny0_, nx1_, ny1_;
                        int nz_;
                        gta_veh_nose(&veh, wox0_, woy0_, wang0_,
                                     &nx0_, &ny0_);
                        gta_veh_nose(&veh, veh.ox, veh.oy, veh.ang16,
                                     &nx1_, &ny1_);
                        nz_ = gta_veh_layer(&nav, player.layer,
                                       (int)(nx0_ >> 21), (int)(ny0_ >> 21),
                                       (int)(nx1_ >> 21), (int)(ny1_ >> 21),
                                       nx1_ - nx0_, ny1_ - ny0_);
                        if (nz_ != player.layer) {
                            printf("gta: car layer %d -> %d at block "
                                   "(%d,%d)\n", player.layer, nz_,
                                   (int)(veh.ox >> 21), (int)(veh.oy >> 21));
                            fflush(stdout);
                            player.layer = nz_;
                        }
                    }
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
                            /* AND IT DENTS THE PANEL THAT TOOK IT. Only a
                             * car-to-car ram used to do that, so a player who
                             * drove into every building in Liberty City ended
                             * up with a scratchless car and a damage number
                             * nobody could see - "jak jechalem to nic sie nie
                             * dzieje z rogami". The contact is in the
                             * direction the car was going when the wall
                             * stopped it, and veh.hit_vx is exactly that
                             * vector. */
                            veh.dmg_bits |= 1UL << gta_car_panel_delta(
                                &tiles.cars[veh.model], veh.ox, veh.oy,
                                gta_veh_angle(&veh),
                                veh.ox + veh.hit_vx * 4,
                                veh.oy + veh.hit_vy * 4);
                            printf("gta: wall hit at %d px/tick - "
                                   "damage %d\n", wdmg_, veh.damage);
                            fflush(stdout);
                        }
                    }
                    /* AND HE DOES NOT END THE TICK INSIDE ANOTHER CAR.
                     *
                     * This is the original's own answer, and it is the only
                     * one that does not show: it never lets an overlap
                     * happen, so it never needs a shove to undo one. Its
                     * physics step bisects eight times between the transform
                     * it has committed and the one it proposes - position AND
                     * angle - and keeps the last one that was clear.
                     *
                     * Without it the correction has to remove the whole
                     * overlap afterwards, and at 20 px a tick that is
                     * sixteen pixels in one frame on the car the player is
                     * steering: "nadal za mocno mnie odrzuca ... teleportuje
                     * mnie o 10 pikseli w 1 klatce". Measured with
                     * `gtadump hitcar ... 20 200 0 0 64`, WORST PUSH ON THE
                     * PLAYER.
                     *
                     * Eight steps of a 256th each: the last free point is
                     * within half a pixel of the contact, which is closer
                     * than the eye can see at 32 px to a block. */
                    if (opt_traffic) {
                        const gta_car_info *bi_ = &tiles.cars[veh.model];
                        int bhl_ = gta_car_world_len(bi_) / 2;
                        int bhw_ = gta_car_world_wid(bi_) / 2;
                        long nx_ = veh.ox, ny_ = veh.oy, na_ = veh.ang16;
                        if (gta_traffic_sweep_box(&traffic, wox0_, woy0_,
                                                  wang0_, &nx_, &ny_, &na_,
                                                  bhl_, bhw_, player.layer)) {
                            /* The body centre is what was swept; the centre
                             * of mass follows it by the same amount. */
                            veh.x += nx_ - veh.ox;
                            veh.y += ny_ - veh.oy;
                            veh.ox = nx_;
                            veh.oy = ny_;
                            veh.ang16 = na_;
                            veh_contact_stops++;
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
                        /* "SHUNTS 'N' BUMPS": two points of heat per car
                         * hit, and only when the player is driving at speed
                         * - the original exempts anything inside its
                         * -5..11 band, which is the same five units that
                         * decide whether a run-over kills. Nudging a parked
                         * car is not a crime. */
                        if (nhit > 0) {
                            long avx = veh.vx < 0 ? -veh.vx : veh.vx;
                            long avy = veh.vy < 0 ? -veh.vy : veh.vy;
                            if (avx >= 5L * 32768L || avy >= 5L * 32768L) {
                                int k;
                                for (k = 0; k < nhit; k++)
                                    gta_score_crime(&score, GTA_CRIME_SHUNT);
                            }
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
                            /* The panel that took it: the resolution vector
                             * points out of the other body, so the contact is
                             * the other way. */
                            veh.dmg_bits |= 1UL << gta_car_panel_delta(
                                &tiles.cars[veh.model], veh.ox, veh.oy,
                                gta_veh_angle(&veh),
                                veh.ox - rpx * 8, veh.oy - rpy * 8);
                            printf("gta: ram x%d - player dv (%ld,%ld) "
                                   "damage %d\n", nhit,
                                   rvx >> 16, rvy >> 16, veh.damage);
                            fflush(stdout);
                        }
                    }
                    if (in_car) {
                        player.x = veh.ox;
                        player.y = veh.oy;
                    }
                } else if (walk_mode && !enter_anim && !vault && !slide) {
                    /* GTA's own on-foot controls: forward and back on the
                     * up/down keys, left and right TURN rather than strafe,
                     * and the player RUNS by default - the original has no
                     * walk key at all. Shift is the exception, not the
                     * accelerator.
                     *
                     * NOT WHILE HE IS GETTING IN OR OUT. With no forward
                     * input this function resets p->anim to STAND and
                     * p->frame to 0 - correct for standing still, fatal here.
                     * It ran on every tick of the animation and threw away
                     * the frame the animation block had just set one line
                     * earlier, so gta_ped_enter_seq (26,26,26,25,25,29..33)
                     * and the exit run 16..23 were computed and then
                     * discarded. The player stood motionless at frame 98 for
                     * the whole 0.8 seconds: dead code that looked exactly
                     * like a missing feature. */
                    if (punch_left > 0 && !up && !down) {
                        /* THE STANDING PUNCH is a state of its own,
                         * 0xa9..0xae, a frame every GTA_PUNCH_TICKS;
                         * gta_player_update would reset it to STAND. He
                         * may still turn. */
                        player.anim = GTA_ANIM_PUNCH;
                        player.frame = (GTA_PUNCH_TICKS * GTA_PED_PUNCH_FRAMES
                                        - punch_left) / GTA_PUNCH_TICKS;
                        if (right || left)
                            player.angle = (player.angle
                                + ((right ? 1 : 0) - (left ? 1 : 0)) * 5) & 255;
                    } else {
                        gta_player_update(&player, &map,
                                          (right ? 1 : 0) - (left ? 1 : 0),
                                          (up ? 1 : 0) - (down ? 1 : 0),
                                          fast);
                    }
                }
                /* THE FIRE LATCH - the original's, once a
                 * tick: with fists a punch (not while one is in flight);
                 * with the pistol a round every time the cooldown has run
                 * out, the peds around panicked, the ammo counted down and
                 * the weapon dropped to fists when it hits zero. Never
                 * from a car, never mid-animation. */
                if (fire_cool > 0)
                    fire_cool--;
                if (punch_left > 0) {
                    punch_left--;
                    if (punch_left == GTA_PUNCH_TICKS * (GTA_PED_PUNCH_FRAMES - 2)) {
                        /* the blow lands on the third state */
                        int v = gta_peds_punch(&peds, player.x, player.y,
                                               player.angle, player.layer);
                        if (v >= 0)
                            printf("gta: punch - ped %d down\n", v);
                        else if (v == -2)
                            printf("gta: punch - missed (the 11th/12th of 13)\n");
                    }
                }
                if (fire_held && walk_mode && !in_car && !enter_anim
                    && !vault && !slide) {
                    if (weapon == 0) {
                        if (punch_left == 0)
                            punch_left = GTA_PUNCH_TICKS * GTA_PED_PUNCH_FRAMES;
                    } else if (fire_cool == 0 && ammo[weapon] > 0) {
                        if (gta_weapons_fire(&weapons, weapon, player.x,
                                             player.y, player.layer,
                                             player.angle,
                                             player.anim == GTA_ANIM_RUN,
                                             -1)) {
                            fire_cool = gta_weapons_cooldown(weapon);
                            gta_peds_panic(&peds, player.x, player.y,
                                           player.layer);
                            /* The machine gun and the flamethrower get
                             * five shots out of one unit; the pistol and the
                             * rocket launcher spend one each. */
                            if (ammo_sub[weapon] > 0) {
                                if (--ammo_sub[weapon] == 0) {
                                    ammo_sub[weapon] = GTA_AMMO_PER_UNIT;
                                    ammo[weapon]--;
                                }
                            } else {
                                ammo[weapon]--;
                            }
                            if (ammo[weapon] <= 0) {
                                ammo[weapon] = 0;
                                weapon = ammo[GTA_WEAPON_PISTOL] > 0
                                       ? GTA_WEAPON_PISTOL : GTA_WEAPON_FIST;
                                printf("gta: out of ammo - weapon %d\n",
                                       weapon);
                            }
                        }
                    }
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
                    gta_traffic_set_wanted(&traffic, score.level,
                                           in_car || enter_anim == 2);
                    gta_peds_set_player(&peds, in_car ? veh.ox : player.x,
                                        in_car ? veh.oy : player.y,
                                        player.layer, in_car);
                    {
                        long cx_, cy_;
                        int cl_, ca_;
                        if (gta_traffic_cop_out(&traffic, &cx_, &cy_, &cl_, &ca_)) {
                            if (gta_peds_spawn_cop(&peds, cx_, cy_, cl_, ca_))
                                printf("gta: police - a cop is on foot at"
                                       " (%ld,%ld), %d out\n", cx_ >> 16, cy_ >> 16,
                                       gta_peds_cops_out(&peds));
                            else
                                printf("gta: police - no room for the cop\n");
                            fflush(stdout);
                        }
                    }
                    if (peds.stat_cops_killed != cops_killed_seen) {
                        /* A COP KILLED: a hundred more heat on top of the
                         * murder, and every car standing with its driver
                         * out gives up. */
                        long k_ = peds.stat_cops_killed - cops_killed_seen;
                        cops_killed_seen = peds.stat_cops_killed;
                        while (k_-- > 0)
                            gta_score_crime(&score, GTA_CRIME_MURDER);
                        gta_traffic_cops_give_up(&traffic);
                        printf("gta: police - a cop was killed\n");
                        fflush(stdout);
                    }
                    {
                        long rx_, ry_;
                        int rl_, ra_;
                        while (gta_traffic_roadblock_cop(&traffic, &rx_, &ry_, &rl_, &ra_))
                            if (gta_peds_spawn_cop(&peds, rx_, ry_, rl_, ra_))
                                gta_peds_post_last_cop(&peds);
                    }
                    if (bust_timer == 0 && score.level > 0 && peds.cop_shoot == 0 &&
                        gta_peds_cop_event(&peds)) {
                        /* BUSTED. The original: the jingle, the card, the
                         * multiplier halved (never below 1), armour, speed
                         * and every weapon gone, heat and level zero, and
                         * the player put down at the nearest police station
                         * on foot. Score and lives untouched. */
                        int k;
                        bust_timer = BUST_TICKS;
                        card_kind = 1;
                        if (jail_free)
                            jail_free = 0;      /* the card is spent instead */
                        else if (score.multiplier > 1)
                            score.multiplier /= 2;
                        for (k = 1; k < GTA_WEAPON_COUNT; k++) ammo[k] = 0;
                        weapon = 0;
                        fire_held = 0;
                        printf("gta: BUSTED - multiplier %d, weapons gone,"
                               " crimes this life:", score.multiplier);
                        for (k = 0; k < GTA_CRIME_COUNT; k++)
                            if (score.crimes[k]) printf(" %d x%ld", k, score.crimes[k]);
                        printf("\n");
                        fflush(stdout);
                        gta_score_clear_heat(&score);
                        gta_score_new_life(&score);
                    }
                    if (bust_timer > 0 && --bust_timer == 0) {
                        /* The card is over: out of the car, and to the
                         * station. */
                        int best = -1, k;
                        long bd = 0;
                        long fx_ = in_car ? veh.ox : player.x;
                        long fy_ = in_car ? veh.oy : player.y;
                        if (in_car) {
                            int a_ = gta_veh_angle(&veh);
                            if (!gta_traffic_abandon(&traffic, veh.model, veh.ox,
                                                     veh.oy, a_, player.layer,
                                                     veh.remap, veh.damage))
                                printf("gta: fleet full, car lost\n");
                            in_car = 0;
                            enter_anim = 0;
                            enter_driver = 0;
                            door_tick = -1;
                            player.anim = GTA_ANIM_STAND;
                            player.frame = 0;
                        }
                        const gta_map_loc *loc_ = card_kind == 2 ? map.hospital : map.police;
                        int nloc_ = card_kind == 2 ? map.n_hospital : map.n_police;
                        if (card_kind == 2) {
                            player_lives--;
                            player_health = 100;
                            player_armour = 0;
                            if (player_lives < 0) {
                                /* GAME OVER: the level starts again. */
                                player_lives = 4;
                                gta_score_init(&score);
                                printf("gta: GAME OVER - the level starts again\n");
                            }
                        }
                        for (k = 0; k < nloc_; k++) {
                            long dx_ = ((long)loc_[k].x << 21) - fx_;
                            long dy_ = ((long)loc_[k].y << 21) - fy_;
                            long d_;
                            if (dx_ < 0) dx_ = -dx_;
                            if (dy_ < 0) dy_ = -dy_;
                            d_ = dx_ > dy_ ? dx_ : dy_;
                            if (best < 0 || d_ < bd) { best = k; bd = d_; }
                        }
                        if (best >= 0) {
                            /* A pavement block within three of the station,
                             * on whichever layer has one. */
                            int sx = loc_[best].x, sy = loc_[best].y;
                            int r_, ex, ey, z_, found = 0;
                            for (r_ = 0; r_ <= 3 && !found; r_++)
                                for (ey = -r_; ey <= r_ && !found; ey++)
                                    for (ex = -r_; ex <= r_ && !found; ex++)
                                        for (z_ = 0; z_ < GTA_MAP_LAYERS && !found; z_++) {
                                            int g_ = gta_nav_ground(gta_nav_at_m(&nav, sx + ex, sy + ey, z_));
                                            if (g_ == GTA_GROUND_PAVEMENT) {
                                                player.x = ((long)(sx + ex) << 21) + (16L << 16);
                                                player.y = ((long)(sy + ey) << 21) + (16L << 16);
                                                player.layer = z_;
                                                found = 1;
                                            }
                                        }
                            printf("gta: %s - put down at the %s (%d,%d)%s\n",
                                   card_text[card_kind & 3],
                                   card_kind == 2 ? "hospital" : "police station",
                                   sx, sy, found ? "" : " - no pavement, left in place");
                        } else {
                            printf("gta: %s - no %s on this map\n", card_text[card_kind & 3],
                                   card_kind == 2 ? "hospital" : "police station");
                        }
                        gta_peds_clear_cops(&peds);
                        walk_mode = 1;
                        fflush(stdout);
                    }
                    /* ...and while he is getting OUT the car is still his,
                     * still solid, and not yet in the fleet. */
                    if (in_car || enter_anim == 2) {
                        long sp = veh.vx < 0 ? -veh.vx : veh.vx;
                        long sq = veh.vy < 0 ? -veh.vy : veh.vy;
                        gta_traffic_set_player(&traffic, 1, veh.ox, veh.oy,
                                               sp > sq ? sp : sq,
                                               gta_veh_angle(&veh),
                                               player.layer,
                                               veh.len / 2, veh.wid / 2);
                    } else {
                        /* ON FOOT HE IS STILL SOMETHING TO BRAKE FOR.
                         *
                         * This used to switch the player off the moment he
                         * left the car, so the fleet could not see him at all:
                         * traffic drove straight through a man standing in the
                         * road and shoved him along the street - "po graczu
                         * tez przejezdzaja nie przejmujac sie. nawet screena
                         * nie moglem zrobic tak sie pchaja".
                         *
                         * The fleet reads this in two places and both are the
                         * right answer for a pedestrian: gap_ahead() makes the
                         * car behind him keep its distance, and body_on_sq()
                         * stops anything driving into the square he is
                         * standing in. His body box is the walker's own three
                         * pixels, not a car's. */
                        long pspd = (player.anim == GTA_ANIM_RUN
                                  || player.anim == GTA_ANIM_WALK)
                                  ? GTA_RUN_SPEED_FP : 0;
                        gta_traffic_set_player(&traffic, 1,
                                               player.x, player.y, pspd,
                                               player.angle, player.layer,
                                               3, 3);
                    }
                    /* AND EVERYBODY ON FOOT, so the fleet brakes for them.
                     * Refreshed here rather than kept in step incrementally:
                     * the pool is twelve and a rebuild costs nothing next to
                     * getting it out of step. Somebody already lying in the
                     * road is not in the list - a car does not stop for a body
                     * and the original drives over it. */
                    gta_traffic_clear_walkers(&traffic);
                    {
                        int wi_;
                        for (wi_ = 0; wi_ < GTA_MAX_PEDS; wi_++) {
                            const gta_ped *pp = &peds.p[wi_];
                            if (!pp->alive || pp->down > 0)
                                continue;
                            gta_traffic_add_walker(&traffic, pp->x, pp->y,
                                                   pp->layer);
                        }
                    }
                    gta_traffic_set_view_blocks(&traffic,
                                            (render_w() / 2) / zoom_display + 1);
                    amiga_wd_set(AMIGA_WD_PHASE_TRAFFIC);
                    gta_traffic_tick(&traffic, &map, view.cam_x, view.cam_y);
                    amiga_wd_set(AMIGA_WD_PHASE_SIM);
                }
                /* The spawner puts people on the edge of the view ahead of
                 * the player: it needs the view in blocks and his heading
                 * (the car's when he drives). */
                gta_peds_set_view(&peds,
                                  (render_w() / 2) / zoom_display + 1,
                                  (SCREEN_H / 2) / zoom_display + 1,
                                  in_car ? gta_veh_angle(&veh) : player.angle,
                                  in_car ? (veh.vx || veh.vy)
                                         : (player.anim == GTA_ANIM_WALK
                                            || player.anim == GTA_ANIM_RUN));
                /* THE CAR HE IS SITTING IN, WRITTEN OFF. It burns for the
                 * same fuse the fleet's wrecks get and then comes apart in
                 * five bursts; he is put out on the road first, since there
                 * is no player health yet to take from him. */
                if (in_car && veh.damage >= GTA_CAR_WRECKED) {
                    if (veh.fuse == 0) {
                        veh.fuse = GTA_CAR_FUSE;
                        veh.dmg_bits |= GTA_DELTA_DMG_MASK;
                        printf("gta: your car is a write-off - get out\n");
                        fflush(stdout);
                    } else if (--veh.fuse == 0) {
                        const gta_car_info *wi = &tiles.cars[veh.model];
                        long wx = veh.ox, wy = veh.oy;
                        int wface = gta_veh_angle(&veh);
                        /* Out on the road beside it, on his feet. */
                        player.x = wx + (long)gta_cos(wface) * 48;
                        player.y = wy + (long)gta_sin(wface) * 48;
                        player.angle = (wface + 64) & 255;
                        player.anim = GTA_ANIM_STAND;
                        player.frame = 0;
                        in_car = 0;
                        enter_anim = 0;
                        enter_driver = 0;
                        door_tick = -1;
                        gta_weapons_wreck_car(&weapons, wi, wx, wy, wface,
                                              player.layer, &peds, &traffic,
                                              &score, 1);
                        /* AND THE SCRAP STAYS THERE. His car is not in the
                         * fleet while he is driving it, so without this the
                         * one car in the city he is guaranteed to be looking
                         * at is the one that vanishes when it explodes. */
                        if (opt_traffic)
                            gta_traffic_leave_wreck(&traffic, veh.model,
                                                    wx, wy, wface,
                                                    player.layer, veh.remap);
                        printf("gta: your car blew up at (%ld,%ld)\n",
                               wx >> 16, wy >> 16);
                        fflush(stdout);
                    }
                }
                gta_score_tick(&score);
                hud_weapon = weapon;
                hud_ammo = ammo[weapon >= 0 && weapon < 5 ? weapon : 0];
                amiga_wd_set(AMIGA_WD_PHASE_PEDS);
                gta_peds_tick(&peds, &map, view.cam_x, view.cam_y);
                /* The bullets fly after the people have moved, in the
                 * original's order: peds, cars, then the block. */
                amiga_wd_set(AMIGA_WD_PHASE_WEAPONS);
                gta_weapons_tick(&weapons, &nav, &peds, &traffic, &tiles, &score);
                amiga_wd_set(AMIGA_WD_PHASE_PLAYER);
                if (pickups.n > 0 && bust_timer == 0) {
                    long px_ = in_car ? veh.ox : player.x;
                    long py_ = in_car ? veh.oy : player.y;
                    int kind_, amount_;
                    gta_pickups_open_at(&pickups, px_, py_, player.layer,
                                        in_car ? 22 : 10);
                    if (gta_pickups_take(&pickups, px_, py_, player.layer,
                                         in_car ? 22 : 10, &kind_, &amount_)) {
                        static const char *const kind_name[16] = {
                            "?", "pistol", "machine gun", "rocket", "flame",
                            "?", "speed", "speed", "speed", "bribe", "armour",
                            "multiplier", "jail free", "life", "kill frenzy", "life" };
                        printf("gta: picked up %s %d\n",
                               kind_ >= 0 && kind_ < 16 ? kind_name[kind_] : "?",
                               amount_);
                        if (kind_ >= 1 && kind_ <= 4) {
                            /* The original: += amount capped at 99, 100+
                             * means infinite for (amount-100) ticks, and the
                             * picked weapon is selected. No infinite yet:
                             * it is 99 rounds. */
                            int a_ = amount_ >= 100 ? 99 : amount_;
                            if (a_ == 0) a_ = kind_ == 3 ? 5 : kind_ == 4 ? 10 : 20;
                            ammo[kind_] += a_;
                            if (ammo[kind_] > 99) ammo[kind_] = 99;
                            weapon = kind_;
                        } else if (kind_ == GTA_PICKUP_BRIBE) {
                            gta_score_clear_heat(&score);
                        } else if (kind_ == GTA_PICKUP_MULTIPLIER) {
                            score.multiplier++;
                        } else if (kind_ == GTA_PICKUP_JAILFREE) {
                            jail_free = 1;
                        } else if (kind_ == GTA_PICKUP_ARMOUR) {
                            player_armour = 3;
                        } else if (kind_ == GTA_PICKUP_LIFE || kind_ == 15) {
                            player_lives++;
                        } else {
                            /* armour, speed, life, kill frenzy: not yet -
                             * nothing reads them. Taken all the same. */
                        }
                        fflush(stdout);
                    }
                }
                /* THE PLAYER AS A TARGET, AND THE COPS' ORDERS. */
                {
                    int hb_, hc_, bl_, bu_;
                    int armed_ = fire_held && weapon > 0;
                    long sx_, sy_;
                    int sl_, sa_, si_;
                    gta_weapons_set_player(&weapons,
                                           in_car ? veh.ox : player.x,
                                           in_car ? veh.oy : player.y,
                                           player.layer, in_car,
                                           veh.len / 2, veh.wid / 2);
                    gta_peds_set_cop_shoot(&peds, score.level >= 4 ? 2
                                           : (armed_ || score.level >= 3) ? 1 : 0);
                    while ((si_ = gta_peds_cop_shot(&peds, &sx_, &sy_, &sl_, &sa_)) >= 0)
                        gta_weapons_fire(&weapons, 1, sx_, sy_, sl_, sa_, 0, si_);
                    gta_weapons_player_damage(&weapons, &hb_, &hc_, &bl_, &bu_);
                    if (bust_timer == 0) {
                        int k_;
                        for (k_ = 0; k_ < hb_; k_++) {
                            if (player_armour > 0) player_armour--;
                            else player_health -= 10;
                        }
                        if (hc_ > 0 && in_car) veh.damage += 5 * hc_;
                        if (bl_ > 0) player_health = 0;
                        if (bu_ > 0 && player_burning < 100) player_burning = 100;
                        if (player_burning > 0) { player_burning--; player_health--; }
                        if (gta_peds_cop_execute(&peds)) player_health = 0;
                        if (hb_ || hc_ || bl_) {
                            printf("gta: player hit - bullets %d, on the car %d,"
                                   " blast %d; health %d armour %d\n",
                                   hb_, hc_, bl_, player_health, player_armour);
                            fflush(stdout);
                        }
                        if (player_health <= 0) {
                            /* WASTED. The original: the card, weapons gone,
                             * heat and level zero, a life taken, and the
                             * respawn at the nearest hospital with health
                             * 100; multiplier and score untouched. At no
                             * lives it is the game over; the port starts
                             * the level again. */
                            player_health = 0;
                            player_burning = 0;
                            bust_timer = BUST_TICKS;
                            card_kind = 2;
                            for (k_ = 1; k_ < GTA_WEAPON_COUNT; k_++) ammo[k_] = 0;
                            weapon = 0;
                            fire_held = 0;
                            gta_score_clear_heat(&score);
                            printf("gta: WASTED - lives left %d\n", player_lives - 1);
                            fflush(stdout);
                        }
                    }
                }
                if (in_car) {
                    long avx = veh.vx < 0 ? -veh.vx : veh.vx;
                    long avy = veh.vy < 0 ? -veh.vy : veh.vy;
                    int ph = gta_peds_ram(&peds, veh.ox, veh.oy,
                                          gta_veh_angle(&veh),
                                          veh.len / 2, veh.wid / 2,
                                          player.layer,
                                          avx > avy ? avx + avy / 2
                                                    : avy + avx / 2);
                    if (ph) {
                        int k;
                        long award = 0;
                        for (k = 0; k < ph; k++) {
                            award = gta_score_event(&score,
                                        GTA_SCORE_TYPE_CIVILIAN,
                                        GTA_SCORE_REASON_RUNOVER);
                            /* The original files the hit AND the death: a
                             * man run over at speed is dead, and the two
                             * reports together are what make one run-over
                             * a level on its own (150 of 151). */
                            gta_score_crime(&score, GTA_CRIME_RUNOVER);
                            gta_score_crime(&score, GTA_CRIME_MURDER);
                        }
                        printf("gta: ran over %d - %ld so far, %ld points"
                               " (score %ld, heat %d)\n", ph,
                               peds.stat_runover, award, score.score,
                               score.heat);
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
                /* THE RELOAD FILE. Work:reload.txt, dropped by the host,
                 * means "start again with what is in the drawer now": the
                 * game deletes it and leaves with return code 5, and the run
                 * script's `if warn` loop starts the binary again. That is
                 * a new build and new scripts in the SAME emulator, without
                 * the restart that takes the mouse and the keyboard off the
                 * developer working beside it - which is what happened,
                 * every couple of minutes, and was rightly objected to. */
                if ((sim_ticks % 25) == 0 && !g_reload) {
                    FILE *rf = fopen(GTA_DIR "reload.txt", "r");
                    if (rf) {
                        fclose(rf);
                        remove(GTA_DIR "reload.txt");
                        g_reload = 1;
                        running = 0;
                        log_line("gta: reload - leaving with RC 5 for the "
                                 "run script to start the new build");
                    }
                }
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
                    printf("gta: police - wanted %d, %d chasing, %d on patrol,"
                           " sent %ld made %ld released %ld\n",
                           score.level, traffic.n_cop_chasing,
                           traffic.n_cop_patrol, traffic.stat_cops_sent,
                           traffic.stat_cops_made, traffic.stat_cops_released);
                    gta_traffic_police_report(&traffic);
                    printf("gta: peds at the lights - %ld set off, %ld crossed\n",
                           peds.stat_crossings, peds.stat_crossed);
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
                    /* AND THE PEOPLE: how many are alive, and where. A
                     * brain whose peds all die unborn looks, on a film,
                     * exactly like an empty city. */
                    {
                        int pi_, alive_ = 0, seen_ = 0;
                        for (pi_ = 0; pi_ < GTA_MAX_PEDS; pi_++) {
                            const gta_ped *pp = &peds.p[pi_];
                            if (!pp->alive) continue;
                            alive_++;
                            if (pp->offscreen == 0) seen_++;
                        }
                        /* AND WHETHER THEY ARE PILING UP. The developer
                         * saw people stacking on one side of the street -
                         * the fault the anti-crowd rule exists to prevent -
                         * and "it looks crowded" cannot be argued with. A
                         * pair within four pixels is two men standing in the
                         * same doorway; the worst pile is how many are in
                         * the biggest of those knots. */
                        {
                            int a_, b_, pairs_ = 0, worst_ = 0;
                            long wx_ = 0, wy_ = 0;
                            for (a_ = 0; a_ < GTA_MAX_PEDS; a_++) {
                                int near_ = 0;
                                if (!peds.p[a_].alive || peds.p[a_].corpse)
                                    continue;
                                for (b_ = 0; b_ < GTA_MAX_PEDS; b_++) {
                                    long dx_, dy_;
                                    if (b_ == a_ || !peds.p[b_].alive
                                        || peds.p[b_].corpse)
                                        continue;
                                    dx_ = (peds.p[a_].x - peds.p[b_].x) >> 16;
                                    dy_ = (peds.p[a_].y - peds.p[b_].y) >> 16;
                                    if (dx_ > -4 && dx_ < 4
                                        && dy_ > -4 && dy_ < 4) {
                                        near_++;
                                        if (b_ > a_) pairs_++;
                                    }
                                }
                                if (near_ > worst_) {
                                    worst_ = near_;
                                    wx_ = peds.p[a_].x >> 16;
                                    wy_ = peds.p[a_].y >> 16;
                                }
                            }
                            snprintf(ln, sizeof ln,
                                     "gta: peds %d alive (%d in view), spawned"
                                     " %ld, run over %ld, killed %ld; %d pairs"
                                     " within 4px, worst %d at (%ld,%ld)",
                                     alive_, seen_, peds.stat_spawned,
                                     peds.stat_runover, peds.stat_killed,
                                     pairs_, worst_ + (worst_ ? 1 : 0),
                                     wx_, wy_);
                        }
                        log_line(ln);
                        if (weapons.stat_fired || peds.stat_punched) {
                            snprintf(ln, sizeof ln,
                                     "gta: weapons fired %ld: ped %ld car %ld"
                                     " wall %ld spent %ld, %ld bursts;"
                                     " punched %ld; weapon %d ammo %d;"
                                     " score %ld",
                                     weapons.stat_fired, weapons.stat_ped,
                                     weapons.stat_car, weapons.stat_wall,
                                     weapons.stat_expired, weapons.stat_expl,
                                     peds.stat_punched, weapon, ammo[weapon],
                                     score.score);
                            log_line(ln);
                        }
                    }
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

                    /* AND THE TWO IMPOSSIBLE THINGS - a car turning further
                     * than any car can in one tick, or moving further. Both
                     * are the developer's own reports, as numbers. */
                    snprintf(cl, sizeof cl,
                             "gta: impossible - %ld turns (worst %ld of 256,"
                             " state %ld), %ld jumps (worst %ld px, state %ld)",
                             traffic.stat_face_jump, traffic.stat_face_jump_max,
                             traffic.stat_face_jump_ctx,
                             traffic.stat_pos_jump, traffic.stat_pos_jump_max,
                             traffic.stat_pos_jump_ctx);
                    log_line(cl);

                    /* AND TRAFFIC HITTING TRAFFIC, which until today never
                     * happened at all: cars drove through each other unless
                     * they were closing hard, and drove through anything
                     * parked whatever the speed. */
                    snprintf(cl, sizeof cl,
                             "gta: fleet hits %ld, knocked loose %ld, settled"
                             " %ld, corners dropped %ld",
                             traffic.stat_fleet_hits, traffic.stat_knocked,
                             traffic.stat_knock_ended, traffic.stat_arc_dropped);
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
        /* The splats are ground marks: under everybody. */
        gta_weapons_draw_ground(&weapons, &view);
        gta_pickups_draw(&pickups, &view, 12);
        gta_peds_draw(&peds, &view);
        /* THE CAR BEING ENTERED IS STILL A CAR.
         *
         * This is the fault the whole change is about. `gta_traffic_grab_car`
         * takes the car out of the fleet on the tick RETURN is pressed, and
         * the player's own `veh` is not created until the animation ends forty
         * ticks later - so for 0.8 seconds NOTHING drew it. The car blinked
         * out, the player stood in the road, and then the car reappeared with
         * him inside. out/before_enter_sheet.png is what that looked like.
         *
         * Everything needed to draw it was already being carried in
         * enter_model / enter_cx / enter_cy / enter_face / enter_remap for the
         * gta_veh_init at the end. It just was not being used.
         *
         * ORDER IS THE ANIMATION. Sprites are drawn in insertion order within
         * a layer, so adding the player FIRST puts him behind the car - which
         * is what should happen as he climbs in on frames 29..33, and what
         * Carnage3D gets from its eSpriteDrawOrder_CarPassenger. A convertible
         * would want the other order; we do not draw a seated player at all
         * yet, so that distinction has nowhere to show up (see LEFTOFF). */
        {
            /* THE CAR HE IS GETTING OUT OF IS HIS CAR STILL - drawn from
             * `veh`, with its door on the exit's own clock - until the exit
             * ends and the fleet takes it. */
            int car_shown  = in_car || enter_anim;
            int use_veh    = in_car || enter_anim == 2;
            int car_model  = use_veh ? veh.model : enter_model;
            long car_x     = use_veh ? veh.ox : enter_cx;
            long car_y     = use_veh ? veh.oy : enter_cy;
            int  car_ang   = use_veh ? gta_veh_angle(&veh) : enter_face;
            int  car_remap = use_veh ? veh.remap : enter_remap;
            int  door_now  = door_delta(door_tick);
            if (enter_anim == 2 && !enter_bike) {
                int s = enter_step < GTA_PED_EXITCAR_FRAMES
                      ? enter_step : GTA_PED_EXITCAR_FRAMES - 1;
                door_now = exit_door[s] ? GTA_DELTA_DOOR1 + exit_door[s] - 1
                                        : -1;
            }

            /* WHO IS ON TOP. A hard top hides its driver, so the man goes in
             * first and the car covers him. A bike or a convertible has
             * nothing to hide him under: the vehicle goes in first and the
             * rider is drawn OVER it - climbing on, riding (frame 84 on a
             * bike, 97 in an open car, turning with the vehicle), and
             * climbing off. Before this the rider simply vanished on
             * mounting, which is what the developer saw. */
            const gta_car_info *pi = car_shown ? &tiles.cars[car_model] : 0;
            /* ...and a man in mid-vault is over the roof whatever the car
             * is. The original gets that from his z, set to the car's
             * floor height for the duration; here it is draw order. */
            int on_top = pi && (pi->vtype == GTA_VEH_BIKE
                                || (pi->convertible & 1) || vault == 1);

            if (!in_car && !on_top && vault != 2)
                gta_render_add_sprite(&view, player.x, player.y, player.layer,
                                      gta_player_grid(&player),
                                      armed_sprite(&player, punch_left,
                                                   ARMED_NOW),
                                      gta_player_draw_angle(&player));
            if (car_shown && pi->sprite_index >= 0)
                gta_render_add_sprite_dm(&view, car_x, car_y, player.layer,
                                      player.layer, pi->sprite_index,
                                      (car_ang + GTA_SPRITE_ART_SOUTH) & 255,
                                      car_remap >= 0
                                          && car_remap < GTA_CAR_REMAPS
                                          ? (int)pi->remap8[car_remap] : 0,
                                      door_now,
                                      use_veh ? veh.dmg_bits : 0);
            /* ...and if it is a write-off, it burns while its fuse runs. */
            if (car_shown && use_veh && veh.fuse > 0) {
                int fs_ = gta_tiles_object_sprite(&tiles, GTA_OBJ_FIRE);
                if (fs_ >= 0)
                    gta_render_add_sprite(&view, car_x, car_y, player.layer,
                                          player.layer,
                                          fs_ + ((veh.fuse / 3) % 7), 0);
            }
            if (on_top) {
                if (in_car)
                    gta_render_add_sprite(&view, car_x, car_y, player.layer,
                                          player.layer,
                                          player.ped_base
                                          + (pi->vtype == GTA_VEH_BIKE
                                             ? GTA_PED_SIT_ON_BIKE
                                             : GTA_PED_SIT_IN_CAR),
                                          (car_ang + GTA_SPRITE_ART_SOUTH)
                                          & 255);
                else
                    gta_render_add_sprite(&view, player.x, player.y,
                                          player.layer,
                                          gta_player_grid(&player),
                                          armed_sprite(&player, punch_left,
                                                       ARMED_NOW),
                                          gta_player_draw_angle(&player));
            }
        }

        /* Cars after the player, so that if the sprite list ever fills the
         * thing that gets dropped is a parked car and not the man the
         * keyboard is driving. */
        if (opt_traffic)
            gta_traffic_draw(&traffic, &view);
        /* The free vault is over a FLEET car, so he goes in after the
         * fleet. */
        if (vault == 2)
            gta_render_add_sprite(&view, player.x, player.y, player.layer,
                                  gta_player_grid(&player),
                                  armed_sprite(&player, punch_left, ARMED_NOW),
                                  gta_player_draw_angle(&player));
        /* The tracers, over everything on the street. */
        gta_weapons_draw_air(&weapons, &view);

        /* Redraw every pass, moving or not. Holding the frame when nothing has
         * changed would be free frames per second and a dishonest measurement;
         * the place to earn that back is Phase 7, with a dirty-rectangle
         * scheme that is measured rather than assumed. */
        {
            unsigned long pa = amiga_uclock_us();
            unsigned long c0 = bench_blit_us, pb;

            mode_apply(&view);
            amiga_wd_set(AMIGA_WD_PHASE_RENDER);
            gta_render_frame(&view);
            pb = amiga_uclock_us();
            amiga_wd_set(AMIGA_WD_PHASE_PRESENT);
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
        amiga_wd_set(AMIGA_WD_PHASE_CAP);
        if (frame_cap) {
            while ((unsigned long)(amiga_uclock_us() - frame_t0)
                       < (unsigned long)FRAME_CAP_US)
                ;
        }
        frame_t0 = amiga_uclock_us();
    }

    amiga_watchdog_stop();
    t1 = amiga_uclock_us();
    if (frames > 0)
        log_fps("gta: interactive", frames, t1 - t0);

    printf("gta: lid cache - %ld tiles scaled, %lu of %lu bytes, %ld overflow\n",
           view.lc_fills, view.lc_used,
           (unsigned long)GTA_LIDCACHE_BYTES, view.lc_full);
    fflush(stdout);

    amigagfx_close();
    gta_render_free(&view);
    gta_sfx_free(&sfx);
    gta_map_free(&map);
    gta_tiles_free(&tiles);
    log_line("gta: clean exit");
    /* 5 is WARN to AmigaDOS: the run script's `if warn` restarts the game.
     * A player's ESC returns 0 and the script falls through. */
    return g_reload ? 5 : 0;
}
