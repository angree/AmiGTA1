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
#include "gta_style.h"      /* GTA_SPR_ARROW - the HUD icon category */
#include "gta_hud.h"
#include "gta_player.h"
#include "gta_traffic.h"
#include "gta_vehphys.h"
#include "gta_peds.h"
#include "gta_weapon.h"
#include "gta_score.h"
#include "gta_pickup.h"
#include "gta_script.h"
#include "gta_front.h"
#include "gta_font.h"
#include "gta_text.h"
#include "gta_prefs.h"
#include "gta_sfx.h"
#include "gta_audio.h"
#include "gta_snd.h"
#include "gta_iff.h"

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
/* The multiplier has a font of its own in the data and nothing else uses
 * it, which is as clear a statement as the file makes about where it goes. */
#define FONT_MULT  GTA_DIR "GTADATA/missmul1.fon"
/* The front end's own font and its baked art. */
#define FONT_MENU  GTA_DIR "GTADATA/f_mtext.fon"
#define FRONT_PATH GTA_DIR "GTADATA/front.fnt"
/* THE SOUND BANK IS OPTIONAL, and that is not laziness.
 *
 * Nothing plays it yet, the shipped archive has no game data at all, and a
 * player who has only converted the art must still get a running game rather
 * than an error about a file they were never told to make. Absent means
 * silent. When there is a player, it will still mean silent. */
#define SFX_PATH   GTA_DIR "GTADATA/level001.snd"
/* THE VOICE - vocalcom, baked the same way (`gtabake -sfx audio/vocalcom`)
 * but never loaded: only its index is read, and each line comes off the disk
 * when it is said. See gta_sfx_open_index(). */
#define VOICE_PATH GTA_DIR "GTADATA/vocalcom.snd"

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
#define KEY_Y      0x15     /* QUIT GAME? - yes */
#define KEY_N      0x36     /* QUIT GAME? - no */

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

/* WHERE THE PLAYER ACTUALLY STARTS, once the level script has been read: the
 * PLAYER declaration's block. START_BX/START_BY stay as they are because the
 * BENCHMARKS quote them - a headline frame rate measured over a different
 * junction is not comparable with the ones already in PROGRESS.md. */
static int start_bx = START_BX, start_by = START_BY;

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
#define MAX_CATCHUP  6
/* How much real time the simulation may spend catching up in ONE frame. A
 * 50 Hz world needs 20 ms of ticks per 20 ms of clock; at ten frames a
 * second that is five ticks a frame, and on the pseudo-040 a tick is about
 * 8 ms, so forty milliseconds is the whole of that and a little over. Past
 * the budget the world falls behind the clock - which is the old slow motion
 * - but only as far as the machine truly cannot go, not by a fixed count. */
#define SIM_BUDGET_US 40000UL

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
/* `rampdbg 1` in opts.txt: one line every time the height a driven car is
 * drawn at changes, which is the only way to see the pulse the developer
 * reported - the fault is in the SEQUENCE of values, not in any one of
 * them, and a screenshot cannot show a sequence. */
static int opt_rampdbg = 0;
/* `engine <id>` in opts.txt: which bank sound the engine note loops. The
 * default and the shortlist that produced it are in gta_audio.c; nobody
 * here can hear the emulator, so this is how the developer settles it. */
static int opt_engine = -1;     /* -1: the car's own (205); `engine N` forces one */
static int opt_traffic = 1;
static int opt_fleet   = -1;
static int opt_lights  = -1;    /* -1 = the module's default (on) */
static int opt_halfrate = -1;   /* -1 = the module's default (on); `halfrate 0` for the A/B */
static int opt_cruise = -1;     /* -1 = the module's default (on); `cruise 0` for the A/B */
static int opt_driveprof = 0;   /* `driveprof 1`: time drive_one's sections (PROGRESS 182) -
                                 * one E-clock read is ~68 us on the pseudo-040, nine a
                                 * tick for the sampled car, so it is OFF unless asked */
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
static int opt_bench = 0;       /* `bench 1`: the startup benchmark (218: ours, not the player's) */
/* -1 = leave gta.prefs alone; 0 = silent; 1 = on. See the opts.txt parser. */
static int opt_audio_opt = -1;

/* WHAT THE PLAYER CHOSE FOR SOUND, read from gta.prefs by gtaprefs.
 *
 * `gta_audio.c` branches on this. GTA_AUDIO_OFF and GTA_AUDIO_AHI both mean
 * SILENCE for now and they mean it differently: OFF is the player's choice,
 * AHI is a promise this port has not kept yet, and neither may quietly fall
 * back to Paula - a machine where AHI was chosen is usually one where Paula
 * is not reachable at all (MorphOS), and banging audio.device there is
 * reported to hang it. AUTO and PAULA open the chipset.
 *
 * ON MORPHOS THE MAPPING IS INVERTED, and the paragraph above is the reason:
 * the machine that cannot reach Paula is that one, so AHI is not a promise
 * there - it is the only backend. native/morphos_audio.c implements the whole
 * amiga_audio.h contract over it and amiga_audio.c is not built. Every
 * setting but OFF opens AHI; see the branch in gta_audio.c. */
static int opt_audio = GTA_AUDIO_AUTO;

static gta_traffic traffic;

/* THE MAP AND THE BAKED TILE SET. Up here with the rest of the file statics
 * rather than beside `view` and `player`, because the HUD draws the style
 * file's own icons now and every drawing routine in this file needs them. */
static gta_map   map;
static gta_tiles tiles;
static gta_peds peds;

/* The pedestrians ask the traffic module about the lights through this. */
static int ped_light_green(void *ctx, int bx, int by, int along_x)
{
    return gta_traffic_light_green((const gta_traffic *)ctx, bx, by, along_x);
}
static gta_pickups pickups;
/* THE LEVEL SCRIPT'S DECLARATION BLOCK - the phones, the parked mission
 * cars, the triggers, the counters and the rest of what a mission is played
 * with. Read here so the game says in the log what it found; the interpreter
 * that acts on it is the next step (LEFTOFF, phase 5 item 6). */
static gta_script script;

/* How many crates the script has made, and how many POWERUP_ONs asked for one
 * that was already there. The second number is not a fault: 175 POWERUP_ON
 * commands name only 137 distinct powerups. */
static long script_crates_made, script_crates_refused;
/* WHICH ICON THE LAST BRIEF ASKED FOR - GTA_BRIEF_*. */
static int  script_brief_kind = GTA_BRIEF_PAGER;
static int script_triggers_listed;

static void pager_brief(int key);
static void script_brief(void *ctx, int kind, int key, int arg);
static void script_score(void *ctx, long points);
static void log_fps(const char *label, int frames, unsigned long us);
static void dump_frame(const char *path, const unsigned char *chunky,
                       int pitch, int w, int h, const unsigned char *palette);


/* THE ORIGINAL'S FONTS AND TEXTS (Phase 5 item 7, the first piece). The
 * pager font draws the briefs along the bottom, the score font the score,
 * the big font the cards. Each is optional: when a file is missing the
 * port's own 3x5 font draws that part, as before. */
static gta_font pager_font, score_font, big_font, mult_font;
static int have_pager, have_score_font, have_big, have_mult;
static gta_text texts;
static gta_sfx   voice;         /* index only - see VOICE_PATH */
static int kf_ticks[2] = { -1, -1 };    /* see script_kf_timer() */
/* THE THREE DISPLAYS, and which one a command uses is the original's
 * business, not a choice.
 *
 *   (a) THE BRIEF BOX along the bottom: an ICON at the left edge and the
 *       text beside it. Six kinds; the icon of each is a sprite of the HUD
 *       set (gta_hud.h). It stands for `(len + 24)` of the script's own
 *       ticks - `((strlen + 0x18) * (4 / text_speed)) / 2` at the normal
 *       speed - and a kind 0..2 brief replaces a kind 3..5 one at once
 *       because those carry the higher priority.
 *
 *   (b) THE PAGER at the top left: the device is sprite +3 of the HUD set,
 *       80x30 with a dark window and a red light, and the line SCROLLS
 *       through the window right to left until it has gone.
 *
 *   (c) THE BIG MESSAGE, centred low: up to three words, `len * 5` ticks.
 *
 * Ticks here are the SIMULATION'S 50 Hz, so every duration out of the
 * original is doubled on the way in. */
static char brief_text[400];
static int  brief_ticks, brief_kind;
static char pager_line[400];
static int  pager_ticks, pager_px, pager_secs, pager_led;
static char big_text[80];
static int  big_ticks;

/* The window in the pager device the text shows through, measured off the
 * sprite: the dark panel runs from x=9 to x=71 and y=7 to y=18 of the 80x30
 * device. */
#define PAGER_WIN_X0  9
#define PAGER_WIN_X1 71
#define PAGER_WIN_Y   6
#define PAGER_LED_X  11
#define PAGER_LED_Y  19

static void text_copy(char *dst, int cap, const char *s)
{
    int i;
    for (i = 0; s[i] && i < cap - 1; i++)
        dst[i] = s[i];
    dst[i] = 0;
}

static int text_len(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* (b) - a line through the pager. */
static void pager_show(const char *s)
{
    text_copy(pager_line, (int)sizeof pager_line, s);
    pager_ticks = 1;
    pager_px = 0;
    pager_secs = -1;
}

/* (a) - the brief box. Priority: kinds 0..2 beat kinds 3..5. */
static void brief_show(int kind, const char *s)
{
    if (brief_ticks > 0 && brief_kind <= GTA_BRIEF_MOBILE &&
        kind > GTA_BRIEF_MOBILE)
        return;                         /* the important one keeps the box */
    text_copy(brief_text, (int)sizeof brief_text, s);
    brief_kind = kind;
    brief_ticks = (text_len(s) + 24) * 2;
}

/* (c) - the big card. */
static void big_show(const char *s)
{
    /* AND THREE OF THEM ARE SAID. the original's routine, the original's big message,
     * compares the text with three keys before it draws it and speaks the
     * matching line - by the TEXT, so whichever command put it up, the
     * jingle comes with it. */
    static const int say_key[3]  = { 2501, 2500, 2504 };
    static const int say_line[3] = { GTA_VOICE_MISSION_FAILED,
                                     GTA_VOICE_MISSION_COMPLETE,
                                     GTA_VOICE_FRENZY_FAILED };
    int k;
    for (k = 0; k < 3; k++) {
        const char *t = gta_text_get(&texts, say_key[k]);
        if (t && strcmp(t, s) == 0) {
            gta_audio_speak(&voice, say_line[k], 64, 1);
            break;
        }
    }
    text_copy(big_text, (int)sizeof big_text, s);
    big_ticks = text_len(s) * 5 * 2;
}

/* The pager line with numeric key `key` from the texts, if there is one. */
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

/* WHAT THE SCRIPT MAY DO TO THE WORLD. Same reason as script_brief: the
 * interpreter is built by the host tools too and must not know what a crate
 * is. `pickups` and `nav` are file statics, so the context is unused. */
static void script_powerup_on(void *ctx, int line, int kind, int amount,
                              int bx, int by)
{
    (void)ctx;
    if (gta_pickups_add_block(&pickups, &nav, bx, by, kind, amount)) {
        script_crates_made++;
        /* ONE LINE PER CRATE, on purpose. The count alone cannot tell a
         * right crate from a wrong one, and the mapping being checked here
         * is p1 -> declaration -> block: awk the log for these and compare
         * them with mission.ini's own POWERUP lines. */
        printf("gta: crate line %d kind %d amount %d at (%d,%d)\n",
               line, kind, amount, bx, by);
    } else {
        script_crates_refused++;
    }
}

static void script_powerup_off(void *ctx, int line, int bx, int by)
{
    (void)ctx; (void)line;
    gta_pickups_remove_block(&pickups, bx, by);
}

static int script_powerup_done(void *ctx, int line, int bx, int by)
{
    (void)ctx; (void)line;
    return !gta_pickups_at_block(&pickups, bx, by);
}

/* THE ARROW - the one piece of guidance GTA 1 gives you, and it is NOT a
 * marker over the target.
 *
 * The original puts it near the
 * PLAYER, at `player - dir(angle) * (base + reach)`, where the angle is the
 * bearing from the player to the target and `reach` moves eight units a
 * frame:
 *
 *   * target OFF screen - reach falls to 0, so the arrow sits just clear of
 *     the player and simply points the way;
 *   * target ON screen - reach grows toward `max(|dx|,|dy|) - base`, so the
 *     arrow slides out from the player and comes to rest beside the thing.
 *
 * `base` is 0x20 on foot and `(car_length + 1) * 6 + 0x20` in a car, in the
 * original's units of 64 to a block - half of ours. */
static int  arrow_on;
static long arrow_tx, arrow_ty;     /* 16.16 world, the target */
static long arrow_reach;            /* world pixels, the sliding part */
static int  arrow_angle;
static int  arrow_sprite = -1;

/* THE HUD ICON SET is the same "arrow" category the mission arrow comes
 * from; these are the offsets the original uses. See gta_hud.h. */
static int  hud_icon = -1;              /* the category's base sprite */
#define HUD_ICON_PAGER    3
#define HUD_ICON_PAGER_LED 4
#define HUD_ICON_WEAPON   5             /* +0..3: pistol, MG, rocket, flame */
#define HUD_ICON_COP     16             /* and 17 - the pair flashes */
#define HUD_ICON_ARMOUR  18             /* and 19 */
#define HUD_ICON_KEY     20             /* and 21 - the jail-free card */
#define HUD_ICON_FRENZY  22             /* and 23 */
/* ARROWCAR follows something that drives away, so the target is re-read
 * every tick from the fleet rather than being a fixed point. */
static unsigned long arrow_car_h;
static unsigned long arrow_ped_h;

/* THE SERIAL OF THE CAR THE PLAYER IS IN, so a mission can say "that car".
 * Set from the fleet when he gets in and handed back to it when he gets out;
 * zero when he is on foot. Without it a stolen car is a NEW car the moment
 * the door shuts and STEAL / CHECK_CAR have nothing to compare against. */
static unsigned long veh_serial;
static int veh_mission;         /* ...and whether it belongs to a job */

/* A MIRROR OF THE CAR HE IS DRIVING. `veh` and `in_car` are locals of main()
 * and the script's callbacks are file-scope, so the sim tick copies over the
 * four fields they ask about - the same arrangement as hud_weapon. */
/* SET BY THE SPRAY SHOP, applied by the tick - the callback runs inside the
 * script and `veh` is a local of main(). -1 when there is nothing to do. */
/* THE CAR IN THE AIR - see the note over the take-off test below. `veh_z` is
 * height above the layer's own surface in 16.16 world pixels, `veh_vz` the
 * rate, `veh_lift` how many ticks of climb are left. */
static long veh_z, veh_vz;
static int  veh_lift, veh_air;
static long veh_air_from;               /* how far it has flown, world px */

/* The original's numbers, halved twice over: its world unit is half this
 * port's pixel and its tick is half as long, so 8 units of gravity a tick
 * becomes 2 pixels and 4 of lift becomes 1, with twice as many lifting
 * ticks. The arc is the same shape and the same length. */
#define VEH_GRAVITY  (1L << 16)
#define VEH_LIFT     (1L << 16)
#define VEH_Z_MAX    (160L << 16)       /* the original's cap: five blocks */

static int  script_respray_to = -1;
static int  script_in_car, script_veh_model, script_veh_damage;
static long script_veh_x, script_veh_y, script_veh_speed;
static long script_pl_x, script_pl_y;

/* THE SECOND ARROW - see GTA_CMD_RED_ARROW. It has no animation of its
 * own here: it stands at the first arrow's distance from the player, in the
 * target's direction, and is not drawn once he is on the target's block. */
#define RED_ARROW_REMAP 3
static int  red_on;
static long red_tx, red_ty;

static void script_red_arrow(void *ctx, int on, long wx, long wy)
{
    (void)ctx;
    red_on = on;
    red_tx = wx;
    red_ty = wy;
    printf("gta: script - red arrow %s (%ld,%ld)\n", on ? "ON at" : "off",
           wx >> 16, wy >> 16);
    fflush(stdout);
}

static void script_arrow(void *ctx, int on, long wx, long wy)
{
    (void)ctx;
    if (on && !arrow_on)
        arrow_reach = 0;
    arrow_on = on;
    arrow_car_h = 0;
    arrow_ped_h = 0;
    arrow_tx = wx;
    arrow_ty = wy;
}

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
#define GTA_VERSION "v0.6.1"
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
/* WHICH THREE INDICES THE BAR IS DRAWN WITH, remembered because they are
 * fixed for the life of the screen and the front end loads a palette under
 * them. See front_palette(). */
static int bar_pen_text, bar_pen_fill, bar_pen_trim;

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
    bar_pen_text = text;
    bar_pen_fill = fill;
    bar_pen_trim = trim;
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
/* THE MODES, SMALLEST FIRST, so F4 steps up in size and wraps.
 *
 * `w,x,h,y` is the picture inside the display; `sw,sh` is the display to
 * open, and 0 means "the one the settings asked for". Only the last two
 * change the display, and only they can therefore FAIL - a screen mode the
 * machine has not got is a refusal, not a crash. */
static struct {
    int w, x, h, y;
    int sw, sh;
    const char *name;
} view_modes[] = {
    { 192, 64, 168, 16,   0,   0, "narrow, letterboxed" },
    { 256, 32, 168, 16,   0,   0, "letterboxed"         },
    { 256, 32, 200,  0,   0,   0, "5:4 (c2p-aligned)"   },
    { 320,  0, 200,  0,   0,   0, "full width"          },
    { 320,  0, 256,  0, 320, 256, "320x256 PAL"         },
    { 352,  0, 272,  0, 352, 272, "PAL overscan"        }
};

/* How many of them are on offer. The two big ones need a display this port
 * can actually ask for, which is the plain 320x200 one; a settings file that
 * has already chosen 320x240, 640x480 or the doubled mode keeps the four
 * that are only a rectangle. */
static int n_view_modes = 4;
/* The display the settings asked for, kept so a mode can go back to it. */
static int base_screen_w, base_screen_h, base_render_w, base_render_h;

static void view_modes_init(void)
{
    int fast   = (SCREEN_W * 4 / 5) & ~31;
    int narrow = (SCREEN_W * 3 / 5) & ~31;
    /* The letterbox: a bar of a twelfth of the height top and bottom,
     * rounded to eight rows so the arithmetic stays whole at half
     * resolution. 200 gives 16 and 168. */
    int bar    = (SCREEN_H / 12) & ~7;
    if (fast < 32) fast = 32;
    if (narrow < 32) narrow = 32;
    if (bar < 4) bar = 4;

    view_modes[0].w = narrow;
    view_modes[0].x = ((SCREEN_W - narrow) / 2) & ~31;
    view_modes[0].h = SCREEN_H - 2 * bar;
    view_modes[0].y = bar;

    view_modes[1].w = fast;
    view_modes[1].x = ((SCREEN_W - fast) / 2) & ~31;
    view_modes[1].h = SCREEN_H - 2 * bar;
    view_modes[1].y = bar;

    view_modes[2].w = fast;
    view_modes[2].x = ((SCREEN_W - fast) / 2) & ~31;
    view_modes[2].h = SCREEN_H;
    view_modes[2].y = 0;

    view_modes[3].w = SCREEN_W;
    view_modes[3].x = 0;
    view_modes[3].h = SCREEN_H;
    view_modes[3].y = 0;

    base_screen_w = g_screen_w;
    base_screen_h = g_screen_h;
    base_render_w = SCREEN_W;
    base_render_h = SCREEN_H;

    n_view_modes = (g_screen_w == 320 && g_screen_h == 200 && !g_scale2x)
                 ? 6 : 4;
}

/* Named indices. Nothing counts entries by hand - see the note below about
 * the evening an index one past the end cost. */
#define VIEW_MODES n_view_modes
#define VIEW_NARROW 0
#define VIEW_LETTER 1
#define VIEW_FAST   2
#define VIEW_FULL   3
#define VIEW_TALL   4
#define VIEW_OVER   5

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
/* CLAMPED TO THE SCREEN THAT IS ACTUALLY OPEN. Two of F4's modes ask for a
 * bigger display and are given one when they are entered; anything that sets
 * the mode index WITHOUT opening that display - the startup benchmark does,
 * because F4 cannot be pressed in an unattended run - would otherwise have
 * the renderer draw past the end of the chunky buffer. See the head of this
 * change: the first symptom was the script paying 421 million points. */
static int render_x(void)
{
    int x = view_modes[view_mode()].x;
    return (x >= 0 && x < SCREEN_W) ? x : 0;
}
static int render_y(void)
{
    int y = view_modes[view_mode()].y;
    return (y >= 0 && y < SCREEN_H) ? y : 0;
}
static int render_w(void)
{
    int x = render_x(), w = view_modes[view_mode()].w;
    if (w < 32) w = 32;
    return (x + w <= SCREEN_W) ? w : SCREEN_W - x;
}
static int render_h(void)
{
    int y = render_y(), h = view_modes[view_mode()].h;
    if (h < 32) h = 32;
    return (y + h <= SCREEN_H) ? h : SCREEN_H - y;
}

/* Where the picture actually lands in the chunky buffer, which at half
 * resolution is NOT render_x(): the low buffer is expanded by whole factors,
 * so an odd offset or width is rounded down twice over. Deriving the bars, the
 * readout and the blit from these instead of from render_x() is what makes the
 * narrow modes come out right at half resolution - the first version did not,
 * and left a column of the previous frame standing at the right-hand edge. */
static int present_x(void) { return (render_x() / mode_scale) * mode_scale; }
static int present_w(void) { return (render_w() / mode_scale) * mode_scale; }
static int present_y(void) { return (render_y() / mode_scale) * mode_scale; }
static int present_h(void) { return (render_h() / mode_scale) * mode_scale; }
/* The last row of the picture, which is where the brief box sits. Not the
 * last row of the SCREEN: in a letterboxed mode those are the bar. */
static int present_bot(void) { return present_y() + present_h(); }

/* THE SAME STREET, WHATEVER THE WINDOW (developer, 2026-09-25): "when the
 * resolution changes the camera should zoom in or out in proportion - in a
 * small window you can hardly see anything". The player's zoom is what a
 * FULL 320-pixel width shows; a narrower window zooms OUT by the same ratio
 * and a wider one IN, so every F4 size shows the same stretch of the city
 * across. `zoom_display` stays the player's (the -/= keys move it); this is
 * what the renderer and everything that asks "what is on screen" gets. */
#define ZOOM_REF_W 320
static int zoom_eff(void)
{
    long z = ((long)zoom_display * render_w() + ZOOM_REF_W / 2) / ZOOM_REF_W;
    z -= z % mode_scale;
    if (z < GTA_ZOOM_MIN * mode_scale) z = GTA_ZOOM_MIN * mode_scale;
    if (z > GTA_ZOOM_MAX) z = GTA_ZOOM_MAX;
    return (int)z;
}

static void mode_apply(gta_view *v)
{
    int w = render_w() / mode_scale;
    int h = render_h() / mode_scale;

    if (applied_mode != mode_narrow) {
        applied_mode = mode_narrow;
        bars_dirty = 1;
    }

    if (mode_scale == 2)
        gta_render_target(v, low_buffer + (long)(render_y() / mode_scale) * LOW_W
                             + render_x() / mode_scale,
                          w, h, LOW_W);
    else
        gta_render_target(v, g_chunky + (long)render_y() * g_pitch + render_x(),
                          w, h, g_pitch);
    v->flat_2d = mode_flat;
    gta_render_set_zoom(v, zoom_eff() / mode_scale);
}

/* SWITCHING F4's MODE, including the two that change the SCREEN.
 *
 * The four small modes are a rectangle inside the display the settings asked
 * for and cost nothing to enter. 320x256 and PAL overscan are a different
 * screen, so they go through the same close-and-reopen F3 uses for the title
 * bar - and can be REFUSED by a machine that has not got the mode, in which
 * case the previous screen is put back and the mode is not taken. Returns 0
 * only when even the old screen would not come back, which leaves nothing to
 * draw on and is the one case the caller must stop for. */
static int view_mode_set(gta_view *v, int want)
{
    int sw = view_modes[want].sw ? view_modes[want].sw : base_screen_w;
    int sh = view_modes[want].sh ? view_modes[want].sh : base_screen_h;

    if (sw != g_screen_w || sh != g_screen_h) {
        int ow = g_screen_w, oh = g_screen_h;
        int orw = SCREEN_W, orh = SCREEN_H;
        amigagfx_close();
        g_screen_w = sw;
        g_screen_h = sh;
        g_render_w = g_scale2x ? sw / 2 : sw;
        g_render_h = g_scale2x ? sh / 2 : sh;
        if (!open_display(v, g_show_bar)) {
            amigagfx_close();
            g_screen_w = ow; g_screen_h = oh;
            g_render_w = orw; g_render_h = orh;
            if (!open_display(v, g_show_bar)) {
                log_line("gta: the screen could not be reopened - stopping");
                return 0;
            }
            printf("gta: %s REFUSED (%dx%d) - kept %s\n",
                   view_modes[want].name, sw, sh,
                   view_modes[view_mode()].name);
            fflush(stdout);
            mode_apply(v);
            return 1;
        }
    }
    mode_narrow = want;
    mode_apply(v);
    printf("gta: render %dx%d at (%d,%d) in a %dx%d screen - %s\n",
           render_w(), render_h(), render_x(), render_y(),
           g_screen_w, g_screen_h, view_modes[view_mode()].name);
    fflush(stdout);
    return 1;
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

/* WHICH OF THE THREE DISPLAYS. The brief box and the pager are still the
 * one line along the bottom until the UI step; the kind is carried so that
 * step has only the drawing left to do, and so the log says which display
 * the script asked for. */
static void script_brief(void *ctx, int kind, int key, int arg)
{
    const char *s;
    (void)ctx;
    if (kind == GTA_BRIEF_CANCEL) {
        pager_ticks = 0;
        return;
    }
    s = gta_text_get(&texts, key);
    if (!s) {
        printf("gta: brief - no text %d\n", key);
        fflush(stdout);
        return;
    }
    script_brief_kind = kind;
    if (kind <= GTA_BRIEF_FRENZY) {
        brief_show(kind, s);
    } else if (kind == GTA_BRIEF_BIG) {
        big_show(s);
    } else {
        pager_show(s);
        if (kind == GTA_BRIEF_PAGER_T)
            pager_secs = arg;
    }
}

/* THE MONEY. `p5` on a success path, times the player's own multiplier, as
 * the original does it. */
static void script_score(void *ctx, long points)
{
    long paid;
    (void)ctx;
    if (points <= 0)
        return;
    paid = points * (score.multiplier > 0 ? score.multiplier : 1);
    score.score += paid;
    if (score.score > 999999999L) score.score = 999999999L;
    printf("gta: script - paid %ld (%ld x%d), score %ld\n",
           paid, points, score.multiplier, score.score);
    fflush(stdout);
}
/* WHAT THE CORNER SHOWS. The weapon and its ammunition live in the main
 * loop, where the keyboard is; the readout is drawn from a function that
 * runs in five other places as well (the tour, the benchmark), so the two
 * numbers are mirrored here rather than passed down through all of them. */
static int hud_weapon;
static int hud_ammo;
static int hud_frames;
static long hud_fps10;
static unsigned long hud_t0;

/* WHERE THE DEVELOPMENT READOUT LIVES: two 5-pixel lines just above the
 * brief line at the bottom, out of the game's own corners. */
/* WHERE THE DEVELOPMENT READOUT LIVES. Not the top left - that is the
 * pager, the weapon and the armour - and not the bottom left either, which
 * is where the brief box stacks its lines. Under the score, right-aligned
 * with it, in the corner this port has always used for its own numbers. */
#define HUD_DEBUG_Y 32


/* ---- THE FRONT END -------------------------------------------------------
 *
 * The title screen and the menu, in the game's own art: gta_front.h says
 * where that art is and how its format was worked out from the file sizes.
 * It runs ONCE, before the city is drawn, and it has its own palette - the
 * screen is set to f_pal while it is up and back to the style file's when
 * the game starts.
 *
 * IT IS SKIPPED WHEN A SCRIPT IS DRIVING. Every automated test in this
 * project runs with no keyboard at all (autodrive.txt / autowalk.txt /
 * autoinput.txt), and a menu that waits for a key would hang every one of
 * them. Nothing about the harness had to change; the menu simply does not
 * appear when a script is there.
 */
static gta_front front;
static gta_font  menu_font;
static int       have_menu_font;
static unsigned char front_ink;         /* brightest entry of f_pal */

#define MENU_ITEMS 3
static const char *const menu_item[MENU_ITEMS] = {
    "START GAME", "AMIGA OPTIONS", "QUIT"
};
#define MENU_QUIT 2

/* THE PLAYER'S SETTINGS AS LOADED, kept so the Amiga options page can change
 * one of them and write the file back without losing the others. */
static gta_prefs g_prefs;
static int g_prefs_loaded;

static void menu_line(unsigned char *chunky, int pitch, int y,
                      const char *text, int on)
{
    int w = have_menu_font ? gta_font_width(&menu_font, text)
                           : gta_hud_width(text);
    int fh = have_menu_font ? menu_font.height : 7;
    int x = (SCREEN_W - w) / 2;
    if (have_menu_font)
        gta_font_draw(&menu_font, chunky, pitch, SCREEN_W, SCREEN_H,
                      x, y, text);
    else
        gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H, x, y, text);
    /* THE SELECTION is a rule above and below, in the brightest colour the
     * FRONT END'S palette has - not the game's, which is not loaded while
     * this is on screen. */
    if (on) {
        int i;
        for (i = 0; i < 2; i++) {
            int by = y + (i ? fh + 2 : -3);
            unsigned char *d;
            int bx;
            if (by < 0 || by >= SCREEN_H) continue;
            d = chunky + (long)by * pitch;
            for (bx = x - 8; bx < x + w + 8; bx++)
                if (bx >= 0 && bx < SCREEN_W) d[bx] = front_ink;
        }
    }
}

/* THE FRONT END'S PALETTE, and the brightest entry of it to draw with.
 *
 * Factored out because the extraction screen runs BEFORE the menu and needs
 * exactly the same two things. The three title-bar pens keep the GAME's
 * colours whatever the artwork wanted them for: Intuition draws the bar out
 * of indices fixed when the screen opened, and a palette that ignores them
 * draws it in two colours nobody chose. The picture loses three of its 236
 * and never misses them.
 */
static void front_palette_apply(void)
{
    static unsigned char pal[768];
    const unsigned char *src = front.ok ? front.pal : tiles.palette;
    int i, k, best = 255;
    long bright = -1;

    for (i = 0; i < 256; i++) {
        long v = (long)src[i * 3] + src[i * 3 + 1] + src[i * 3 + 2];
        if (v > bright) { bright = v; best = i; }
    }
    front_ink = (unsigned char)best;

    for (k = 0; k < 768; k++)
        pal[k] = src[k];
    for (k = 0; k < 3; k++) {
        pal[bar_pen_text * 3 + k] = tiles.palette[bar_pen_text * 3 + k];
        pal[bar_pen_fill * 3 + k] = tiles.palette[bar_pen_fill * 3 + k];
        pal[bar_pen_trim * 3 + k] = tiles.palette[bar_pen_trim * 3 + k];
    }
    amigagfx_set_palette(pal, 0, 256);
}

/* THE TITLE THEME, in the order a player is likely to have it: the track
 * this port's own extractor writes, then one converted on a PC, then the
 * first radio station - because after a first-run extraction there IS a
 * radio1 and a title screen in silence would look like a fault. */
static void front_music_start(void)
{
    if (gta_audio_music_start(GTA_DIR "GTADATA/title.8svx", 1))
        return;
    if (gta_audio_music_start(GTA_DIR "GTADATA/title.mus", 1))
        return;
    gta_audio_radio_next();
}

/* ---- THE FIRST LOAD: EXTRACTING THE MUSIC --------------------------------
 *
 * OpenXcom does this and it is the right shape for the problem: the player
 * owns the original game, the port cannot ship anything derived from it, so
 * the first launch converts what they have and every launch after that finds
 * the conversion already done.
 *
 * WHAT IS CONVERTED. `GTADATA/Music/Track*.wav` - the 2002 release's
 * soundtrack, 22050 Hz 16-bit stereo - into `GTADATA/radioN.8svx`, signed
 * 8-bit mono at the same rate, which is what Paula's DMA reads with no
 * decoding at all. native/gta_iff.c does the work and says why in its header.
 *
 * IT RUNS EVEN WHEN A SCRIPT IS DRIVING, unlike the menu below. A menu waits
 * for a key nobody is going to press; this waits for nothing and it is data
 * preparation rather than a screen the player interacts with - and a test rig
 * that quietly skipped it would never once have run the code that ships.
 *
 * ESC ABANDONS IT and the partial track is deleted (gta_iff_abort), so the
 * next run does the same file again rather than playing half of one. A track
 * already extracted is never looked at twice.
 */
static int extract_dumped;

static void extract_draw(unsigned char *chunky, int pitch,
                         int idx, int n, const char *name, int permille)
{
    char line[48];
    int x0, x1, y, top, i, j, w;

    if (front.ok)
        gta_front_draw(&front, chunky, pitch, 0, 0);
    else
        memset(chunky, 0, (size_t)pitch * SCREEN_H);

    /* THE BLOCK SITS UNDER THE LOGO, not across it: the artwork's top 70
     * rows are the words "grand theft auto" and the first version drew
     * "EXTRACTING MUSIC" straight through them. Anchored as a fraction of
     * the screen so 320x256 and PAL overscan place it the same way. */
    top = SCREEN_H * 38 / 100;
    y = top + 46;
    x0 = 40;
    x1 = SCREEN_W - 40;
    if (x1 <= x0 + 8 || y + 58 >= SCREEN_H)
        return;                         /* a screen too small to say it on */

    menu_line(chunky, pitch, top, "EXTRACTING MUSIC", 0);
    snprintf(line, sizeof line, "TRACK %d OF %d", idx + 1, n);
    menu_line(chunky, pitch, top + 22, line, 0);

    /* The frame, then the fill. Two colours only - the brightest entry of
     * whichever palette is up and index 0 - because this screen is drawn
     * before the game's own colours are loaded and nothing else is known. */
    for (i = x0 - 2; i <= x1 + 1; i++) {
        chunky[(long)(y - 2) * pitch + i] = front_ink;
        chunky[(long)(y + 12) * pitch + i] = front_ink;
    }
    for (j = y - 2; j <= y + 12; j++) {
        chunky[(long)j * pitch + x0 - 2] = front_ink;
        chunky[(long)j * pitch + x1 + 1] = front_ink;
    }
    w = (x1 - x0) * permille / 1000;
    for (j = y; j < y + 10; j++) {
        memset(chunky + (long)j * pitch + x0, front_ink, (size_t)w);
        memset(chunky + (long)j * pitch + x0 + w, 0,
               (size_t)(x1 - x0 - w));
    }

    snprintf(line, sizeof line, "%d%%", permille / 10);
    menu_line(chunky, pitch, y + 22, line, 0);
    menu_line(chunky, pitch, y + 44, name, 0);
    amigagfx_blit(0, 0, SCREEN_W, SCREEN_H);

    /* ONE PICTURE OF IT, unasked for, the same way the title screen writes
     * its own evidence: this screen is only ever seen once on a machine and
     * a test that has to be watched is a test nobody runs. */
    if (!extract_dumped && permille > 400) {
        extract_dumped = 1;
        dump_frame(GTA_DIR "extract.raw", chunky, pitch, SCREEN_W, SCREEN_H,
                   front.ok ? front.pal : tiles.palette);
    }
}

/* Returns the number of tracks extracted. */
static int front_extract(unsigned char *chunky, int pitch)
{
    gta_iff_item item[GTA_IFF_MAX_ITEMS];
    gta_iff *job;
    int n, i, made = 0, aborted = 0;
    unsigned long t0;

    n = gta_iff_scan(GTA_DIR "GTADATA/", item, GTA_IFF_MAX_ITEMS);
    if (n <= 0)
        return 0;
    job = (gta_iff *)malloc(sizeof *job);
    if (!job) {
        log_line("gta: music extraction - no memory for the converter");
        return 0;
    }
    printf("gta: music extraction - %d track%s to convert\n",
           n, n == 1 ? "" : "s");
    fflush(stdout);
    front_palette_apply();
    t0 = amiga_uclock_us();

    for (i = 0; i < n && !aborted; i++) {
        const char *name = item[i].src, *p;
        int rc, last = -1;
        unsigned long ft0 = amiga_uclock_us();

        for (p = item[i].src; *p; p++)
            if (*p == '/' || *p == ':')
                name = p + 1;

        if (gta_iff_open(job, item[i].src, item[i].dst, 0)) {
            printf("gta: music extraction - cannot convert %s\n", item[i].src);
            fflush(stdout);
            continue;
        }
        printf("gta: music extraction - %s (%lu KB) -> %s, %lu samples\n",
               name, item[i].bytes / 1024UL, item[i].dst, job->out_total);
        fflush(stdout);
        extract_draw(chunky, pitch, i, n, name, 0);

        while ((rc = gta_iff_step(job)) > 0) {
            AmigaGfxEvent ev;
            int pm = gta_iff_permille(job);
            /* REDRAWN ON EACH WHOLE PERCENT, not on each step: the bar is a
             * full-screen c2p and a step is a 16 KB read, so drawing every
             * step would put more of the extraction into the display than
             * into the conversion. */
            if (pm / 10 != last) {
                last = pm / 10;
                extract_draw(chunky, pitch, i, n, name, pm);
            }
            while (amigagfx_poll(&ev)) {
                if (ev.type == AMIGAGFX_EV_QUIT)
                    aborted = 1;
                if (ev.type == AMIGAGFX_EV_KEY && !(ev.code & 0x80) &&
                    (ev.code & 0x7F) == KEY_ESC)
                    aborted = 1;
            }
            if (aborted)
                break;
        }
        if (rc < 0 || aborted) {
            gta_iff_abort(job);
            printf("gta: music extraction - %s %s, partial file removed\n",
                   name, aborted ? "abandoned" : "FAILED");
            fflush(stdout);
        } else {
            unsigned long us = amiga_uclock_us() - ft0;
            made++;
            extract_draw(chunky, pitch, i, n, name, 1000);
            /* MILLISECONDS, not seconds: a twenty-second test track converts
             * in well under one and "done in 0 s" says nothing about how
             * long the player's twelve-minute one will take. */
            printf("gta: music extraction - %s done in %lu ms (%lu KB/s)\n",
                   item[i].dst, us / 1000UL,
                   us > 0 ? (item[i].bytes / 1024UL) * 1000000UL / us : 0UL);
            fflush(stdout);
        }
    }
    free(job);
    printf("gta: music extraction - %d of %d track%s in %lu s%s\n",
           made, n, n == 1 ? "" : "s",
           (unsigned long)((amiga_uclock_us() - t0) / 1000000UL),
           aborted ? " (ESC)" : "");
    fflush(stdout);
    return made;
}

/* THE MENU. Returns 0 to play and 1 to quit. */
static int front_menu(unsigned char *chunky, int pitch)
{
    int sel = 0, frame = 0, done = 0, quit = 0;
    /* page 0 the main menu, 1 AMIGA OPTIONS: the settings that make the
     * game playable on a slower machine, changed here and written to
     * gta.prefs at once (gtaprefs.c edits the rest of that file) */
    int page = 0, osel = 0;
    unsigned long cap_t0, menu_t0;
    long menu_frames = 0;
    AmigaGfxEvent ev;

    if (!front.ok)
        return 0;
    /* Optional, like every other piece of converted art: no track anywhere,
     * no music, and the menu is otherwise unchanged. */
    front_music_start();
    front_palette_apply();
    printf("gta: front end - %d logo frames, menu font %s, bar pens %d/%d/%d\n",
           front.frames, have_menu_font ? "yes" : "no",
           bar_pen_text, bar_pen_fill, bar_pen_trim);
    fflush(stdout);

    cap_t0 = menu_t0 = amiga_uclock_us();
    while (!done) {
        int y0 = GTA_FRONT_UP + 34;
        int i;

        gta_front_draw(&front, chunky, pitch, 0, frame >> 3);
        if (page == 0) {
            for (i = 0; i < MENU_ITEMS; i++)
                menu_line(chunky, pitch, y0 + i * 22, menu_item[i], i == sel);
        } else {
            menu_line(chunky, pitch, y0 - 4, "AMIGA OPTIONS", 0);
            {
                char cl[24];
                snprintf(cl, sizeof cl, "CARS: %d", g_prefs.cars);
                menu_line(chunky, pitch, y0 + 20, cl, osel == 0);
            }
            menu_line(chunky, pitch, y0 + 42, "BACK", osel == 1);
        }
        /* The version, clear of the bottom edge - the menu font is fourteen
         * pixels tall and at SCREEN_H - 12 the last two rows were cut. */
        menu_line(chunky, pitch, SCREEN_H - 26, GTA_VERSION, 0);
        amigagfx_blit(0, 0, SCREEN_W, SCREEN_H);

        /* ONE PICTURE OF THE TITLE SCREEN, without anybody having to press
         * anything: the menu is the one part of this port a script cannot
         * reach, so it writes its own evidence. */
        if (frame == 24 || frame == 40)
            dump_frame(frame == 24 ? GTA_DIR "front0.raw"
                                   : GTA_DIR "front1.raw",
                       chunky, pitch, SCREEN_W, SCREEN_H, front.pal);
        if (++frame >= front.frames * 8)
            frame = 0;

        /* THE CAP, and it is the same one the interactive loop uses - see
         * FRAME_CAP_US. Without it this loop ran at whatever the machine
         * would give it, which starved Intuition and hung the game after a
         * while; it is the only loop in the port that had none. The
         * subtraction is unsigned because the microsecond clock is 32 bits
         * and wraps every 71 minutes. */
        while ((unsigned long)(amiga_uclock_us() - cap_t0)
                   < (unsigned long)FRAME_CAP_US)
            ;
        cap_t0 = amiga_uclock_us();
        menu_frames++;

        /* THE TITLE MUSIC. Serviced from THIS loop, because the front end
         * runs before the interactive one exists and a stream nobody refills
         * once a frame simply stops. */
        gta_audio_music_tick();

        while (amigagfx_poll(&ev)) {
            if (ev.type == AMIGAGFX_EV_QUIT) { quit = 1; done = 1; }
            if (ev.type != AMIGAGFX_EV_KEY || (ev.code & 0x80))
                continue;
            if (page == 1) {
                /* AMIGA OPTIONS: up/down pick; on CARS left/right step the
                 * limit by 4 and RETURN/SPACE step it up, wrapping 50 -> 6;
                 * ESC or BACK return. The fleet is the CPU on a slow
                 * Amiga, so this is the setting that buys frame rate. */
                int k = ev.code & 0x7F, step = 0;
                switch (k) {
                case KEY_UP:
                case KEY_DOWN:  osel ^= 1;                            break;
                case KEY_ESC:   page = 0;                             break;
                case KEY_LEFT:  if (osel == 0) step = -GTA_CARS_STEP; break;
                case KEY_RIGHT: if (osel == 0) step =  GTA_CARS_STEP; break;
                case KEY_RETURN:
                case KEY_SPACE:
                    if (osel == 1) { page = 0; break; }
                    step = GTA_CARS_STEP;
                    break;
                default: break;
                }
                if (step) {
                    g_prefs.cars += step;
                    if (g_prefs.cars > GTA_CARS_MAX) g_prefs.cars = GTA_CARS_MIN;
                    if (g_prefs.cars < GTA_CARS_MIN) g_prefs.cars = GTA_CARS_MAX;
                    gta_prefs_save(GTA_DIR, &g_prefs);
                    traffic.fleet_cap = g_prefs.cars;
                    printf("gta: amiga options - cars %d, saved\n", g_prefs.cars);
                    fflush(stdout);
                }
                continue;
            }
            switch (ev.code & 0x7F) {
            case KEY_UP:   sel = (sel + MENU_ITEMS - 1) % MENU_ITEMS; break;
            case KEY_DOWN: sel = (sel + 1) % MENU_ITEMS;              break;
            case KEY_ESC:  quit = 1; done = 1;                        break;
            case KEY_RETURN:
            case KEY_SPACE:
                if (sel == 1) { page = 1; osel = 0; break; }
                quit = (sel == MENU_QUIT); done = 1;                  break;
            default: break;
            }
        }
        /* AND THE RELOAD FILE, so the harness can restart a game that is
         * sitting on the title screen instead of waiting for a key that
         * nobody is going to press. */
        {
            FILE *rf = fopen(GTA_DIR "reload.txt", "r");
            if (rf) {
                fclose(rf);
                remove(GTA_DIR "reload.txt");
                g_reload = 1;
                quit = 1;
                done = 1;
            }
        }
    }
    /* WHAT THE CAP ACTUALLY DID. The menu is the one loop whose rate nobody
     * can read off a benchmark, so it says it on the way out. */
    {
        unsigned long us = amiga_uclock_us() - menu_t0;
        log_fps("gta: front end", (int)menu_frames, us);
    }
    gta_audio_music_stop();
    amigagfx_set_palette(tiles.palette, 0, 256);
    printf("gta: front end - %s\n", quit ? "quit" : "start");
    fflush(stdout);
    return quit;
}

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
     * text was still on the screen and no longer on the game.
     *
     * AND AT THE BOTTOM, not the top. The top left is the game's: the pager,
     * the weapon in hand and the armour go there in the original and this
     * readout was drawn straight over them. */
    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                 present_x() + render_w() - 2 - gta_hud_width(line),
                 HUD_DEBUG_Y, line);

    /* The DISPLAYED zoom, not v->zoom_px - at half resolution the renderer is
     * told half of it, and a readout that jumped from 32 to 16 when the
     * resolution key was pressed would read as the camera having moved. */
    p = gta_hud_int(line, zoom_eff());
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
    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                 present_x() + render_w() - 2 - gta_hud_width(line),
                 HUD_DEBUG_Y + 8, line);
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
/* ESC pressed in play: the QUIT GAME? card is up and the world stands still. */
static int g_quit_ask;
static unsigned int quit_poll;  /* frames under the card, for the reload poll */

/* THE PLAYER'S LIFE. Health 100, four lives at the start as the original
 * gives; armour is the pickup's three hits; a burning player loses a point
 * a tick for a hundred ticks. Nothing heals but the hospital. */
static int player_health = 100;
static int player_armour;
static int player_lives = 4;
static int player_burning;
/* THE SPEED-UP, powerup kinds 6, 7 and 8. The original asks
 * whether the player is in a car: on foot it starts a timer of 0x177 ticks
 * (375, fifteen seconds at the game's 25 Hz) and says "speed!"; in a car it
 * works on the CAR and says "car speed!" - and that one has no timer, the
 * car keeps it.
 *
 * THE DURATION IS THE ORIGINAL'S. THE MAGNITUDE IS OURS: the original sets
 * a field whose readers are not worked out, so how much faster is a guess
 * until somebody measures it. Doubling the
 * man's pace is what a drug called "speed" should feel like; a quarter more
 * top speed is what a car can take without the handling going with it. */
#define SPEED_TICKS 375
static int player_speed;        /* ticks of speed-up left, on foot */

/* ONE TICK OF THE ARROW. See script_arrow() for where the rule comes from.
 * Everything is in world pixels (half the original's units), so its 0x20 is
 * 16 and its step of 8 is 4. */
#define ARROW_BASE_FOOT 16
#define ARROW_STEP       4
/* HOW BIG THE ARROW IS, as a percentage of the style file's own sprite. The
 * original's arrow is 12x10 pixels, which at this port's scale is a smudge -
 * the developer asked for three times that and it is a marker rather than a
 * thing in the world, so nothing about the city's scale argues against it. */
#define ARROW_SCALE    300

/* The `0x20` / `(car_length + 1) * 6 + 0x20` half of the stand-off. */
static long arrow_base(int in_a_car, int car_len)
{
    return in_a_car ? ARROW_BASE_FOOT + (long)(car_len / 2 + 1) * 3
                    : ARROW_BASE_FOOT;
}

/* AND A QUARTER OF THE CAMERA HEIGHT, which is the bigger half of it -
 * the original adds a quarter of the camera's own z to the stand-off - the
 * same z its projection divides by.
 *
 * The original counts 64 units to a block in every axis, so its camera at
 * `n` grid levels is `n * 64` units up and a quarter of that is `n * 16`;
 * this port's world pixel is two of its units and `cam_h` is in QUARTER
 * levels, so the same quantity is `cam_h * 2` world pixels. At the shipped
 * camera of 8 levels that is 64 px - two blocks - which is why the arrow
 * floats well away from the player rather than sitting on his shoulder. */
static long arrow_standoff(gta_view *v, int in_a_car, int car_len)
{
    return arrow_base(in_a_car, car_len) + (long)gta_render_cam_h(v, 0) * 2;
}

static void arrow_tick(gta_view *v, long px, long py, int in_a_car,
                       int car_len)
{
    long dx, dy, want, base, arrow_standoff_v;
    int on_screen;

    if (!arrow_on)
        return;
    /* A CAR MOVES. ARROWCAR named one, so where it is has to be asked every
     * tick; when it is gone the arrow simply stays where it last saw it. */
    if (arrow_car_h) {
        long cx_, cy_;
        if (arrow_car_h == veh_serial && script_in_car) {
            arrow_tx = script_veh_x;
            arrow_ty = script_veh_y;
        } else if (gta_traffic_find_car(&traffic, arrow_car_h, &cx_, &cy_, 0, 0)) {
            arrow_tx = cx_;
            arrow_ty = cy_;
        }
    } else if (arrow_ped_h) {
        long cx_, cy_;
        if (gta_peds_find(&peds, arrow_ped_h, &cx_, &cy_, 0, 0)) {
            arrow_tx = cx_;
            arrow_ty = cy_;
        }
    }
    arrow_standoff_v = arrow_standoff(v, in_a_car, car_len);
    dx = arrow_tx - px;
    dy = arrow_ty - py;
    arrow_angle = (int)(gta_dir16(dx, dy) >> 16) & 255;

    base = arrow_base(in_a_car, car_len);

    /* ON SCREEN? The visible half-width in world pixels is half the render
     * width divided by the zoom's pixels per block, times 32 - which the
     * renderer already knows as its own reach. Compared per axis, like the
     * original's rectangle test. */
    {
        long hx = ((long)render_w() / 2) * 32 / (zoom_eff() > 0 ? zoom_eff() : 32);
        long hy = ((long)render_h() / 2) * 32 / (zoom_eff() > 0 ? zoom_eff() : 32);
        long adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        on_screen = (adx >> 16) <= hx && (ady >> 16) <= hy;
        want = 0;
        if (on_screen) {
            /* It settles half a block SHORT of the target, along whichever
             * axis is the longer - so `reach` is what is left after both the
             * stand-off and that last half block. */
            long m = (adx > ady ? adx : ady) >> 16;
            want = m - base - arrow_standoff_v;
            if (want < 0) want = 0;
        }
    }
    if (arrow_reach < want) {
        arrow_reach += ARROW_STEP;
        if (arrow_reach > want) arrow_reach = want;
    } else if (arrow_reach > want) {
        arrow_reach -= ARROW_STEP;
        if (arrow_reach < want) arrow_reach = want;
    }
}

/* Queue it. The arrow is a world sprite like everything else, so it scales
 * and sorts with the city instead of being pasted on the screen. */
static void arrow_draw(gta_view *v, long px, long py, int layer,
                       int in_a_car, int car_len)
{
    long r;
    if (!arrow_on || arrow_sprite < 0)
        return;
    r = arrow_standoff(v, in_a_car, car_len) + arrow_reach;
    /* OVER EVERYTHING, AND THREE TIMES THE SIZE - the developer's report:
     * *"strzalka jest za mala i jest rysowana pod samochodami. powinna byc z
     * 3x wieksza i byc rysowana NAD wszystkim"*.
     *
     * A layer of GTA_MAP_LAYERS is not a mistake: the renderer draws any
     * sprite whose layer is outside the map's own LAST, on top of everything,
     * and says so where it does it. The GRID stays the player's layer, so the
     * arrow is still projected at the right height and in the right place -
     * only the pass it is drawn in changes. */
    gta_render_add_sprite(v,
        px + (((long)gta_sin(arrow_angle) * r) >> 14 << 16),
        py - (((long)gta_cos(arrow_angle) * r) >> 14 << 16),
        GTA_MAP_LAYERS, layer, arrow_sprite, arrow_angle);
    gta_render_sprite_scale(v, ARROW_SCALE);
}

/* THE THREE DISPLAYS, drawn. See the note on brief_text for what each is.
 *
 * All three are the game's own art: the pager is a sprite, the icons are
 * sprites and the letters are GTA's .FON files. The port's 3x5 font is the
 * fall-back for a data set that has none of them. */
/* THE TEXT DISPLAYS' CLOCKS, ONCE PER SIMULATION TICK - not once per frame.
 *
 * They used to advance inside text_displays(), which is a DRAW and runs once
 * a frame. At 50-60 fps nobody could tell; on the pseudo-040 at ten frames a
 * second the world kept 66% of its pace (three ticks a frame) while the
 * pager crawled at a fifth of it, and the developer saw exactly that: *"gra
 * zwalnia troche. a za to pager zwalnia wielokrotnie. jakby sie frameskip
 * popsul"*. The pager's scroll, its blinking light, its seconds, the brief
 * box's life and the big card's are all sim time now, so they slow down and
 * speed up with the city and never on their own. */
static void red_arrow_draw(gta_view *v, long px, long py, int layer,
                           int in_a_car, int car_len)
{
    long r;
    int a;
    if (!red_on || arrow_sprite < 0)
        return;
    if ((px >> 21) == (red_tx >> 21) && (py >> 21) == (red_ty >> 21))
        return;
    a = (int)(gta_dir16(red_tx - px, red_ty - py) >> 16) & 255;
    r = arrow_standoff(v, in_a_car, car_len);
    gta_render_add_sprite_r(v,
        px + (((long)gta_sin(a) * r) >> 14 << 16),
        py - (((long)gta_cos(a) * r) >> 14 << 16),
        GTA_MAP_LAYERS, layer, arrow_sprite, a, RED_ARROW_REMAP);
    gta_render_sprite_scale(v, ARROW_SCALE);
}

static void text_displays_tick(void)
{
    if (pager_ticks > 0 && pager_line[0]) {
        int wide = have_pager ? gta_font_width(&pager_font, pager_line)
                              : gta_hud_width(pager_line);
        int win = PAGER_WIN_X1 - PAGER_WIN_X0;
        if (++pager_led >= 16) pager_led = 0;
        if (++pager_ticks & 1) {
            pager_px++;
            if (pager_px > win + wide)
                pager_ticks = 0;        /* scrolled off: gone */
            if (pager_secs > 0 && (pager_ticks % 50) == 0)
                pager_secs--;
        }
    }
    if (brief_ticks > 0 && brief_text[0])
        brief_ticks--;
    if (big_ticks > 0 && big_text[0])
        big_ticks--;
}

static void text_displays(unsigned char *chunky, int pitch)
{
    int px = present_x();
    /* The picture's own top and bottom rows - see the head of this change. */
    int py = present_y();
    int pb = present_bot();

    /* (b) THE PAGER, top left. The line scrolls right to left through the
     * device's window and the message is over when it has gone off the
     * left-hand end. */
    if (pager_ticks > 0 && pager_line[0]) {
        int win = PAGER_WIN_X1 - PAGER_WIN_X0;
        int tx = px + PAGER_WIN_X0 + win - pager_px;
        if (hud_icon >= 0)
            gta_hud_sprite(chunky, pitch, SCREEN_W, SCREEN_H, px, py,
                           &tiles, hud_icon + HUD_ICON_PAGER);
        if (have_pager)
            gta_font_draw_clip_shadow(&pager_font, chunky, pitch,
                                      SCREEN_W, SCREEN_H,
                                      tx, py + PAGER_WIN_Y, pager_line,
                                      px + PAGER_WIN_X0, py + PAGER_WIN_Y,
                                      px + PAGER_WIN_X1, py + 19);
        else
            gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                         px + 2, py + PAGER_WIN_Y, pager_line);
        /* THE LIGHT, blinking every five ticks as the original's does. */
        if (hud_icon >= 0 && (pager_led & 8))
            gta_hud_sprite(chunky, pitch, SCREEN_W, SCREEN_H,
                           px + PAGER_LED_X, py + PAGER_LED_Y, &tiles,
                           hud_icon + HUD_ICON_PAGER_LED);
        /* A TIMED LINE counts its seconds down beside the device. */
        if (pager_secs >= 0) {
            char n[8];
            *gta_hud_int(n, pager_secs) = 0;
            gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                         px + 84, 8, n);
        }
        /* (the scroll, the light and the seconds advance in
         * text_displays_tick(), on the simulation clock) */
    }

    /* (a) THE BRIEF BOX, bottom left: the icon sits ON THE BOTTOM EDGE and
     * the text is WRAPPED beside it, as many lines as it needs, STACKED
     * UPWARDS - `screen_height - ((lines - 1) * spacing + 1)` in
     * the original. A job's brief is a sentence, not a caption. */
    if (brief_ticks > 0 && brief_text[0]) {
        static const unsigned char icon_of[6] = { 11, 10, 12, 14, 13, 15 };
        int ih = 0, iw = 0;
        int k = brief_kind >= 0 && brief_kind < 6 ? brief_kind : 3;
        int lh = have_pager ? pager_font.height + 1 : 8;
        int wrap, nlines = 0, start = 0, i2;
        short brk[8];
        if (hud_icon >= 0) {
            ih = gta_hud_sprite_h(&tiles, hud_icon + icon_of[k]);
            iw = gta_hud_sprite_w(&tiles, hud_icon + icon_of[k]);
        }
        wrap = render_w() - iw - 4;
        /* WHERE THE LINES BREAK: greedily, on spaces, measured in the font
         * that will draw them. Eight lines is more than any text in the
         * file needs and the ninth is simply not shown. */
        while (brief_text[start] && nlines < 8) {
            int e = start, last = -1;
            while (brief_text[e]) {
                int q = e;
                char save;
                while (brief_text[q] && brief_text[q] != ' ') q++;
                save = brief_text[q];
                brief_text[q] = 0;
                {
                    int wpx = have_pager
                            ? gta_font_width(&pager_font, brief_text + start)
                            : gta_hud_width(brief_text + start);
                    brief_text[q] = save;
                    if (wpx > wrap && last >= 0)
                        break;
                }
                last = q;
                if (!brief_text[q]) break;
                e = q + 1;
            }
            if (last < 0) break;
            brk[nlines++] = (short)last;
            start = last;
            while (brief_text[start] == ' ') start++;
        }
        if (ih)
            gta_hud_sprite(chunky, pitch, SCREEN_W, SCREEN_H, px,
                           pb - ih, &tiles, hud_icon + icon_of[k]);
        start = 0;
        for (i2 = 0; i2 < nlines; i2++) {
            char save = brief_text[brk[i2]];
            int y = pb - 2 - (nlines - i2) * lh;
            brief_text[brk[i2]] = 0;
            if (have_pager)
                gta_font_draw_shadow(&pager_font, chunky, pitch,
                                     SCREEN_W, SCREEN_H,
                                     px + iw + 3, y, brief_text + start);
            else
                gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                             px + iw + 3, y + 3, brief_text + start);
            brief_text[brk[i2]] = save;
            start = brk[i2];
            while (brief_text[start] == ' ') start++;
        }
        /* (brief_ticks runs down in text_displays_tick()) */
    }

    /* (c) THE BIG MESSAGE - UP TO THREE WORDS, ONE PER LINE, each centred.
     * The original keeps them in three separate buffers with a word count
     * separately, and that is the only
     * reason "MISSION COMPLETE!" fits across a 320-pixel screen in a font
     * this size. */
    if (big_ticks > 0 && big_text[0]) {
        int lh = have_big ? big_font.height + 2 : 26;
        int word[3], n = 0, i2, at = 0;
        while (big_text[at] && n < 3) {
            while (big_text[at] == ' ') at++;
            if (!big_text[at]) break;
            word[n++] = at;
            while (big_text[at] && big_text[at] != ' ') at++;
        }
        for (i2 = 0; i2 < n; i2++) {
            int end = word[i2];
            char save;
            int bw, y;
            while (big_text[end] && big_text[end] != ' ') end++;
            save = big_text[end];
            big_text[end] = 0;
            bw = have_big ? gta_font_width(&big_font, big_text + word[i2])
                          : gta_hud_width_big(big_text + word[i2], 3);
            y = py + present_h() / 2 - (n * lh) / 2 + i2 * lh;
            if (have_big)
                gta_font_draw_shadow(&big_font, chunky, pitch,
                                     SCREEN_W, SCREEN_H,
                                     px + (render_w() - bw) / 2, y,
                                     big_text + word[i2]);
            else
                gta_hud_text_big(chunky, pitch, SCREEN_W, SCREEN_H,
                                 px + (render_w() - bw) / 2, y,
                                 big_text + word[i2], 3);
            big_text[end] = save;
        }
        /* (big_ticks runs down in text_displays_tick()) */
    }
}

static void hud_score(unsigned char *chunky, int pitch)
{
    char line[24];
    char *p;
    int right = present_x() + render_w() - 2;
    int top   = present_y();

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
                      right - gta_font_width(&score_font, digits),
                      top + 1, digits);
    } else
    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                 right - gta_hud_width(line), top + 2, line);

    /* THE MULTIPLIER, under the score and right-aligned with it. It only
     * ever goes up - a job, or the crate - and it multiplies everything the
     * player earns, so it belongs beside the number it acts on. */
    if (score.multiplier > 1) {
        char m[8];
        char *q = m;
        *q++ = 'x';
        q = gta_hud_int(q, score.multiplier);
        *q = 0;
        if (have_mult) {
            char g[8];
            int k2;
            /* missmul1.fon holds ELEVEN glyphs where score1.fon holds ten:
             * the digits, and then one more. The one more is the 'x', which
             * is the only other character a multiplier is ever written
             * with - so digits map to '!'.. and 'x' to the eleventh, '+'. */
            for (k2 = 0; m[k2]; k2++)
                g[k2] = m[k2] >= '0' && m[k2] <= '9'
                      ? (char)(m[k2] - '0' + '!')
                      : (char)('!' + 10);
            g[k2] = 0;
            gta_font_draw(&mult_font, chunky, pitch, SCREEN_W, SCREEN_H,
                          right - gta_font_width(&mult_font, g),
                          top + (have_score_font ? score_font.height + 1 : 10),
                          g);
        } else {
            gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                         right - gta_hud_width(m),
                         top + (have_score_font ? 14 : 10), m);
        }
    }

    /* THE WEAPON USED TO BE SPELLED OUT HERE. It is an ICON on the left now,
     * where the original puts it, so this line is only for the case where
     * the style file has no HUD sprites to draw. */
    if (hud_icon < 0) {
        const char *n = weapon_name[hud_weapon >= 0 && hud_weapon < 5
                                    ? hud_weapon : 0];
        const char *q;
        p = line;
        for (q = n; *q; q++) *p++ = *q;
        if (hud_weapon != 0) {
            *p++ = ' ';
            p = gta_hud_int(p, hud_ammo);
        }
        *p = 0;
        gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                     right - gta_hud_width(line),
                     top + (have_score_font ? 14 : 10), line);
    }

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
                 right - gta_hud_width(line),
                 top + (have_score_font ? 22 : 18), line);

    /* THE FRENZY'S CLOCKS, in whole seconds, under the rest - the original's routine
     * draws each while it is not -1, the timed one first. */
    {
        int k, row = 0;
        for (k = 0; k < 2; k++) {
            if (kf_ticks[k] < 0)
                continue;
            p = gta_hud_int(line, kf_ticks[k] / 25);
            *p = 0;
            gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                         right - gta_hud_width(line),
                         top + (have_score_font ? 30 : 26) + row * 8, line);
            row++;
        }
    }

    text_displays(chunky, pitch);

    if (bust_timer > 0) {
        const char *t = card_text[card_kind & 3];
        int bw = have_big ? gta_font_width(&big_font, t) : gta_hud_width_big(t, 4);
        if (have_big)
            gta_font_draw_shadow(&big_font, chunky, pitch, SCREEN_W, SCREEN_H,
                                 present_x() + (render_w() - bw) / 2,
                                 top + present_h() / 2 - big_font.height / 2,
                                 t);
        else
            gta_hud_text_big(chunky, pitch, SCREEN_W, SCREEN_H,
                             present_x() + (render_w() - bw) / 2,
                             top + present_h() / 2 - 12, t, 4);
    }

    /* ESC ASKS FIRST (218): the game stands still under this card until Y or
     * RETURN quits and N or ESC goes back. It used to quit on the spot. */
    if (g_quit_ask) {
        static const char *const ql[2] = { "QUIT GAME?", "Y / N" };
        int k, h = have_big ? big_font.height : 24;
        for (k = 0; k < 2; k++) {
            int bw = have_big ? gta_font_width(&big_font, ql[k])
                              : gta_hud_width_big(ql[k], 4);
            int y = top + present_h() / 2 - h + k * (h + 4);
            if (have_big)
                gta_font_draw_shadow(&big_font, chunky, pitch, SCREEN_W, SCREEN_H,
                                     present_x() + (render_w() - bw) / 2, y,
                                     ql[k]);
            else
                gta_hud_text_big(chunky, pitch, SCREEN_W, SCREEN_H,
                                 present_x() + (render_w() - bw) / 2, y,
                                 ql[k], 4);
        }
    }

    /* THE WANTED LEVEL: that many heads across the top middle, each one
     * flashing every other frame, as the original's are. Nothing at level
     * 0 - an empty row would be a readout of nothing. */
    /* THE WANTED LEVEL: the original's own cop head, sprites 16 and 17 of
     * the HUD set, CENTRED ALONG THE TOP - `iVar17 = (width - n * w) / 2`
     * and y = 0. Each head keeps its OWN flash timer and
     * changes state every other frame, which is why the row shimmers rather
     * than blinking in unison. */
    if (score.level > 0) {
        static unsigned char head_on[4], head_t[4];
        int n = score.level > 4 ? 4 : score.level, k;
        int iw = hud_icon >= 0
               ? gta_hud_sprite_w(&tiles, hud_icon + HUD_ICON_COP) + 1
               : GTA_HUD_COP_W;
        int x = present_x() + render_w() / 2 - (n * iw) / 2;
        for (k = 0; k < n; k++) {
            if (hud_icon >= 0)
                gta_hud_sprite(chunky, pitch, SCREEN_W, SCREEN_H,
                               x + k * iw, top, &tiles,
                               hud_icon + HUD_ICON_COP + (head_on[k] ? 1 : 0));
            else if ((head_t[k] & 2) == 0)
                gta_hud_cop(chunky, pitch, SCREEN_W, SCREEN_H,
                            x + k * GTA_HUD_COP_W, top + 2);
            if (++head_t[k] >= 2) {
                head_t[k] = 0;
                head_on[k] = (unsigned char)!head_on[k];
            }
        }
    }

    /* THE LEFT-HAND COLUMN, in the original's order: the weapon in hand with
     * its ammunition, then the things you are carrying - armour, the
     * get-out-of-jail-free card - each a two-frame pair that flashes. */
    if (hud_icon >= 0) {
        static unsigned char ind_t, ind_on;
        int x = present_x() + 1;
        /* UNDER THE PAGER when the pager is showing - the original takes
         * this y from the device's own height - the pager sprite's, and 0 when it
         * is not up. */
        int y = top + (pager_ticks > 0
              ? gta_hud_sprite_h(&tiles, hud_icon + HUD_ICON_PAGER) + 1 : 2);
        if (++ind_t >= 8) { ind_t = 0; ind_on = (unsigned char)!ind_on; }
        if (hud_weapon > 0 && hud_weapon <= 4) {
            char n[8];
            int iw = gta_hud_sprite_w(&tiles, hud_icon + HUD_ICON_WEAPON
                                              + hud_weapon - 1);
            gta_hud_sprite(chunky, pitch, SCREEN_W, SCREEN_H, x, y, &tiles,
                           hud_icon + HUD_ICON_WEAPON + hud_weapon - 1);
            *gta_hud_int(n, hud_ammo) = 0;
            gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                         x + iw + 2, y + 6, n);
            y += gta_hud_sprite_h(&tiles, hud_icon + HUD_ICON_WEAPON
                                          + hud_weapon - 1) + 2;
        }
        if (player_armour > 0) {
            gta_hud_sprite(chunky, pitch, SCREEN_W, SCREEN_H, x, y, &tiles,
                           hud_icon + HUD_ICON_ARMOUR + (ind_on ? 1 : 0));
            y += gta_hud_sprite_h(&tiles, hud_icon + HUD_ICON_ARMOUR) + 2;
        }
        if (jail_free) {
            gta_hud_sprite(chunky, pitch, SCREEN_W, SCREEN_H, x, y, &tiles,
                           hud_icon + HUD_ICON_KEY + (ind_on ? 1 : 0));
            y += gta_hud_sprite_h(&tiles, hud_icon + HUD_ICON_KEY) + 2;
        }
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
    gta_hud_text(chunky, pitch, SCREEN_W, SCREEN_H,
                 present_x() + render_w() - 2 - gta_hud_width(line),
                 HUD_DEBUG_Y + 16, line);
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
    {
        /* The bars, on all four sides now. Cleared every frame: the renderer
         * only clears its own rectangle, so a zoom or a mode change can leave
         * the previous frame's edges lying in them. */
        int rx = present_x(), rw = present_w();
        int ry = present_y(), rh = present_h();
        if (rx > 0 || rw < SCREEN_W || ry > 0 || rh < SCREEN_H) {
            int y;
            for (y = 0; y < SCREEN_H; y++) {
                unsigned char *row = g_chunky + (long)y * g_pitch;
                if (y < ry || y >= ry + rh) {
                    memset(row, 0, SCREEN_W);
                    continue;
                }
                memset(row, 0, rx);
                memset(row + rx + rw, 0, SCREEN_W - rx - rw);
            }
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
            amigagfx_blit(present_x(), present_y(),
                          present_w(), present_h());
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
/* A megabyte of samples. Static for the same reason the map is: it is memory
 * the linker accounts for rather than a surprise at the far end of a load. */
static gta_sfx   sfx;
static gta_weapons weapons;
static gta_view  view;
static gta_player player;

/* ---- WHAT A JOB DOES TO CARS ---------------------------------------------
 *
 * A mission is nearly always "steal that car and bring it here", so the
 * script has to be able to make one car and be asked about that same car
 * later. The fleet's serial is the handle and it survives the player driving
 * it about (gta_traffic_rename_car). These are the world half of
 * gta_script_world; they live down here rather than with the powerup
 * callbacks because they need `tiles`. */
static unsigned long script_car_on(void *ctx, int line, int model_id,
                                   int bx, int by, int angle)
{
    int rec, lz;
    unsigned long h;
    (void)ctx;
    rec = gta_script_model_index(&tiles, model_id);
    if (rec < 0) {
        printf("gta: script - line %d wants model %d and this style has no "
               "such car\n", line, model_id);
        fflush(stdout);
        return 0;
    }
    lz = gta_script_stand_layer(&nav, bx, by);
    if (lz < 0) lz = 0;
    h = gta_traffic_abandon(&traffic, rec,
                            (((long)bx * 32 + 16) << 16),
                            (((long)by * 32 + 16) << 16),
                            angle, lz, -1, 0);
    if (h)
        gta_traffic_set_mission(&traffic, h, 1);
    printf("gta: script - car line %d (model %d, record %d) put at (%d,%d), "
           "handle %lu\n", line, model_id, rec, bx, by, h);
    fflush(stdout);
    return h;
}

static int script_car_pos(void *ctx, unsigned long h, long *wx, long *wy)
{
    (void)ctx;
    /* THE CAR THE PLAYER HAS is not in the fleet - grab_car took it out the
     * tick he pressed RETURN, and it does not come back until he gets out.
     * While the get-in animation runs there is no `veh` yet either, so the
     * best answer is where HE is: he is standing at its door. */
    if (h && h == veh_serial) {
        *wx = script_in_car ? script_veh_x : script_pl_x;
        *wy = script_in_car ? script_veh_y : script_pl_y;
        return 1;
    }
    return gta_traffic_find_car(&traffic, h, wx, wy, 0, 0);
}

static int script_car_dead(void *ctx, unsigned long h)
{
    long x_, y_;
    int wr_ = 0;
    (void)ctx;
    if (!h)
        return 0;
    /* AND IT IS NOT DEAD JUST BECAUSE THE FLEET HAS NOT GOT IT. See
     * script_car_pos: `veh_serial` is set at the GRAB, forty ticks before
     * `in_car`, and asking the fleet in between reported every mission car
     * the player walked up to as wrecked. */
    if (h == veh_serial)
        return script_in_car && script_veh_damage >= 100;
    /* GONE IS DEAD, which is the original's rule too: it reports zero health
     * for a car whose handle is -1. */
    if (!gta_traffic_find_car(&traffic, h, &x_, &y_, 0, &wr_)) {
        static unsigned long said;
        if (said != h) {
            said = h;
            printf("gta: script - car %lu is GONE from the fleet (%d cars, "
                   "player has %lu)\n", h, traffic.n, veh_serial);
            fflush(stdout);
        }
        return 1;
    }
    return wr_;
}

static unsigned long script_player_car(void *ctx)
{
    (void)ctx;
    return script_in_car ? veh_serial : 0UL;
}

/* MISSION_END: the multiplier goes up by one and stays up. That is the
 * whole reward structure of GTA 1 - a job pays, and every job after it pays
 * more. */
static void script_mission_done(void *ctx)
{
    (void)ctx;
    score.multiplier++;
    printf("gta: script - mission complete, multiplier x%d\n",
           score.multiplier);
    fflush(stdout);
}

static int script_player_car_at(void *ctx, long *wx, long *wy, int *stopped)
{
    (void)ctx;
    if (!script_in_car)
        return 0;
    *wx = script_veh_x;
    *wy = script_veh_y;
    /* "Stopped" at the original's own reading of it: under a quarter of a
     * world pixel a tick, which is a car that has come to rest rather than
     * one merely crawling. */
    *stopped = script_veh_speed < (1L << 14);
    return 1;
}

static int script_player_car_model(void *ctx)
{
    (void)ctx;
    if (!script_in_car || script_veh_model < 0)
        return -1;
    return tiles.cars[script_veh_model].model_id;
}

/* RESET has finished with this car: the fleet may sweep it up now. */
static void script_car_release(void *ctx, unsigned long h)
{
    (void)ctx;
    if (h) {
        gta_traffic_set_mission(&traffic, h, 0);
        if (h == veh_serial)
            veh_mission = 0;
        if (h == arrow_car_h)
            arrow_car_h = 0;
    }
}

static void script_arrow_car(void *ctx, unsigned long h)
{
    (void)ctx;
    if (!h)
        return;
    arrow_car_h = h;
    arrow_ped_h = 0;
    arrow_on = 1;
    arrow_reach = 0;
}

/* ---- THE JOB'S OWN PEOPLE, the world half of gta_script_world. ---- */
static unsigned long script_ped_on(void *ctx, int line, long wx, long wy,
                                   int angle)
{
    int lz = gta_script_stand_layer(&nav, (int)(wx >> 21), (int)(wy >> 21));
    unsigned long h;
    (void)ctx;
    if (lz < 0) lz = 0;
    h = gta_peds_spawn_mission(&peds, wx, wy, lz, angle);
    printf("gta: script - ped line %d at (%ld,%ld) layer %d, handle %lu\n",
           line, wx >> 16, wy >> 16, lz, h);
    fflush(stdout);
    return h;
}

static int script_ped_pos(void *ctx, unsigned long h, long *wx, long *wy)
{
    (void)ctx;
    return gta_peds_find(&peds, h, wx, wy, 0, 0);
}

static int script_ped_dead(void *ctx, unsigned long h)
{
    int alive = 0;
    (void)ctx;
    if (!h)
        return 0;
    if (!gta_peds_find(&peds, h, 0, 0, 0, &alive))
        return 1;                       /* gone is dead, as for a car */
    return !alive;
}

static void script_arrow_ped(void *ctx, unsigned long h)
{
    (void)ctx;
    if (!h)
        return;
    arrow_ped_h = h;
    arrow_car_h = 0;
    arrow_on = 1;
    arrow_reach = 0;
}

/* PED_BACK: he gets in when the player brings the right car to him. The
 * reach is a car length either way - `GTA_PED_INTO_CAR_PX` - which is close
 * enough that you have to stop AT him and loose enough that you do not have
 * to be exact. */
#define GTA_PED_INTO_CAR_PX 34

static int script_ped_into_car(void *ctx, unsigned long ped, unsigned long car)
{
    long px_, py_, dx, dy;
    (void)ctx;
    if (!ped || !car || !script_in_car || veh_serial != car)
        return 0;
    if (!gta_peds_find(&peds, ped, &px_, &py_, 0, 0))
        return 0;
    dx = script_veh_x - px_; if (dx < 0) dx = -dx;
    dy = script_veh_y - py_; if (dy < 0) dy = -dy;
    if (dx > ((long)GTA_PED_INTO_CAR_PX << 16) ||
        dy > ((long)GTA_PED_INTO_CAR_PX << 16))
        return 0;
    if (arrow_ped_h == ped) {
        arrow_ped_h = 0;
        arrow_on = 0;
    }
    return gta_peds_take_mission(&peds, ped);
}

/* THE PLAYER'S OWN STATE, for a job's guards. "Arrested" is the BUSTED
 * card being up, which is the whole of what the port has of an arrest. */
/* THE SPRAY SHOP does two things and the second is the point of it: the car
 * changes colour, and the heat goes with the paint. A police force looking
 * for a red saloon stops looking when it becomes a blue one. */
static int script_respray(void *ctx, int remap)
{
    (void)ctx;
    if (!script_in_car)
        return 0;
    script_respray_to = remap;
    gta_audio_play(&sfx, GTA_SND_RESPRAY, 64, 0);
    return 1;
}

static int script_player_arrested(void *ctx)
{
    (void)ctx;
    return bust_timer > 0 && card_kind == 1;
}

static int script_player_dead(void *ctx)
{
    (void)ctx;
    return player_health <= 0;
}

static void script_ped_kill(void *ctx, unsigned long h)
{
    (void)ctx;
    if (arrow_ped_h == h) { arrow_ped_h = 0; arrow_on = 0; }
    gta_peds_take_mission(&peds, h);
}

/* ---- WHAT THE SECOND JOB ASKS OF THE WORLD ---------------------------- */

/* GENERAL_ONSCREEN. The view's own rectangle, in world pixels, with a block
 * of margin either side - the original's test is against the screen rect and
 * a thing on the very edge is on screen. */
static int script_onscreen(void *ctx, long wx, long wy)
{
    int z = zoom_eff() > 0 ? zoom_eff() : 32;
    long hx = ((long)render_w() / 2) * 32 / z;
    long hy = ((long)render_h() / 2) * 32 / z;
    long dx = wx - view.cam_x, dy = wy - view.cam_y;
    (void)ctx;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    return (dx >> 16) <= hx && (dy >> 16) <= hy;
}

/* DROP_WANTED_LEVEL: the heat goes and the police give up - the same thing
 * the spray shop does to a car, done to the man. */
static void script_drop_wanted(void *ctx)
{
    (void)ctx;
    if (score.heat || score.level) {
        printf("gta: script - the wanted level is dropped (was %d)\n",
               score.level);
        fflush(stdout);
    }
    gta_score_clear_heat(&score);
    gta_traffic_cops_give_up(&traffic);
    gta_traffic_police_reset(&traffic);
}

static void script_ped_remap(void *ctx, unsigned long h, int remap)
{
    (void)ctx;
    gta_peds_set_remap(&peds, h, remap);
}

/* CHANGE_PED_TYPE. The original's AI numbering: 0x15..0x2e are the family
 * that takes a target and goes for it, and everything else this port has no
 * separate behaviour for stands where it is. */
static void script_ped_type(void *ctx, unsigned long h, int type,
                            long tx, long ty, int on_player)
{
    int mode = (type >= 0x15 && type <= 0x2e) ? GTA_PED_MODE_HUNT
                                              : GTA_PED_MODE_MISSION;
    (void)ctx;
    printf("gta: script - ped %lu becomes type %d (%s%s)\n", h, type,
           mode == GTA_PED_MODE_HUNT ? "hunting" : "standing",
           on_player ? " the player" : "");
    fflush(stdout);
    gta_peds_mission_ai(&peds, h, mode, tx, ty, on_player);
}

static void script_ped_sendto(void *ctx, unsigned long h, long wx, long wy)
{
    (void)ctx;
    gta_peds_mission_ai(&peds, h, GTA_PED_MODE_SENDTO, wx, wy, 0);
}

/* WAIT_FOR_PED. Half a block, which is the original's own "at that block". */
static int script_ped_at(void *ctx, unsigned long h, long wx, long wy)
{
    (void)ctx;
    return gta_peds_at(&peds, h, wx, wy, 16);
}

/* HELL_ON: the car goes down like any other mission car and then somebody is
 * put in the driving seat, so taking it is a carjacking rather than a walk-up.
 * It does not drive off - three of them are parked side by side. */
static unsigned long script_car_on_driven(void *ctx, int line, int model_id,
                                          int bx, int by, int angle)
{
    unsigned long h = script_car_on(ctx, line, model_id, bx, by, angle);
    if (h) {
        gta_traffic_set_driver(&traffic, h, 1);
        printf("gta: script - ...with a driver in it\n");
        fflush(stdout);
    }
    return h;
}

static long script_score_now(void *ctx)
{
    (void)ctx;
    return score.score;
}

/* EXPLODE / PLAIN_EXPL_BUILDING - see gta_weapons_explode_face(). The layer
 * is the one a man would stand on beside that wall, which is where the blast
 * and its two fires have to be for anybody to be hurt by them. */
static void script_explode(void *ctx, int line, int bx, int by, int face,
                           int debris)
{
    static const int fdx[4] = { -1, 1, 0, 0 }, fdy[4] = { 0, 0, -1, 1 };
    long wx = 0, wy = 0;
    int lz = gta_script_stand_layer(&nav, bx + fdx[face & 3], by + fdy[face & 3]);
    (void)ctx;
    /* A face that looks into a courtyard or another building has nowhere to
     * stand beside it (the bank has five such: the first run logged layer
     * -1 for them and they were drawn nowhere). The block's own standing
     * layer, then the player's, so the blast is always somewhere visible. */
    if (lz < 0) lz = gta_script_stand_layer(&nav, bx, by);
    if (lz < 0) lz = player.layer;
    gta_weapons_explode_face(&weapons, bx, by, lz, face, &peds, &traffic,
                             &score, 0, &wx, &wy);
    printf("gta: script - %s line %d: block (%d,%d) face %d, blast at "
           "(%ld,%ld) layer %d\n", debris ? "EXPLODE" : "PLAIN_EXPL_BUILDING",
           line, bx, by, face, wx >> 16, wy >> 16, lz);
    fflush(stdout);
}

/* STOP_FRENZY - the endless weapon's clock is in the main loop, so this only
 * asks; the loop runs the clock out on its next tick and the ordinary
 * "back to what he carried" path does the rest. */
static int script_frenzy_stop_req = 0;
static void script_frenzy_stop(void *ctx)
{
    (void)ctx;
    script_frenzy_stop_req = 1;
    printf("gta: script - STOP_FRENZY\n");
    fflush(stdout);
}

/* SAY - a line of the speech bank for the script (gta_script_world.say). */
static void script_say(void *ctx, int line)
{
    (void)ctx;
    gta_audio_speak(&voice, line, 64, 1);
}

/* THE KILL FRENZY'S TWO CLOCKS - player+0x194 and +0x196 in the original,
 * set by the KF_* commands, run down once a logic tick (25 a second) by
 * the original's routine until they reach -1, and drawn by the HUD (the original's routine) as
 * seconds under the score, one below the other, while they are not -1. They
 * decide nothing: the script's own SURVIVE does the timing. kf_ticks is
 * declared beside `voice`, because the HUD draws it. */
static void script_kf_timer(void *ctx, int which, long ticks)
{
    (void)ctx;
    if (which < 0 || which > 1) return;
    kf_ticks[which] = ticks < 0 ? -1 : (ticks > 32767 ? 32767 : (int)ticks);
    printf("gta: script - frenzy clock %d %s %d s\n", which,
           ticks < 0 ? "off," : "set to", ticks < 0 ? 0 : (int)(ticks / 25));
    fflush(stdout);
}

/* THE VOICE'S DICE - the original's own generator, the original's table =
 * (x * 0x79 + 1) % 0x800, shared by every speech call that picks a line. */
static int voice_lcg;
static int voice_rand(void)
{
    voice_lcg = (voice_lcg * 0x79 + 1) % 0x800;
    return voice_lcg;
}

/* THE ENGINE'S RATE - the original's routine's switch on the car info's
 * `sound_function`, `v` the car's signed speed in the original's units
 * (car+0x1c: VEH_SPEED_UNIT, half a world pixel a physics step):
 *   0  a table by a sawtooth "gear", v / 10 + v % 10 + 1, capped at 12
 *      (the original's routine; reversing indexes it by -v); sample 0x36 twice as fast
 *   1  the same, halved
 *   2  22050 + 1400 v, or + 400 |v| reversing (the original's routine); 3 three times
 *   4  4000 + 2000 |v| (the original's routine - the tank)
 *   5  6000 + 333 v below 8, 750 v below 12, 500 v below 18, 333 v above;
 *      6000 + 500 |v| reversing (the original's routine - the buses)
 * and three times whatever it is while the car is in the air (+0x109).
 * The volume, the original's routine: 40% at a standstill, 40 + 1.5 v % above,
 * never over 100. */
static const long engine_gear_hz[13] = {
    18000L, 33000L, 41000L, 48000L, 53000L, 58000L, 62000L,
    66000L, 69500L, 72000L, 74000L, 76500L, 77000L
};
static long engine_rate(int sound_function, int sample, int v, int air,
                        int *pct)
{
    long hz;
    int a = v < 0 ? -v : v, g;

    switch (sound_function) {
    case 0:
    case 1:
        if (v == 0)      g = 0;
        else if (v < 0)  g = a > 12 ? 12 : a;
        else { g = v / 10 + v % 10 + 1; if (g > 12) g = 12; }
        hz = engine_gear_hz[g];
        if (sound_function == 1) hz /= 2;
        else if (sample == 0x36) hz *= 2;
        break;
    case 2:
    case 3:
        hz = 22050L + (v < 0 ? 400L * a : 1400L * v);
        if (sound_function == 3) hz *= 3;
        break;
    case 4:
        hz = 4000L + 2000L * a;
        break;
    case 5:
        if (v < 0)        hz = 6000L + 500L * a;
        else if (v < 8)   hz = 6000L + 333L * v;
        else if (v < 12)  hz = 750L * v;
        else if (v < 18)  hz = 500L * v;
        else              hz = 333L * v;
        break;
    default:
        hz = 4000L;         /* the original's sample 0x84 - not in the bank */
        break;
    }
    if (air)
        hz *= 3;
    *pct = v < 1 ? 40 : (40 + v * 3 / 2 > 100 ? 100 : 40 + v * 3 / 2);
    return hz;
}

/* THE COVER TEST - the original's routine(x, y, z), which every positional voice, the
 * one-shot dispatcher and the car sound loop ask before they play, and HALVE
 * the volume when it says yes (214). The original's z grows DOWNWARD (0 the
 * top layer; a falling car adds to it) and its column is stored top-down, so
 * its level iz is this port's 5 - z: the block the source is IN - the one
 * whose ground type it walks on, whose own lid would be over its head (the
 * road it stands on is the lid of the block below; see gtadump column at
 * (106,114)). Yes when that block has a lid that is neither flat nor a slope
 * (type bits 0x3f80), or when any block ABOVE has a lid that is not flat
 * (0x80). So: a roof, a bridge or a tunnel over the source halves it; the
 * open street does not. */
static int covered(long x, long y, int layer)
{
    int bx = (int)(x >> 21), by = (int)(y >> 21), z;
    gta_block b;
    if (layer < 0) layer = 0;
    if (gta_map_block(&map, bx, by, layer, &b) &&
        b.faces[GTA_FACE_LID] != 0 && (b.type_map & 0x3f80) == 0)
        return 1;
    for (z = layer + 1; z < GTA_MAP_LAYERS; z++)
        if (gta_map_block(&map, bx, by, z, &b) &&
            b.faces[GTA_FACE_LID] != 0 && (b.type_map & 0x80) == 0)
            return 1;
    return 0;
}

/* A COP SHOUTS - the original's routine(x, y, z, kind, ped), rule for rule:
 *   - nothing while a line is being said, and never the same kind twice in a
 *     row (the original's table holds the last);
 *   - kinds 0x12..0x14 each have a countdown of CALLS (the original's table[]): a
 *     call while it is above zero only counts it down, and a shout sets it
 *     to 50; 0x15 (the arrest) has none;
 *   - only within 0x3ffffff of the player in |dx|+|dy| - sixteen blocks - and
 *     louder the nearer, the original's routine's (range - d) >> 19 out of 127;
 *   - the line is kind + (ped % 3) * 4: three cops' voices of four shouts.
 * The original halves the volume when the original's routine finds the point covered;
 * the port does not ask that yet. Coordinates are the port's 16.16 pixels at
 * 32 a block, so the range is 0x1ffffff and the volume comes out 0..63. */
static int shout_last = -1;
static int shout_cool[3];
static void cop_shout(long x, long y, int layer, long px, long py, int kind,
                      int ped)
{
    long dx = x - px, dy = y - py, d;
    int k = kind - 0x12;

    if (gta_audio_speaking() || kind == shout_last)
        return;
    if (kind != 0x15) {
        if (k < 0 || k > 2)
            return;
        if (shout_cool[k]-- > 0)
            return;
    }
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    d = dx + dy;
    if (d >= 0x1ffffffL)
        return;
    {
        int vol = (int)((0x1ffffffL - d) >> 19);
        if (covered(x, y, layer)) vol >>= 1;
        gta_audio_speak(&voice, kind + (ped % 3) * 4, vol, 0);
    }
    shout_last = kind;
    if (kind != 0x15)
        shout_cool[k] = 50;
}

/* SOMEBODY IN THE STREET YELLS - the original's routine(x, y, z): lines 30..50. Its
 * callers are the car-meets-car handler (the original's routine: a car at speed above
 * 10 in a collision, one time in two), the blocked car's AI (the original's routine)
 * and the pedestrian code (the original's routine). The rules, from the machine code:
 *   - the dice are thrown first, every call (the original's table advances even
 *     with the speech off), and one throw picks both the line, r % 21, and
 *     the silence after it, r % 6 + 4 calls;
 *   - nothing while a line is being said;
 *   - a countdown of CALLS (the original's table): a call only counts it down unless
 *     it was already below zero;
 *   - the same range and volume as cop_shout(). */
static int yell_cool;
static void street_yell(long x, long y, int layer, long px, long py)
{
    int r = voice_rand();
    long dx = x - px, dy = y - py, d;

    if (gta_audio_speaking())
        return;
    if (yell_cool-- >= 0)
        return;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    d = dx + dy;
    if (d >= 0x1ffffffL)
        return;
    {
        int vol = (int)((0x1ffffffL - d) >> 19);
        if (covered(x, y, layer)) vol >>= 1;
        gta_audio_speak(&voice, 30 + r % 21, vol, 0);
    }
    yell_cool = r % 6 + 4;
}

/* THE PANIC'S SCREAMS - the original's routine, the original's "frighten the peds
 * around a point", for each ped it frightens:
 *   - an EFFECT, 0x17 / 0x16 / 0x15 in turn (the original's table steps once per
 *     ped and wraps at 3), from the ped;
 *   - a VOICE through the original's routine when the one who frightened them is not
 *     in mode 7: lines 62 + r % 8, a countdown of 3 calls (the original's table),
 *     and nothing while a line is being said. (Mode 7 goes to the original's routine,
 *     52..60, countdown 2 - the port's panic comes only from the player
 *     shooting on foot, so that branch has no caller here yet.)
 * ONE DEVIATION: the original plays every ped's effect, and through its
 * mixer that is a chorus; here each would steal a Paula channel from the
 * last - and from the gunshot that caused it - so only the first ped's
 * effect is played. Every ped still steps the turn. */
static int scream_turn, scream_cool;
static void panic_screams(long px, long py)
{
    int k;
    for (k = 0; k < peds.panic_n; k++) {
        const gta_ped *p = &peds.p[peds.panic_idx[k]];
        long dx = p->x - px, dy = p->y - py, d;
        int vol, r = voice_rand();
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        d = dx + dy;
        vol = d < 0x1ffffffL ? (int)((0x1ffffffL - d) >> 19) : 0;
        if (vol > 0 && covered(p->x, p->y, p->layer)) vol >>= 1;
        if (k == 0 && vol > 0)
            gta_audio_play(&sfx, 0x17 - scream_turn, vol, 0);
        scream_turn = scream_turn >= 2 ? 0 : scream_turn + 1;
        if (!gta_audio_speaking() && scream_cool-- < 0 && vol > 0) {
            gta_audio_speak(&voice, 62 + r % 8, vol, 0);
            scream_cool = 3;
        }
    }
}

/* THE BANK'S BELLS - see gta_script_world.alarm. Five of them, as the
 * original has (the original's routine); each rings sample 0x46 (70) - read out of
 * the sound loop that plays them (the source list at 0x143f44, where EBX
 * is loaded with 0x46 before the original's routine at 0x151d4) - loudest at the
 * bell and silent past 32 blocks, re-struck as each strike ends. */
#define ALARM_SLOTS  5
#define ALARM_SAMPLE 0x46
static struct { int line, on; long x, y; int wait; } alarms[ALARM_SLOTS];

static void script_alarm(void *ctx, int line, int on, int bx, int by)
{
    int k, free_ = -1;
    (void)ctx;
    for (k = 0; k < ALARM_SLOTS; k++) {
        if (alarms[k].on && alarms[k].line == line) {
            if (!on) {
                alarms[k].on = 0;
                printf("gta: script - the alarm at (%d,%d) stops\n", bx, by);
                fflush(stdout);
            }
            return;
        }
        if (!alarms[k].on && free_ < 0) free_ = k;
    }
    if (!on || free_ < 0)
        return;
    alarms[free_].on = 1;
    alarms[free_].line = line;
    alarms[free_].x = ((long)bx * 32 + 16) << 16;
    alarms[free_].y = ((long)by * 32 + 16) << 16;
    alarms[free_].wait = 0;
    printf("gta: script - an alarm rings at (%d,%d), slot %d\n", bx, by, free_);
    fflush(stdout);
}

/* One tick of the bells: the nearest ringing one, if it is within reach. */
static void alarms_tick(long px, long py)
{
    int k, best = -1;
    long bd = 0;
    for (k = 0; k < ALARM_SLOTS; k++) {
        long dx, dy, d;
        if (!alarms[k].on) continue;
        if (alarms[k].wait > 0) { alarms[k].wait--; continue; }
        dx = (alarms[k].x - px) >> 16; dy = (alarms[k].y - py) >> 16;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        d = dx + dy;
        if (d < 1024 && (best < 0 || d < bd)) { best = k; bd = d; }
    }
    if (best >= 0) {
        const gta_sfx_entry *e = (ALARM_SAMPLE < sfx.count)
                               ? &sfx.entry[ALARM_SAMPLE] : 0;
        int vol = (int)(64 - bd / 16);
        long ticks = 25;
        if (vol < 4) vol = 4;
        if (e && e->rate)
            ticks = (long)e->length * 50 / e->rate + 1;
        gta_audio_play(&sfx, ALARM_SAMPLE, vol, 0);
        alarms[best].wait = (int)ticks;
    }
}

/* BANK_ROBBERY - heat +1000 up to the cap, and level 4 whatever the heat
 * says (the original writes player+0x14a = 4 directly; force_level raises
 * the heat to that level's threshold so the recompute agrees). */
static void script_robbery(void *ctx)
{
    (void)ctx;
    score.heat += 1000;
    if (score.heat > GTA_HEAT_CAP) score.heat = GTA_HEAT_CAP;
    gta_score_force_level(&score, 4);
    printf("gta: script - BANK_ROBBERY: heat %d, wanted level %d\n",
           score.heat, score.level);
    fflush(stdout);
}

/* BOMBS IN CARS - see gta_script_world.setbomb. The original keeps the type
 * in car+0x9a and a fuse in car+0x9e, and its vehicle loop (the original's routine)
 * acts on them only for a car that is ACTIVE - driven:
 *
 *   type 2  the fuse runs down from 125 and then it goes off
 *   type 5  armed by speed: past 3/4 of the car's top speed it clicks and
 *           becomes type 6
 *   type 6  the SPEED bomb: below half the top speed, it goes off
 *   type 4  a tanker's bomb: the wreck blows five times, not once (its
 *           trigger is a global this port has not identified - not ported)
 *
 * "Top speed" is car+0x28, read here as the car table's max_speed, compared
 * against the car's speed in source units - half a world pixel a tick. The
 * only driven car a bomb can be in here is the player's, so that is the one
 * checked. */
#define BOMB_SLOTS 4
static struct { unsigned long h; int type, fuse; } bombs[BOMB_SLOTS];
static int freeze_enter, freeze_ticks;

static void script_setbomb(void *ctx, unsigned long h, int type)
{
    int k, free_ = -1;
    (void)ctx;
    for (k = 0; k < BOMB_SLOTS; k++) {
        if (bombs[k].h == h) { free_ = k; break; }
        if (!bombs[k].h && free_ < 0) free_ = k;
    }
    if (free_ < 0) return;
    bombs[free_].h = type ? h : 0UL;
    bombs[free_].type = type;
    bombs[free_].fuse = 0x7d;
    /* the original's routine: arming zeroes the car's damage (car+0xf7), so a type 4
     * starts from a clean slate. The fleet's cars are reachable from here;
     * the player's own is not (it lives in the main loop) and keeps his. */
    if (type) {
        int j;
        for (j = 0; j < traffic.n; j++)
            if (traffic.cars[j].serial == h)
                traffic.cars[j].damage = 0;
    }
    printf("gta: script - SETBOMB car %lu type %d%s\n", h, type,
           type ? "" : " (disarmed)");
    fflush(stdout);
}

static void script_freeze(void *ctx, int on, int ticks)
{
    (void)ctx;
    if (ticks > 0) freeze_ticks = ticks;
    else freeze_enter = on;
    printf("gta: script - %s\n", ticks > 0 ? "FREEZE_TIMED" :
           on ? "FREEZE_ENTER: he cannot leave the car" : "UNFREEZE_ENTER");
    fflush(stdout);
}

static void script_named_text(void *ctx, const char *name)
{
    const char *s = gta_text_get_name(&texts, name);
    (void)ctx;
    if (s) big_show(s);
    printf("gta: script - text [%s] %s\n", name, s ? s : "- not in the texts");
    fflush(stdout);
}

/* TYPE 4's TRIGGER: the car's damage (car+0xf7) above the original's table, which
 * the original's routine sets to 10 at level start - so the tanker job's bomb goes
 * off at the first real knock. SETBOMB zeroes the damage when it arms. */
#define BOMB_DAMAGE_TRIGGER 10

/* One tick of the bombs: the player's car, and any car of the fleet that
 * carries one (the tanker of 1020 SETBOMB 230 has a driver). Speeds are
 * compared as ratios of the car's own top, so the fleet's 16.16 units and
 * the player's source units both work. */
static void bombs_tick(gta_veh *vp, int in_a_car)
{
    int k;
    for (k = 0; k < BOMB_SLOTS; k++) {
        long sp, top, bx_, by_;
        int layer_, dmg, is_player, go;
        gta_car *fc = 0;
        if (!bombs[k].h)
            continue;
        is_player = (in_a_car && bombs[k].h == veh_serial);
        if (is_player) {
            long vx_ = vp->vx < 0 ? -vp->vx : vp->vx;
            long vy_ = vp->vy < 0 ? -vp->vy : vp->vy;
            sp = ((vx_ > vy_ ? vx_ : vy_) * 2) >> 16;   /* source px a tick */
            top = tiles.cars[vp->model].max_speed;
            dmg = vp->damage;
            bx_ = vp->ox; by_ = vp->oy; layer_ = player.layer;
        } else {
            int j;
            for (j = 0; j < traffic.n; j++)
                if (traffic.cars[j].serial == bombs[k].h && !traffic.cars[j].done) {
                    fc = &traffic.cars[j];
                    break;
                }
            if (!fc)
                continue;               /* not in the world just now */
            sp = fc->speed; top = fc->top;
            dmg = fc->damage;
            bx_ = fc->x; by_ = fc->y; layer_ = fc->layer;
        }
        if (bombs[k].type == 5 && sp > top * 3 / 4) {
            bombs[k].type = 6;
            printf("gta: bomb - click: armed at speed %ld of %ld\n", sp, top);
            fflush(stdout);
            continue;
        }
        go = (bombs[k].type == 6 && sp < top / 2) ||
             (bombs[k].type == 4 && dmg > BOMB_DAMAGE_TRIGGER) ||
             (bombs[k].type == 2 && --bombs[k].fuse < 0);
        /* a fleet car sitting at a light is not "driven" in the original's
         * sense for the speed bomb; only the damage and fuse kinds apply */
        if (go && !is_player && bombs[k].type == 6 && sp == 0)
            go = 0;
        if (!go)
            continue;
        printf("gta: bomb - type %d in car %lu goes off (speed %ld of %ld, "
               "damage %d)\n", bombs[k].type, bombs[k].h, sp, top, dmg);
        fflush(stdout);
        bombs[k].h = 0;
        gta_weapons_explode(&weapons, bx_, by_, layer_, &peds, &traffic,
                            &score, 0);
        if (is_player) {
            vp->damage = GTA_CAR_WRECKED;
            vp->fuse = 1;
        }
    }
}

static void script_car_kill(void *ctx, unsigned long h)
{
    (void)ctx;
    if (h == veh_serial && script_in_car) {
        printf("gta: script - KILL_CAR %lu: he is driving it, left alone\n", h);
    } else {
        printf("gta: script - KILL_CAR %lu: %s\n", h,
               gta_traffic_remove_car(&traffic, h) ? "gone" : "not in the fleet");
        if (h == arrow_car_h)
            arrow_car_h = 0;
    }
    fflush(stdout);
}

static void script_ped_speed(void *ctx, unsigned long h, int speed)
{
    (void)ctx;
    gta_peds_set_speed(&peds, h, speed);
}

/* THE DOCK CRANES - the world's half of CRANE and DO_GTA.
 *
 * WHAT IS THE ORIGINAL'S, and what is not yet: every rule below is read
 * out of the original's routine / the original's routine / the original's routine / the original's routine and the
 * state machine in the original's routine's tail. What is NOT here is the crane
 * itself on screen: the original swings an arm (map object 0x1e) over the
 * car, lifts it on the hook, swings it over the ship and lowers it into the
 * hold - six to a ship. Here the car is simply taken when the lift time is
 * up. The money, the texts, the six-car hold and the demands are all as
 * the original's.
 *
 *   offered: not in a car, or not on the trigger's block    -> not there
 *            a police car (model 4 or 0x20)                  "crane_nopolice"
 *            longer than 64 (car+0x36 < -0x40)               "crane_long"
 *            a bomb in it (car+0x9a)                         "crane_nobomb"
 *            the crane busy / the hold full (6)              "crane2" / "crane1"
 *            a car of a job (car+299 >= 0)                   "crane4"
 *            else TAKEN: "crane0" - or "crane5" when a demand is hung on the
 *            crane and this is not what it asked for
 *   taken:   he gets out; the car must not be moved further than a block
 *            before the hook has it ("crane_screwed"), then the lift
 *   lifted:  damage >= 100 "crane_wreck", no money; else pay
 *            max(1000, value[crane] * 1000 * (100 - damage) / 100 / (dup + 1))
 *            where dup is how many of this model are already in the hold
 *            ("duplicate_model(s)"), then "crane_excellent" for no damage or
 *            "cranecar<damage/25>"; a demanded model counts towards it
 *   full:    the ship leaves once the player is out of sight of it */
#define CRANE_MAX       4
#define CRANE_HOLD      6
/* THE GANTRY'S PACE: the original's routine moves the crane object one of the
 * original's units a frame along its axis - half a port pixel, and a game
 * frame is a tick and a half here - so a third of a pixel a tick. The reach
 * is 128 units (two blocks) out over the bay and back; the drop into the
 * hold is its own, and is only a pause here. */
#define CRANE_STEP      (65536L / 3)
#define CRANE_REACH_PX  64
#define CRANE_SHIFT_PX  32
#define CRANE_LOWER     45      /* ticks the car hangs going down into the hold */
enum { CR_IDLE, CR_REACH, CR_WAIT_OUT, CR_CARRY, CR_LOWER, CR_SHIFT, CR_HOLD,
       CR_RETURN, CR_DONE, CR_FAIL };
static struct {
    long x, y;                  /* 16.16 world, where the gantry rests */
    long arm_y;                 /* 16.16, where the gantry is now - it runs along y */
    int  dir;                   /* the declaration's `a`: -1 or +1, which way the bay is */
    int  angle;                 /* the sprite's heading, port 256ths */
    int  state;                 /* CR_* */
    unsigned long car;
    long car_x, car_y;
    int  timer;
    int  miss;                  /* ticks the car could not be found (he is getting out) */
    /* THE CAR ON THE HOOK - out of the fleet from the lift to the hold */
    int  hang_model, hang_remap, hang_angle, hang_damage;
    int  count;
    int  models[CRANE_HOLD];
    int  dem_model, dem_remap, dem_need, dem_have;
} cranes[CRANE_MAX];
static int n_cranes;

static void cranes_init(void)
{
    int i;
    n_cranes = 0;
    for (i = 0; i < script.n && n_cranes < CRANE_MAX; i++) {
        if (script.d[i].type != GTA_DECL_CRANE)
            continue;
        memset(&cranes[n_cranes], 0, sizeof cranes[n_cranes]);
        /* the ORIGINAL's pixels, 64 to a block: halved for the port's 32 */
        cranes[n_cranes].x = ((long)script.d[i].x / 2) << 16;
        cranes[n_cranes].y = ((long)script.d[i].y / 2) << 16;
        cranes[n_cranes].arm_y = cranes[n_cranes].y;
        cranes[n_cranes].dir = script.d[i].a < 0 ? -1 : 1;
        /* `b` is the original's 0..1023 heading */
        cranes[n_cranes].angle = ((int)script.d[i].b / 4) & 255;
        cranes[n_cranes].dem_model = -1;
        n_cranes++;
    }
    printf("gta: cranes - %d on the docks\n", n_cranes);
}

/* THE CRANE TALKS IN THE BRIEF BOX with the mouth icon - the original's
 * the original's routine(1, text), kind 1 = SPEECH - not on the big card, where the
 * first test put a sentence across half the screen. */
static void crane_say(const char *name)
{
    const char *s = gta_text_get_name(&texts, name);
    if (s) brief_show(GTA_BRIEF_SPEECH, s);
    printf("gta: crane - [%s] %s\n", name, s ? s : "- not in the texts");
    fflush(stdout);
}

/* The four price lines carry "$%d" - the original formats them with the
 * money (the original's routine with an argument). Only a string whose one % is a
 * %d is formatted; anything else is shown as it stands. */
static void crane_say_pay(const char *name, long pay)
{
    const char *s = gta_text_get_name(&texts, name);
    const char *p;
    int pct = 0;
    char buf[128];
    if (!s) { crane_say(name); return; }
    for (p = s; *p; p++)
        if (*p == '%') pct++;
    p = strchr(s, '%');
    if (pct == 1 && p && p[1] == 'd') {
        snprintf(buf, sizeof buf, s, (int)pay);
        brief_show(GTA_BRIEF_SPEECH, buf);
        printf("gta: crane - [%s] %s\n", name, buf);
        fflush(stdout);
    } else {
        crane_say(name);
    }
}

static int script_crane_offer(void *ctx, int k, int bx, int by)
{
    const gta_car_info *ci;
    int j, model;
    (void)ctx;
    if (k < 0 || k >= n_cranes || !script_in_car)
        return -1;
    if ((int)(script_veh_x >> 21) != bx || (int)(script_veh_y >> 21) != by)
        return -1;
    ci = &tiles.cars[script_veh_model];
    model = ci->model_id;
    if (model == 4 || model == 0x20) { crane_say("crane_nopolice"); return 10; }
    if (ci->length > 0x40)           { crane_say("crane_long");     return 10; }
    for (j = 0; j < BOMB_SLOTS; j++)
        if (bombs[j].h && bombs[j].h == veh_serial) {
            crane_say("crane_nobomb");
            return 10;
        }
    if (cranes[k].state != CR_IDLE) { crane_say("crane2"); return 2; }
    if (cranes[k].count >= CRANE_HOLD) { crane_say("crane1"); return 1; }
    /* car+0xf9 > 0: the text says what it is - "The flames look real
     * pretty" - a car on fire; here, a written-off one burning its fuse */
    if (script_veh_damage >= GTA_CAR_WRECKED) { crane_say("crane3"); return 3; }
    if (veh_mission) { crane_say("crane4"); return 4; }
    cranes[k].state = CR_REACH;
    cranes[k].car = veh_serial;
    cranes[k].car_x = script_veh_x;
    cranes[k].car_y = script_veh_y;
    cranes[k].miss = 0;
    crane_say(cranes[k].dem_need > 0 && cranes[k].dem_model >= 0 &&
              cranes[k].dem_model != model ? "crane5" : "crane0");
    printf("gta: crane %d - takes car %lu (model %d)\n", k, veh_serial, model);
    fflush(stdout);
    return 0;
}

static int script_crane_poll(void *ctx, int k)
{
    (void)ctx;
    if (k < 0 || k >= n_cranes)
        return 2;
    if (cranes[k].state == CR_DONE) {
        cranes[k].state = CR_RETURN;
        return 1;
    }
    if (cranes[k].state == CR_FAIL) {
        cranes[k].state = CR_RETURN;
        return 2;
    }
    return 0;
}

static int script_crane_demand(void *ctx, int k, int model, int remap, int need)
{
    (void)ctx;
    if (k < 0 || k >= n_cranes)
        return 0;
    if (need > 0) {
        cranes[k].dem_model = model;
        cranes[k].dem_remap = remap;
        cranes[k].dem_need = need;
        cranes[k].dem_have = 0;
        printf("gta: crane %d - wants %d of model %d\n", k, need, model);
        fflush(stdout);
        return 0;
    }
    if (cranes[k].dem_need > 0 && cranes[k].dem_have >= cranes[k].dem_need) {
        cranes[k].dem_need = 0;
        cranes[k].dem_model = -1;
        return 1;
    }
    return 0;
}

/* Move a gantry towards `target` (16.16) at its pace; 1 once it is there. */
static int crane_move(int k, long target)
{
    long d = target - cranes[k].arm_y;
    if (d > CRANE_STEP)        cranes[k].arm_y += CRANE_STEP;
    else if (d < -CRANE_STEP)  cranes[k].arm_y -= CRANE_STEP;
    else { cranes[k].arm_y = target; return 1; }
    return 0;
}

/* THE CAR GOES INTO THE HOLD: the money, the texts, the demand. */
static void crane_hold(int k)
{
    const gta_car_info *ci = &tiles.cars[cranes[k].hang_model];
    int dmg = cranes[k].hang_damage > 100 ? 100 : cranes[k].hang_damage;
    int model = ci->model_id, dup = 0, j;
    long pay;
    for (j = 0; j < cranes[k].count; j++)
        if (cranes[k].models[j] == model) dup++;
    cranes[k].models[cranes[k].count++] = model;
    if (dmg >= 100) {
        crane_say("crane_wreck");
        return;
    }
    if (dup > 0)
        crane_say(dup == 1 ? "duplicate_model" : "duplicate_models");
    pay = (long)ci->value[k] * 1000L * (100 - dmg) / 100 / (dup + 1);
    if (pay < 1000) pay = 1000;
    printf("gta: crane %d - model %d, damage %d, value %d, %d already "
           "aboard: pays %ld\n", k, model, dmg, ci->value[k], dup, pay);
    fflush(stdout);
    script_score(0, pay);
    if (cranes[k].dem_need > 0 && cranes[k].dem_model == model &&
        (cranes[k].dem_remap < 0 || cranes[k].dem_remap == cranes[k].hang_remap))
        cranes[k].dem_have++;
    /* "crane-excellent" with a HYPHEN - the tools's symbol for the string
     * (s_crane_excellent_...) had turned it into '_'; the key in
     * english.fxt is read by out/fxt_keys.py */
    if (dmg == 0) crane_say_pay("crane-excellent", pay);
    else {
        static const char *const cc[4] = { "cranecar0", "cranecar1",
                                           "cranecar2", "cranecar3" };
        crane_say_pay(cc[(dmg / 25) & 3], pay);
    }
}

/* One tick of the cranes - the original's routine's tail, state for state:
 *   REACH    out over the bay (2 blocks)            original state 1
 *   WAIT_OUT the hook waits for the driver to go    state 2 (car+2 == -1)
 *   CARRY    back over the ship with the car        state 3
 *   LOWER    down into the hold                     state 4
 *   SHIFT    along a block                          state 5
 *   HOLD     let go: it is aboard, and paid         state 6
 *   RETURN   home again                             state 7
 * A car moved more than a block off the bay before the hook has it, or lost,
 * is "crane_screwed" (state 10). */
static void cranes_tick(long px, long py)
{
    int k;
    for (k = 0; k < n_cranes; k++) {
        long over = cranes[k].y - (long)cranes[k].dir * (CRANE_REACH_PX << 16);
        if (cranes[k].state == CR_IDLE && cranes[k].count >= CRANE_HOLD) {
            /* FULL: the ship sails once the player is out of sight of it */
            long dx = (cranes[k].x - px) >> 21, dy = (cranes[k].y - py) >> 21;
            if (dx > 20 || dx < -20 || dy > 20 || dy < -20) {
                cranes[k].count = 0;
                printf("gta: crane %d - the ship has sailed\n", k);
                fflush(stdout);
            }
            continue;
        }
        if (cranes[k].state == CR_REACH || cranes[k].state == CR_WAIT_OUT) {
            long cx_, cy_;
            int wr_ = 0;
            if (script_in_car && veh_serial == cranes[k].car) {
                cx_ = script_veh_x; cy_ = script_veh_y;
            } else if (!gta_traffic_find_car(&traffic, cranes[k].car,
                                             &cx_, &cy_, 0, &wr_)) {
                /* NOT IN THE FLEET FOR A MOMENT while he climbs out - the
                 * car goes back into it at the end of the get-out, under its
                 * old serial (leave_car). Only a car missing for a second is
                 * lost. */
                if (++cranes[k].miss < 50)
                    continue;
                cranes[k].state = CR_FAIL;
                crane_say("crane_screwed");
                continue;
            }
            cranes[k].miss = 0;
            {
                long dx = (cx_ - cranes[k].car_x) >> 16, dy = (cy_ - cranes[k].car_y) >> 16;
                if (dx > 32 || dx < -32 || dy > 32 || dy < -32) {
                    cranes[k].state = CR_FAIL;
                    crane_say("crane_screwed");
                    continue;
                }
            }
            if (cranes[k].state == CR_REACH) {
                if (crane_move(k, over))
                    cranes[k].state = CR_WAIT_OUT;
                continue;
            }
            /* WAIT_OUT: the hook takes it once nobody is in it */
            if (script_in_car && veh_serial == cranes[k].car)
                continue;
            {
                int j, idx = -1;
                for (j = 0; j < traffic.n; j++)
                    if (traffic.cars[j].serial == cranes[k].car) { idx = j; break; }
                if (idx < 0) continue;
                cranes[k].hang_model = traffic.cars[idx].model;
                cranes[k].hang_remap = traffic.cars[idx].remap;
                cranes[k].hang_angle = gta_car_draw_angle(&traffic.cars[idx]);
                cranes[k].hang_damage = traffic.cars[idx].damage;
                gta_traffic_remove_car(&traffic, cranes[k].car);
                cranes[k].car = 0;
                cranes[k].state = CR_CARRY;
                printf("gta: crane %d - the hook has it\n", k);
                fflush(stdout);
            }
            continue;
        }
        switch (cranes[k].state) {
        case CR_CARRY:
            if (crane_move(k, cranes[k].y)) {
                cranes[k].state = CR_LOWER;
                cranes[k].timer = CRANE_LOWER;
            }
            break;
        case CR_LOWER:
            if (--cranes[k].timer <= 0)
                cranes[k].state = CR_SHIFT;
            break;
        case CR_SHIFT:
            if (crane_move(k, cranes[k].y -
                              (long)cranes[k].dir * (CRANE_SHIFT_PX << 16)))
                cranes[k].state = CR_HOLD;
            break;
        case CR_HOLD:
            crane_hold(k);
            cranes[k].state = CR_DONE;      /* the CRANE command pays and moves on */
            break;
        case CR_RETURN:
            if (crane_move(k, cranes[k].y))
                cranes[k].state = CR_IDLE;
            break;
        default:
            break;
        }
    }
}

/* THE GANTRIES ON SCREEN: map object 0x1e where each one is now, at the
 * height it stands (the declaration's z, 192 of the original's = layer 3),
 * and the car on the hook under it while it carries one. */
static void cranes_draw(gta_view *v)
{
    int k, spr = gta_tiles_object_sprite(&tiles, 0x1e);
    for (k = 0; k < n_cranes; k++) {
        long dx = cranes[k].x - v->cam_x, dy = cranes[k].arm_y - v->cam_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > (12L << 21) || dy > (12L << 21))
            continue;
        if (cranes[k].state >= CR_CARRY && cranes[k].state <= CR_SHIFT) {
            const gta_car_info *ci = &tiles.cars[cranes[k].hang_model];
            gta_render_add_sprite_dm(v, cranes[k].x, cranes[k].arm_y, 3, 3,
                                     ci->sprite_index, cranes[k].hang_angle,
                                     cranes[k].hang_remap >= 0 &&
                                     cranes[k].hang_remap < GTA_CAR_REMAPS
                                         ? (int)ci->remap8[cranes[k].hang_remap] : 0,
                                     -1, 0UL);
        }
        if (spr >= 0)
            gta_render_add_sprite(v, cranes[k].x, cranes[k].arm_y, 3, 3,
                                  spr, cranes[k].angle);
    }
}

/* THE LEVEL IS OVER - see gta_script_world.level_end. The original shows
 * the result and goes to the next level; this port has only Liberty City's
 * first, so the card is shown and the game plays on. */
static void script_level_end(void *ctx, int code)
{
    const char *key = code == 1 ? "m22success" : code == 2 ? "m22failed"
                    : code == 3 ? "m22dead" : "m22incomplete";
    const char *s = gta_text_get_name(&texts, key);
    (void)ctx;
    if (s) big_show(s);
    printf("gta: LEVEL OVER - code %d, [%s] %s\n", code, key, s ? s : "");
    fflush(stdout);
}

static const gta_script_world script_world = {
    script_powerup_on, script_powerup_off, script_powerup_done, script_arrow,
    script_car_on, script_car_pos, script_car_dead,
    script_player_car, script_player_car_model, script_arrow_car,
    script_score, script_player_car_at, script_mission_done,
    script_car_release,
    script_ped_on, script_ped_pos, script_ped_dead, script_arrow_ped,
    script_ped_into_car, script_ped_kill,
    script_player_arrested, script_player_dead, script_respray,
    script_onscreen, script_drop_wanted, script_ped_remap, script_ped_type,
    script_ped_sendto, script_ped_at, script_car_on_driven,
    script_score_now, script_explode, script_frenzy_stop,
    script_alarm, script_robbery,
    script_setbomb, script_freeze, script_named_text,
    script_car_kill, script_ped_speed, script_red_arrow,
    script_crane_offer, script_crane_poll, script_crane_demand,
    script_level_end, script_say, script_kf_timer
};

/* PUT THE CAR BACK AND GIVE IT ITS NAME AGAIN.
 *
 * abandon() makes a fresh car with a fresh serial; a mission that said
 * "steal car 189" must still recognise it after the player has driven it
 * across the city and got out, so the old serial is put back. Returns what
 * abandon() returned, so `if (!leave_car(...))` reads as before. */
static unsigned long leave_car(int model, long x, long y, int face, int layer,
                               int remap, int damage)
{
    unsigned long s_ = gta_traffic_abandon(&traffic, model, x, y, face, layer,
                                           remap, damage);
    if (s_ && veh_serial) {
        gta_traffic_rename_car(&traffic, s_, veh_serial);
        s_ = veh_serial;
    }
    if (s_ && veh_mission)
        gta_traffic_set_mission(&traffic, s_, 1);
    veh_serial = 0;
    veh_mission = 0;
    return s_;
}

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

/* A CAR IS SOLID TO A MAN ON FOOT (218). The walk used to test only the
 * map, so he walked straight through parked cars and under moving ones as
 * if they were painted on the road - "przechodzi pod autami bez szkody".
 * After the step: if it put him inside a car body (plus his own 3 px) and
 * he was not inside one before, he keeps whichever axis of the move is free
 * - so he slides along the side of a car he walks into at an angle - or
 * neither. Already inside one (a car stopped on top of him) he may walk out.
 * The way across a car is SPACE while running: the vault or the slide. */
#define PED_CAR_MARGIN 3
static void ped_car_block(gta_player *p, const gta_traffic *tr,
                          const gta_tiles *t, long ox, long oy)
{
    long nx = p->x, ny = p->y;
    if (fleet_car_at(tr, t, nx, ny, p->layer, PED_CAR_MARGIN, 0) < 0)
        return;
    if (fleet_car_at(tr, t, ox, oy, p->layer, PED_CAR_MARGIN, 0) >= 0)
        return;
    if (fleet_car_at(tr, t, nx, oy, p->layer, PED_CAR_MARGIN, 0) < 0) {
        p->y = oy;
    } else if (fleet_car_at(tr, t, ox, ny, p->layer, PED_CAR_MARGIN, 0) < 0) {
        p->x = ox;
    } else {
        p->x = ox;
        p->y = oy;
    }
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
    long ram_cop_seen = 0;      /* police cars rammed, already accounted for */
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
    /* WHICH SOUND EACH WEAPON MAKES, indexed like `ammo` above: 0 is the
     * fist and has none. */
    static const int weapon_snd[GTA_WEAPON_COUNT] = {
        -1, GTA_SND_PISTOL, GTA_SND_MACHINEGUN, GTA_SND_ROCKET, GTA_SND_FLAME
    };
    int  ammo_sub[GTA_WEAPON_COUNT] = { 0, 0, GTA_AMMO_PER_UNIT, 0,
                                        GTA_AMMO_PER_UNIT };
    /* INFINITE AMMUNITION, the original's `+0x192` timer and the weapon it
     * saved. A crate of 100 or more does not give you rounds - it makes the
     * weapon endless for (amount - 100) ticks and hands back what you were
     * carrying afterwards. `inf_w` is 0 when none is running. */
    int  inf_w = 0, inf_ticks = 0, inf_save_w = 0;
    int  inf_save_ammo[GTA_WEAPON_COUNT];
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
        printf("sfx: no " SFX_PATH " - running silent\n");
    if (gta_sfx_open_index(VOICE_PATH, &voice) == 0)
        printf("voice: %d lines, read off the disk as they are said\n",
               voice.count);
    else
        printf("voice: no " VOICE_PATH " - the game says nothing\n");
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
        g_prefs = prefs;
        g_prefs_loaded = 1;
        opt_audio = prefs.audio;
#ifdef __MORPHOS__
        /* THE GRAPHICS SETTING HAS ONE LEGAL VALUE HERE, so it is read and
         * then overruled rather than obeyed.
         *
         * AGA is a chipset this machine does not have, and the Workbench-window
         * backend negotiates a shared palette through ObtainBestPen - both live
         * in amiga_gfx.c, which the MorphOS build does not compile.
         * native/morphos_gfx.c is the RTG path and nothing else.
         *
         * The setting is still worth having in the file: gtaprefs reports what
         * the machine has, and a drawer may be shared with an Amiga install.
         * Obeying a value that cannot be honoured would end in morphos_gfx.c
         * logging "backend 0 requested" and opening RTG anyway - same outcome,
         * reached confusingly.
         *
         * prefs.gfx is left ALONE, not rewritten, so gta_prefs_screen_size()
         * below still sees what the player chose. */
        if (prefs.gfx != GTA_GFX_RTG)
            printf("gta: prefs ask for gfx %s - ignored, MorphOS has only the"
                   " RTG path\n", gta_prefs_gfx_name(prefs.gfx));
        backend = AMIGAGFX_BACKEND_RTG;
#else
        if (prefs.gfx == GTA_GFX_AGA)      backend = AMIGAGFX_BACKEND_AGA;
        else if (prefs.gfx == GTA_GFX_RTG) backend = AMIGAGFX_BACKEND_RTG;
        else if (prefs.gfx == GTA_GFX_WB)  backend = AMIGAGFX_BACKEND_WB;
#endif
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
                if (strcmp(word, "rampdbg") == 0)      opt_rampdbg = (int)val;
                else if (strcmp(word, "engine") == 0)  opt_engine  = (int)val;
                else if (strcmp(word, "overlay") == 0) opt_overlay = (int)val;
                else if (strcmp(word, "traffic") == 0) opt_traffic = (int)val;
                else if (strcmp(word, "fleet") == 0)   opt_fleet   = (int)val;
                else if (strcmp(word, "lights") == 0)  opt_lights  = (int)val;
                else if (strcmp(word, "halfrate") == 0) opt_halfrate = (int)val;
                else if (strcmp(word, "cruise") == 0) opt_cruise = (int)val;
                else if (strcmp(word, "driveprof") == 0) opt_driveprof = (int)val;
                else if (strcmp(word, "catchup") == 0) opt_catchup = (int)val;
                else if (strcmp(word, "benchframes") == 0) opt_benchf = (int)val;
                else if (strcmp(word, "width") == 0)   opt_width   = (int)val;
                else if (strcmp(word, "camh") == 0)    opt_camh    = (int)val;
                else if (strcmp(word, "screen") == 0)  opt_screen  = (int)val;
                else if (strcmp(word, "screen2x") == 0) opt_screen2x = (int)val;
                else if (strcmp(word, "selftest") == 0) opt_selftest = (int)val;
                else if (strcmp(word, "bench") == 0) opt_bench = (int)val;
                /* THE RIG'S SOUND SWITCH. `audio 0` in opts.txt silences the
                 * port whatever gta.prefs says, so the SAME binary can be run
                 * both ways and the difference is the audio's own cost. It is
                 * an override like `screen` and for the same reason: a
                 * measurement must not be at the mercy of a settings file. */
                else if (strcmp(word, "audio") == 0)
                    opt_audio_opt = (int)val;
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

    /* SOUND, and it has to be AFTER opts.txt, not after gta.prefs.
     *
     * The first version opened it beside the prefs, forty lines before
     * opts.txt is even read - so the rig's own `audio 0` switch, whose whole
     * purpose is to run the SAME binary silent and measure the difference,
     * arrived too late to be obeyed and the A/B measured nothing at all.
     * Both settings have to be in before the device is touched. */
    if (opt_audio_opt == 0) {
        printf("gta: opts audio 0 - silent, whatever gta.prefs says\n");
        opt_audio = GTA_AUDIO_OFF;
    }
    gta_audio_open(&sfx, opt_audio);
    /* THE RADIO IS SCANNED AFTER THE EXTRACTION, not here: the first run
     * converts the player's soundtrack and a scan taken before it would find
     * no stations on exactly the run that just made them. See front_extract().
     */

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
    /* THE ARROW SPRITE is the first of the style file's "arrow" category -
     * 12x10, yellow, and the same category holds the whole HUD icon set. */
    arrow_sprite = gta_tiles_sprite_base(&tiles, GTA_SPR_ARROW);
    hud_icon = arrow_sprite;
    printf("gta: HUD icons from sprite %d (%dx%d cop head, %dx%d pager)\n",
           hud_icon,
           gta_hud_sprite_w(&tiles, hud_icon + HUD_ICON_COP),
           gta_hud_sprite_h(&tiles, hud_icon + HUD_ICON_COP),
           gta_hud_sprite_w(&tiles, hud_icon + HUD_ICON_PAGER),
           gta_hud_sprite_h(&tiles, hud_icon + HUD_ICON_PAGER));
    fflush(stdout);
    have_pager      = gta_font_load(&pager_font, FONT_PAGER, tiles.palette) == 0;
    have_score_font = gta_font_load(&score_font, FONT_SCORE, tiles.palette) == 0;
    have_big        = gta_font_load(&big_font,   FONT_BIG,   tiles.palette) == 0;
    have_mult       = gta_font_load(&mult_font,  FONT_MULT,  tiles.palette) == 0;
    /* THE FRONT END. Its font is remapped into ITS OWN palette, not the
     * game's - the title screen is up before the city is, and the two
     * palettes have nothing to do with each other. */
    if (gta_front_load(&front, FRONT_PATH) == 0)
        have_menu_font = gta_font_load(&menu_font, FONT_MENU, front.pal) == 0;
    gta_text_load(&texts, FXT_PATH);
    printf("gta: fonts - pager %s (%d px), score %s, big %s, multiplier %s; "
           "%d texts\n",
           have_pager ? "yes" : "no", pager_font.height,
           have_score_font ? "yes" : "no", have_big ? "yes" : "no",
           have_mult ? "yes" : "no", texts.n);
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

    /* THE FIRST LOAD'S MUSIC EXTRACTION, before anything asks what stations
     * there are. It draws a progress bar, so it has to be after the screen
     * is open and chunky is bound, and it does nothing at all on every run
     * after the first. */
    front_extract(chunky, pitch);
    gta_audio_radio_scan(GTA_DIR "GTADATA/");

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
    /* the player's car limit (Amiga options); `fleet N` in opts.txt still
     * overrides it for the test rig */
    traffic.fleet_cap = opt_fleet >= 0 ? opt_fleet : g_prefs.cars;
    /* The tick times its own phases on the E-clock - see prof_us in
     * gta_traffic.h. Host tools leave the pointer NULL and pay nothing. */
    traffic.prof_clock = amiga_uclock_us;
    traffic.prof_raw = opt_driveprof ? amiga_uclock_raw : 0;
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
        if (opt_halfrate >= 0) traffic.opt_halfrate = opt_halfrate;
        if (opt_cruise >= 0) traffic.opt_cruise = opt_cruise;
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
        if (gta_script_load(&script, INI_PATH, 1) == 0) {
            static const int show[5] = { GTA_DECL_TELEPHONE, GTA_DECL_TRIGGER,
                                         GTA_DECL_PARKED, GTA_DECL_DOOR,
                                         GTA_DECL_SPRAY };
            int k_, j_;
            printf("gta: script - %d declarations in section [1]", script.n);
            for (k_ = 0; k_ < 5; k_++) {
                int c_ = 0;
                for (j_ = 0; j_ < script.n; j_++)
                    if (script.d[j_].type == show[k_]) c_++;
                printf(", %d %s", c_, gta_script_type_name(show[k_]));
            }
            printf("%s\n", script.n_unknown ? " (some names unknown)" : "");
            fflush(stdout);
            gta_script_place(&script, &nav, &tiles);
            if (gta_script_load_cmds(&script, INI_PATH, 1) == 0) {
                gta_script_set_brief(&script, script_brief, 0);
                gta_script_set_world(&script, &script_world, 0);
                cranes_init();
                printf("gta: script - %d commands in the logic block, "
                       "%d distinct names\n", script.n_cmds, script.n_cnames);
                /* AND THE CRATES ARE THE SCRIPT'S NOW. The file reader above
                 * put all 151 POWERUPs out at load, which is not what the
                 * original does: a crate exists when a POWERUP_ON names it.
                 * Empty the table and let the level's opening block fill it -
                 * the fourteen powerups no path ever turns on then stay off,
                 * as they do in the real game. */
                gta_pickups_init(&pickups, &tiles);
                printf("gta: pickups - cleared; the script's POWERUP_ON makes "
                       "them now\n");
                fflush(stdout);
            }

            /* WHERE THE LEVEL STARTS HIM. Until now the player was put down
             * on the renderer's old downtown junction (64,64), which is a
             * fine view and the wrong place: Liberty City's script has no
             * entry point of its own - it runs because the PLAYER block is
             * also TRIGGER 303 - so a player anywhere else never starts the
             * level at all. */
            {
                int sbx_, sby_, sang_;
                if (gta_script_player_start(&script, &sbx_, &sby_, &sang_)) {
                    if (gta_player_init(&player, &map, &tiles, sbx_, sby_)) {
                        player.angle = sang_;
                        start_bx = sbx_;
                        start_by = sby_;
                        gta_render_look_at_block(&view, start_bx, start_by);
                        printf("gta: script - the level starts the player on "
                               "block (%d,%d) layer %d facing %d\n",
                               sbx_, sby_, player.layer, sang_);
                    } else {
                        printf("gta: script - the PLAYER block (%d,%d) has no "
                               "walkable layer; staying at (%d,%d)\n",
                               sbx_, sby_, start_bx, start_by);
                    }
                    fflush(stdout);
                }
            }
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
        int parked = gta_traffic_park(&traffic, &map, start_bx, start_by,
                                      8, traffic.fleet_cap);
        printf("gta: %d cars parked around (%d,%d)\n",
               parked, start_bx, start_by);
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

    /* THE TITLE SCREEN, and only when a person is at the keyboard. Every
     * automated run in this project is driven by a script file and none of
     * them can press a key, so a menu in front of them would hang the
     * harness - see front_menu(). */
    {
        FILE *sf = fopen(GTA_DIR "autodrive.txt", "r");
        if (!sf) sf = fopen(GTA_DIR "autowalk.txt", "r");
        if (!sf) sf = fopen(GTA_DIR "autoinput.txt", "r");
        if (sf) {
            fclose(sf);
            log_line("gta: front end skipped - a script is driving");
        } else if (front_menu(chunky, pitch)) {
            log_line("gta: front end - quit");
            gta_front_free(&front);
            amigagfx_close();
            /* AND THE AUDIO, which is open by now.
             *
             * AmigaOS does not reclaim an OpenDevice from a process that
             * exits without closing it: the four channels stay allocated to a
             * dead IORequest and the NEXT run of the game gets
             * "audio.device would not open" for the rest of the session. It
             * happened the first time this path was taken - the game was left
             * on the title screen, the harness reloaded it from there, and
             * every run afterwards was silent until the guest was rebooted.
             * Every exit closes the audio; there are two of them and this is
             * the easy one to forget. */
            gta_audio_close();
            return g_reload ? 5 : 0;
        }
    }

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
     * the number this prints has to mean "the city is moving".
     *
     * OURS, NOT THE PLAYER'S (218): it takes the better part of a minute on
     * a 68020 before the game starts, flashes the screen through every mode
     * and drives the traffic 200 ticks. `bench 1` in opts.txt runs it; the
     * test rig's opts.txt says so, a player's install has no such line. */
    if (!opt_bench) {
        log_line("gta: benchmark skipped (opts.txt `bench 1` runs it)");
    } else {
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
                /* ONLY THE ONES THAT FIT THE SCREEN THAT IS OPEN. The two
                 * big modes are a different display; entering one here would
                 * mean closing and reopening the screen in the middle of a
                 * measurement, and their frame times would not be comparable
                 * with the rest anyway. */
                if (view_modes[mi].sw
                    && (view_modes[mi].sw != g_screen_w
                        || view_modes[mi].sh != g_screen_h))
                    continue;
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
    }   /* opt_bench */

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
                } else if (sscanf(ln, "brief %d %d", &a, &b) == 2) {
                    /* A TEST FIXTURE: show text `a` on display `b` -
                     * GTA_BRIEF_*, so 0..5 are the brief box's six icons,
                     * 6 the pager, 7 a timed pager line and 8 the big
                     * centred card. */
                    adq[adq_n].op = 16; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = b; adq_n++;
                } else if (sscanf(ln, "brief %d", &a) == 1) {
                    adq[adq_n].op = 16; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = GTA_BRIEF_PAGER;
                    adq_n++;
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
                } else if (sscanf(ln, "goto %d %d", &a, &b) == 2) {
                    /* A TEST FIXTURE: put the player on block (a,b). The
                     * city is 256 blocks across and a job crosses most of
                     * it; walking there by script is not a test of the job.
                     * In a car it takes the car with him - a job ends at a
                     * garage and the test has to be able to arrive. */
                    adq[adq_n].op = 17; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = b; adq_n++;
                } else if (sscanf(ln, "sound %d %d", &a, &b) == 2) {
                    /* A TEST FIXTURE, and the only unattended proof there is:
                     * a sound cannot be looked at. Plays bank entry `a` at
                     * volume `b`, and the log then says how long the channel
                     * was really occupied against how long the sample lasts. */
                    adq[adq_n].op = 19; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = b; adq_n++;
                } else if (sscanf(ln, "sound %d", &a) == 1) {
                    adq[adq_n].op = 19; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq[adq_n].brk = 64; adq_n++;
                } else if (sscanf(ln, "mode %d", &a) == 1) {
                    /* A TEST FIXTURE: F4's window size, which cannot be
                     * pressed from a script. 0..5, smallest first. */
                    adq[adq_n].op = 18; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq_n++;
                } else if (sscanf(ln, "score %d", &a) == 1) {
                    /* A TEST FIXTURE: `a` points onto the score, as if
                     * earned - FRENZY_CHECK and SCORE_CHECK read nothing
                     * else, and an unattended run cannot shoot a crowd. */
                    adq[adq_n].op = 21; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq_n++;
                } else if (sscanf(ln, "script %d", &a) == 1) {
                    /* A TEST FIXTURE: start a script process at that line -
                     * see gta_script_debug_start(). */
                    adq[adq_n].op = 20; adq[adq_n].t = 1;
                    adq[adq_n].thr = a; adq_n++;
                } else if (strncmp(ln, "quitask", 7) == 0) {
                    /* THE QUIT CARD (218), as ESC raises it in play */
                    adq[adq_n].op = 22; adq[adq_n].t = 1; adq_n++;
                } else if (strncmp(ln, "quitno", 6) == 0) {
                    adq[adq_n].op = 23; adq[adq_n].t = 1; adq_n++;
                } else if (strncmp(ln, "quittest", 8) == 0) {
                    /* ...and Y: the same way out, but with the reload code so
                     * the run script starts the game again in this emulator */
                    adq[adq_n].op = 24; adq[adq_n].t = 1; adq_n++;
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
             "F4 window size  F5 2.5D / 2.5D-light / flat  F6 frame cap  F7/F8 camera");
    frames = 0;
    t0 = amiga_uclock_us();
    prof_t0 = t0;
    amiga_watchdog_start();
    pager_brief(1001);          /* the opening brief, now that frames count */

    while (running) {
        int dx = 0, dy = 0, speed;

        /* THE BUFFER CAN MOVE UNDER THE LOOP. F3 and F4 both close the screen
         * and open another, which frees the chunky buffer and allocates a new
         * one; the loop's own copies of the pointer and the pitch then belong
         * to memory the game no longer owns. It cost a filmstrip that showed
         * the previous screen's picture with seventy rows of heap under it,
         * and it would eventually have cost a write into somebody else's
         * memory - so they are re-read every frame rather than at each of the
         * places that can reopen. Two assignments against a frame's ~300 000
         * pixel writes. */
        chunky = g_chunky;
        pitch  = g_pitch;

        /* Reap finished channels. Nothing plays unless something asked for
         * it, so this costs four CheckIO calls a frame and is what makes a
         * channel idle again for the next sound. */
        gta_audio_tick();
        gta_audio_music_tick();

        /* THE CAR RADIO, driven by the CHANGE rather than hooked into each
         * of the four places `in_car` is set. Getting out happens on being
         * wasted, on being busted and on the ordinary exit, and a hook on
         * one of those is a hook somebody forgets on the next one. */
        {
            static int radio_was = -1;

            /* THE ENGINE NOTE, kept alive while he is in a car and revved
             * from the car's own speed. It is a hardware loop on a reserved
             * channel (gta_audio.h), so all this costs per frame is one
             * comparison and, when the speed has actually changed, one
             * ADCMD_PERVOL. */
            if (in_car) {
                /* THE ORIGINAL'S ENGINE - see engine_rate(). The speed in
                 * its units, signed by whether the car moves the way its
                 * nose points (scaled down before the multiply: 32 bits). */
                const gta_car_info *ci_ = &tiles.cars[veh.model];
                long vx_ = veh.vx < 0 ? -veh.vx : veh.vx;
                long vy_ = veh.vy < 0 ? -veh.vy : veh.vy;
                long mag_ = vx_ > vy_ ? vx_ + vy_ / 2 : vy_ + vx_ / 2;
                long nx_, ny_, hz_;
                int v_ = (int)(mag_ / 32768L), pct_;
                int n_ = opt_engine >= 0 ? opt_engine : 0x2d + ci_->engine;
                gta_veh_nose(&veh, veh.ox, veh.oy, veh.ang16, &nx_, &ny_);
                if ((veh.vx >> 8) * ((nx_ - veh.ox) >> 8)
                    + (veh.vy >> 8) * ((ny_ - veh.oy) >> 8) < 0)
                    v_ = -v_;
                /* a new car is a new engine: start() restarts it when the
                 * sample differs and does nothing when it does not */
                gta_audio_engine_start(&sfx, n_);
                hz_ = engine_rate(ci_->sound_function, n_, v_, veh_air, &pct_);
                /* the engine is the floor of the mix, not an event in it:
                 * 100% is 48 of Paula's 64 */
                gta_audio_engine_rate(hz_, 48 * pct_ / 100);

                /* THE SKID - see GTA_SND_SKID. The slide in the original's
                 * force units is 20 x skid / skid_level: skid_level is the
                 * port's own "20" for this car's mass (gta_vehphys.c).
                 * The loop holds it for as long as the car slides; one
                 * effect channel re-plays it each time it runs out. */
                {
                    static unsigned long skid_until;
                    int bus_ = ci_->sound_function == 5;
                    long x_ = veh.skid_level > 0
                            ? 20L * veh.skid / veh.skid_level : 0;
                    int a_ = v_ < 0 ? -v_ : v_;
                    if (handbrake && a_ > 2)
                        x_ = 21;
                    if (bus_) x_ /= 2;
                    if ((bus_ ? (a_ > 14 && x_ > 20) : x_ > 12)
                        || (handbrake && a_ > 1)) {
                        unsigned long now_ = amiga_uclock_us();
                        if ((long)(now_ - skid_until) >= 0) {
                            long shz_ = 8000L + 150L * x_;
                            unsigned long len_ = GTA_SND_SKID < sfx.count
                                ? sfx.entry[GTA_SND_SKID].length : 0;
                            gta_audio_play_hz(&sfx, GTA_SND_SKID,
                                              48 * pct_ / 100, shz_);
                            skid_until = now_ + len_ * 1000UL
                                                / (unsigned long)shz_ * 1000UL;
                        }
                    }
                    /* AND THE BUS'S AIR BRAKE: stopped, having been above
                     * speed 4 since it last stopped (car+0x13e) */
                    {
                        static int bus_top;
                        if (bus_) {
                            if (a_ > bus_top) bus_top = a_;
                            if (a_ == 0 && bus_top > 4) {
                                gta_audio_play(&sfx, GTA_SND_AIR_BRAKE, 48, 0);
                                bus_top = 0;
                            }
                        } else {
                            bus_top = 0;
                        }
                    }
                }
            } else if (gta_audio_engine_playing()) {
                gta_audio_engine_stop();
            }

            if (in_car != radio_was) {
                radio_was = in_car;
                if (in_car) {
                    /* A POLICE CAR CARRIES THE POLICE BAND, and an ordinary
                     * car never does. The model is asked rather than the way
                     * the player got in: he may have found the car empty,
                     * dragged its driver out, or started the level in it,
                     * and only one of those sets a "grabbed a cop car" flag.
                     */
                    int cop = gta_traffic_cop_model(&traffic);
                    if (cop >= 0 && veh.model == cop)
                        gta_audio_radio_police();
                    else
                        gta_audio_radio_next();
                } else {
                    gta_audio_music_stop();
                }
            }
        }

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
                /* QUIT GAME? is up: only the answer counts. Y or RETURN
                 * quits, N or ESC plays on; every other key is swallowed so
                 * nothing moves behind the card. */
                if (g_quit_ask) {
                    if (!held) {
                        if (code == KEY_Y || code == KEY_RETURN) {
                            running = 0;
                            printf("gta: quit - confirmed\n");
                        } else if (code == KEY_N || code == KEY_ESC) {
                            g_quit_ask = 0;
                            printf("gta: quit - cancelled, playing on\n");
                        }
                        fflush(stdout);
                    }
                    continue;
                }
                switch (code) {
                case KEY_UP:     up = held;    break;
                case KEY_DOWN:   down = held;  break;
                case KEY_LEFT:   left = held;  break;
                case KEY_RIGHT:  right = held; break;
                case KEY_LSHIFT:
                case KEY_RSHIFT: fast = held;  break;
                case KEY_ESC:
                    /* During a test script ESC still ends the run at once -
                     * that is how the developer stops one. In play it asks. */
                    if (!held) {
                        if (adq_i < adq_n) {
                            running = 0;
                        } else {
                            g_quit_ask = 1;
                            up = down = left = right = 0;
                            fast = 0;
                            printf("gta: quit? - asking\n");
                            fflush(stdout);
                        }
                    }
                    break;
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
                    /* THE WINDOW SIZE, smallest first, wrapping. Four
                     * rectangles inside the ordinary screen and then two
                     * bigger SCREENS - 320x256 and PAL overscan - which the
                     * machine may refuse; view_mode_set puts the old one
                     * back if it does. Only 320, 256 and 192 wide are
                     * offered because those are the widths whose left edge
                     * is also on the c2p's 32-pixel grid; anything else has
                     * its black bars converted along with the picture and
                     * saves nothing. */
                    if (!held) {
                        if (!view_mode_set(&view,
                                           (mode_narrow + 1) % VIEW_MODES))
                            running = 0;
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
            unsigned long sim_t0;
            int ticks = 0;

            sim_last = now;
            if (dt > (unsigned long)(SIM_US * opt_catchup))
                dt = (unsigned long)(SIM_US * opt_catchup);
            sim_accum += ((unsigned long)dt * (unsigned long)game_speed) / 100UL;
            /* the world stands still under the card - except under a test
             * script, whose clock IS the simulation's: paused, it never
             * reached its next line (quitcard.txt, 218) */
            if (g_quit_ask && adq_i >= adq_n)
                sim_accum = 0;
            /* ...and the reload file is polled here too while the card is
             * up: its usual poll is in the tick loop, which is not running,
             * and a paused game ignored the harness for good (218) */
            if (g_quit_ask && !g_reload && (++quit_poll % 25) == 0) {
                FILE *rf = fopen(GTA_DIR "reload.txt", "r");
                if (rf) {
                    fclose(rf);
                    remove(GTA_DIR "reload.txt");
                    g_reload = 1;
                    running = 0;
                    log_line("gta: reload - leaving with RC 5 (from the "
                             "QUIT card)");
                }
            }

            amiga_wd_set(AMIGA_WD_PHASE_SIM);
            /* A TIME BUDGET, NOT ONLY A COUNT. The cap alone made the world
             * run in slow motion whenever the frame rate fell below
             * 50 / MAX_CATCHUP, which the developer read as a broken frame
             * skip. The count is higher now and the loop also stops once the
             * ticks of THIS frame have eaten SIM_BUDGET_US of real time - so
             * the sim keeps pace with the clock while it can afford to, and
             * the catch-up spiral of PROGRESS 68 (ticks that cost more than
             * the time they repay) is impossible by construction rather than
             * merely capped. */
            sim_t0 = now;
            while (sim_accum >= (unsigned long)SIM_US && ticks < opt_catchup &&
                   (ticks == 0 ||
                    (unsigned long)(amiga_uclock_us() - sim_t0) < SIM_BUDGET_US)) {
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
                    case 22:
                        g_quit_ask = 1;
                        printf("gta: quit? - asking\n");
                        fflush(stdout);
                        break;
                    case 23:
                        g_quit_ask = 0;
                        printf("gta: quit - cancelled, playing on\n");
                        fflush(stdout);
                        break;
                    case 24:
                        running = 0;
                        g_reload = 1;
                        printf("gta: quit - confirmed (test: reload code)\n");
                        fflush(stdout);
                        break;
                    case 16:
                        script_brief(0, adq[adq_i].brk, adq[adq_i].thr,
                                     adq[adq_i].brk == GTA_BRIEF_PAGER_T
                                     ? 20 : 0);
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
                    case 17:
                        if (in_car) {
                            /* WITH THE CAR. A job ends at a garage on the
                             * other side of the city and the test has to be
                             * able to arrive there. */
                            veh.ox = veh.x = (((long)adq[adq_i].thr * 32 + 16) << 16);
                            veh.oy = veh.y = (((long)adq[adq_i].brk * 32 + 16) << 16);
                            veh.vx = veh.vy = 0;
                            veh.omega = 0;
                            player.x = veh.ox;
                            player.y = veh.oy;
                            /* ON THE SURFACE THERE, not the layer he left:
                             * the car kept the start's layer 2 and was
                             * carried under the ramp at (91,112), whose road
                             * is layer 3 (208). */
                            {
                                int lz_ = gta_script_stand_layer(&nav,
                                              adq[adq_i].thr, adq[adq_i].brk);
                                if (lz_ >= 0) player.layer = lz_;
                            }
                            gta_render_look_at_block(&view, adq[adq_i].thr,
                                                     adq[adq_i].brk);
                            printf("gta: goto (%d,%d) in the car, layer %d\n",
                                   adq[adq_i].thr, adq[adq_i].brk,
                                   player.layer);
                        } else if (gta_player_init(&player, &map, &tiles,
                                                   adq[adq_i].thr,
                                                   adq[adq_i].brk)) {
                            gta_render_look_at_block(&view, adq[adq_i].thr,
                                                     adq[adq_i].brk);
                            printf("gta: goto (%d,%d) layer %d\n",
                                   adq[adq_i].thr, adq[adq_i].brk,
                                   player.layer);
                        } else {
                            printf("gta: goto (%d,%d) - nothing walkable "
                                   "there\n",
                                   adq[adq_i].thr, adq[adq_i].brk);
                        }
                        fflush(stdout);
                        break;
                    case 19:
                        gta_audio_play(&sfx, adq[adq_i].thr,
                                       adq[adq_i].brk, 0);
                        break;
                    case 21:
                        score.score += adq[adq_i].thr;
                        printf("gta: autodrive - score +%d, now %ld\n",
                               adq[adq_i].thr, score.score);
                        fflush(stdout);
                        break;
                    case 20:
                        printf("gta: autodrive - script line %d started as "
                               "process %d\n", adq[adq_i].thr,
                               gta_script_debug_start(&script, adq[adq_i].thr));
                        fflush(stdout);
                        break;
                    case 18:
                        if (adq[adq_i].thr >= 0
                            && adq[adq_i].thr < VIEW_MODES) {
                            if (!view_mode_set(&view, adq[adq_i].thr))
                                running = 0;
                        } else {
                            printf("gta: mode %d - there are %d\n",
                                   adq[adq_i].thr, VIEW_MODES);
                            fflush(stdout);
                        }
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
                /* FREEZE_ENTER / FREEZE_TIMED - the original ignores the
                 * enter key outright while player+0x176 is set. */
                if (freeze_ticks > 0)
                    freeze_ticks--;
                if (enter_req && (freeze_enter || freeze_ticks > 0)) {
                    enter_req = 0;
                    printf("gta: RETURN refused - the script holds him\n");
                    fflush(stdout);
                }
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
                            veh_serial = gta_traffic_last_grab_serial(&traffic);
                            veh_mission = gta_traffic_last_grab_mission(&traffic);
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
                            if (!leave_car(veh.model, veh.ox, veh.oy, a_,
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
                        enter_step == 0 && enter_tick == 0) {
                        door_tick = 0;
                        /* THE DOOR OPENS ON SOMEBODY'S RADIO - see
                         * GTA_SND_RADIO_BASE: the car sound loop plays the
                         * model's radio for a car that is not the player's
                         * while its door moves. Only a car with somebody in
                         * it has a radio on. */
                        if (enter_driver) {
                            static const signed char radio_snd[6] =
                                { 0x7e, 0x7f, -1, 0x80, 0x81, 0x82 };
                            int r_ = tiles.cars[enter_model].radio;
                            if (r_ >= 0 && r_ < 6 && radio_snd[r_] >= 0)
                                gta_audio_play(&sfx, radio_snd[r_], 48, 0);
                        }
                    }
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
                                 * and comes after him on foot. It is a
                                 * carjacking and nothing more - the +15
                                 * above. The head this used to force was
                                 * the port's own invention; the original's
                                 * two instant-level-1 triggers are HITTING
                                 * a police car with your car and SHOOTING
                                 * one, and the second of those is now where
                                 * it belongs, in the weapon. */
                                gta_peds_make_cop(&peds, peds.last_index);
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
                            /* the original's routine state 0x22: the door, and the
                             * car started (GTA_SND_DOOR / _CAR_START). With
                             * music on and the engine note running only one
                             * effect channel is free, so the second of the
                             * two takes it from the first. */
                            gta_audio_play(&sfx, GTA_SND_DOOR, 64, 0);
                            gta_audio_play(&sfx, GTA_SND_CAR_START, 64, 0);
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
                            if (!leave_car(veh.model, veh.ox, veh.oy,
                                           enter_a0, player.layer, veh.remap,
                                           veh.damage))
                                printf("gta: fleet full, car lost\n");
                            printf("gta: on foot at (%ld,%ld) facing %d\n",
                                   player.x >> 16, player.y >> 16,
                                   player.angle);
                            /* state 0x19: the door shut behind him */
                            gta_audio_play(&sfx, GTA_SND_DOOR, 64, 0);
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
                        /* over the car while the point ahead OR he himself
                         * (with the margin the walk keeps from a car) is on
                         * it - the point alone put him down on the far
                         * wing, and the walk then let him out of it (218) */
                        int still = fleet_car_at(&traffic, &tiles,
                                                 player.x + (fx << 2),
                                                 player.y + (fy << 2),
                                                 player.layer, 0, 0) >= 0 ||
                                    fleet_car_at(&traffic, &tiles,
                                                 player.x, player.y,
                                                 player.layer,
                                                 PED_CAR_MARGIN, 0) >= 0;
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
                    /* A CAR DOES NOT MOVE 64 PIXELS IN A TICK. When it does,
                     * say so with what it was doing - the record-17 car on
                     * the crane bay (PROGRESS 190) went to (4096,0) and the
                     * log had nothing to say about how. */
                    {
                        long jx_ = (veh.ox - wox0_) >> 16, jy_ = (veh.oy - woy0_) >> 16;
                        if (jx_ > 64 || jx_ < -64 || jy_ > 64 || jy_ < -64) {
                            printf("gta: CAR JUMP model %d (%ld,%ld) -> (%ld,%ld) in "
                                   "one step: v (%ld,%ld) ang16 %ld -> %ld, "
                                   "throttle %d brake %d steer %d hb %d road %d\n",
                                   veh.model, wox0_ >> 16, woy0_ >> 16,
                                   veh.ox >> 16, veh.oy >> 16, veh.vx, veh.vy,
                                   wang0_, veh.ang16, up ? 1 : 0, down ? 1 : 0,
                                   (right ? 1 : 0) - (left ? 1 : 0), handbrake, road_);
                            fflush(stdout);
                        }
                    }
                    if (veh.sliding) veh_slide_ticks++;
                    /* ---- THE JUMP ------------------------------------
                     *
                     * A ramp with a hole after it is a jump, and Liberty City
                     * has several: (91,105..110) is two ramps and two blocks
                     * of air over the water. The port used to stop dead at
                     * the edge, because a car had a layer and no HEIGHT and
                     * `gta_veh_wall` reads air as a wall.
                     *
                     * TAKING OFF: the car is on a ramp, driving UP it, and
                     * the block it is about to enter has no surface on this
                     * layer. Then it leaves the ground, and while it is off
                     * the ground neither the wall test nor the layer test
                     * runs - there is nothing under it to test against.
                     *
                     * The numbers are the original's, from
                     * the original: lift 4 units a tick for
                     * `speed / 4` ticks, gravity 8, height capped at five
                     * blocks. */
                    if (!veh_air) {
                        int bx_ = (int)(veh.ox >> 21), by_ = (int)(veh.oy >> 21);
                        int up_ = gta_map_slope_up_dir(&map, bx_, by_,
                                                       player.layer);
                        if (up_ >= 0) {
                            static const int dxs[4] = { 0, 1, 0, -1 };
                            static const int dys[4] = { -1, 0, 1, 0 };
                            long vx_ = veh.vx < 0 ? -veh.vx : veh.vx;
                            long vy_ = veh.vy < 0 ? -veh.vy : veh.vy;
                            long sp_ = (vx_ > vy_ ? vx_ : vy_) >> 16;
                            /* AN ANGLE IS NOT AN ARRAY INDEX, and this line
                             * read sixty-four entries past a four-entry
                             * table for weeks.
                             *
                             * `gta_map_slope_up_dir()` returns the port's own
                             * ANGLE - 0 north, 64 east, 128 south, 192 west -
                             * which is what gta_veh_layer() compares against
                             * gta_map_step_dir(). This code indexed dxs[] and
                             * dys[] with it directly, so on the ramp the
                             * developer drove up, `up_` was 64 and the "block
                             * ahead" came out as (1316302790,610796522).
                             * Nothing is solid there, so the car LAUNCHED off
                             * every single block of the ramp - five jumps in
                             * a row - and each little ballistic arc grew the
                             * sprite and shrank it again. That is the
                             * "wielkosc auta faluje" the developer reported,
                             * and the moment it appeared ABOVE the bridge
                             * scaffolding was one of those arcs.
                             *
                             * The index is the angle in quarter turns. */
                            int ud_ = (up_ >> 6) & 3;
                            /* Going the way the ramp rises? */
                            int going = (dxs[ud_] > 0 && veh.vx > 0)
                                     || (dxs[ud_] < 0 && veh.vx < 0)
                                     || (dys[ud_] > 0 && veh.vy > 0)
                                     || (dys[ud_] < 0 && veh.vy < 0);
                            int ax_ = bx_ + dxs[ud_], ay_ = by_ + dys[ud_];
                            /* ...and is there anything to drive on to? */
                            int solid = gta_script_stand_layer(&nav, ax_, ay_)
                                        >= player.layer;
                            /* A RAMP THAT CONTINUES IS NOT A LAUNCH, and
                             * leaving this out was the developer's report
                             * that a car "ciagle sie powieksza i pomniejsza"
                             * going up a ramp.
                             *
                             * The test above asks whether there is anything
                             * SOLID to drive on to, and a ramp block is not
                             * solid at its own layer - its ground type is
                             * the slope, which gta_veh_layer() excludes by
                             * name. On the two-block ramp with water after
                             * it that this code was written for, that is
                             * exactly right. On Liberty City's gentle
                             * eight-block ramps it fired on EVERY BLOCK: the
                             * car took off five times in a row, and each
                             * little ballistic arc grew the sprite and shrank
                             * it again. Five jumps, five pulses, one per
                             * block - which is what the log showed.
                             *
                             * So: if the block ahead is itself a ramp on
                             * this layer, the ramp simply continues. */
                            int ahead_ramp = gta_map_slope_up_dir(&map, ax_,
                                                 ay_, player.layer) >= 0;
                            if (going && !solid && !ahead_ramp && sp_ >= 4) {
                                veh_air = 1;
                                /* `speed / 4` of the original's ticks, in
                                 * ours - see the note on VEH_GRAVITY. */
                                veh_lift = (int)sp_ * 2;
                                veh_vz = VEH_LIFT;
                                veh_z = 0;
                                veh_air_from = 0;
                                printf("gta: JUMP from (%d,%d) layer %d at "
                                       "%ld px/tick - %d ticks of climb "
                                       "[up %d ahead (%d,%d) slope %d stand "
                                       "%d]\n",
                                       bx_, by_, player.layer, sp_, veh_lift,
                                       up_, ax_, ay_,
                                       gta_map_slope_up_dir(&map, ax_, ay_,
                                                            player.layer),
                                       gta_script_stand_layer(&nav, ax_, ay_));
                                fflush(stdout);
                            }
                        }
                    }

                    if (veh_air) {
                        /* THE ARC. Nothing under the car is touched while it
                         * is up here: no wall, no layer, no road snap. */
                        long dax_ = veh.ox - wox0_, day_ = veh.oy - woy0_;
                        veh_z += veh_vz;
                        veh_air_from += (dax_ < 0 ? -dax_ : dax_)
                                      + (day_ < 0 ? -day_ : day_);
                        if (veh_lift > 0) veh_lift--;
                        else              veh_vz -= VEH_GRAVITY;
                        if (veh_z > VEH_Z_MAX) { veh_z = VEH_Z_MAX; veh_vz = 0; }
                        if (veh_z <= 0 && veh_vz <= 0) {
                            /* DOWN - but only where there is something to
                             * land ON. Over the gap there is not, and the car
                             * keeps falling; a block below the layer it left,
                             * it is in the water. */
                            int bx_ = (int)(veh.ox >> 21);
                            int by_ = (int)(veh.oy >> 21);
                            int lz_ = gta_script_stand_layer(&nav, bx_, by_);
                            /* ANY SURFACE AT OR BELOW THE LAYER IT LEFT.
                             * The first version compared against
                             * `player.layer + (veh_z >> 21)`, and veh_z is
                             * NEGATIVE by the time it is falling - so the
                             * landing ramp, on the layer it took off from,
                             * failed the test and the car flew through it. */
                            if (lz_ >= 0 && lz_ <= player.layer) {
                                veh_air = 0;
                                veh_z = 0;
                                veh_vz = 0;
                                if (lz_ != player.layer)
                                    player.layer = lz_;
                                printf("gta: LANDED at (%d,%d) layer %d after "
                                       "%ld px\n", bx_, by_,
                                       player.layer, veh_air_from >> 16);
                                fflush(stdout);
                                /* the original's routine: the thump of a car coming
                                 * down (see GTA_SND_LANDING) */
                                gta_audio_play(&sfx, GTA_SND_LANDING, 64, 0);
                            } else if (veh_z < -(long)GTA_TILE_DIM << 16) {
                                /* Short. Whatever is down there takes it. */
                                veh_air = 0;
                                veh_z = 0;
                                veh_vz = 0;
                                printf("gta: jump fell short at (%d,%d)\n",
                                       bx_, by_);
                                fflush(stdout);
                                /* ...and what it fell into answers: the
                                 * first surface under the layer it left,
                                 * water a splash and anything else the
                                 * landing - the original's routine's two sounds */
                                {
                                    int z2_, g2_ = GTA_GROUND_AIR;
                                    for (z2_ = player.layer; z2_ >= 0 &&
                                         g2_ == GTA_GROUND_AIR; z2_--)
                                        g2_ = gta_nav_ground(gta_nav_at_m(
                                                  &nav, bx_, by_, z2_));
                                    gta_audio_play(&sfx,
                                        g2_ == GTA_GROUND_WATER ? GTA_SND_SPLASH
                                                                : GTA_SND_LANDING,
                                        64, 0);
                                }
                            }
                        }
                    }

                    /* AND THE CAR CLIMBS. Until now the layer under a driven
                     * car was frozen at whatever the player was standing on
                     * when he got in, because nothing but gta_player_update()
                     * ever moved it - so a ramp led nowhere and every bridge
                     * was something you drove UNDER. See gta_veh_layer().
                     *
                     * Resolved BEFORE the wall test, so the test runs on the
                     * layer the car has arrived at. */
                    if (!veh_air) {
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
                    wdmg_ = veh_air ? 0
                          : gta_veh_wall(&veh, &nav, player.layer,
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
                        /* THE SOUND OF IT - GTA_SND_WALL, at any impact
                         * above speed 1 in the original's units, which a
                         * whole pixel a tick already is (x3) */
                        gta_audio_play(&sfx, GTA_SND_WALL, 64, 0);
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
                        long hvx_ = veh.vx, hvy_ = veh.vy;
                        int nhit = gta_traffic_ram(&traffic, veh.ox, veh.oy,
                                       gta_veh_angle(&veh),
                                       veh.len / 2, veh.wid / 2,
                                       veh.vx, veh.vy, veh.mass,
                                       player.layer, &rvx, &rvy, &ryaw,
                                       &rpx, &rpy);
                        /* TOUCHING A POLICE CAR IS A HEAD AT ONCE, and it
                         * is not gated on the impact being hard enough to
                         * cost bodywork - see gta_traffic_ram(). */
                        if (traffic.stat_ram_cop != ram_cop_seen) {
                            ram_cop_seen = traffic.stat_ram_cop;
                            gta_score_force_level(&score, 1);
                        }
                        /* THE SPEED IT HIT AT, taken before the response
                         * below takes it away - the heat and the impact's
                         * grade are both about the blow, and measured after
                         * it a ram left the car too slow to count for
                         * either (208: two rams, no 6/7/8, no heat).
                         * hvx_/hvy_ are taken at the top of this block. */
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
                            long avx = hvx_ < 0 ? -hvx_ : hvx_;
                            long avy = hvy_ < 0 ? -hvy_ : hvy_;
                            if (avx >= 5L * 32768L || avy >= 5L * 32768L) {
                                int k;
                                for (k = 0; k < nhit; k++)
                                    gta_score_crime(&score, GTA_CRIME_SHUNT);
                                /* THE IMPACT - see GTA_SND_IMPACT_*: graded by
                                 * the speed, once a contact (gta_traffic_ram
                                 * latches). The original compares car+0x1c,
                                 * and veh.vx/vy are already in its terms:
                                 * 16.16 px per PHYSICS STEP, one unit being
                                 * VEH_SPEED_UNIT (32768) - the shunt test
                                 * just above uses the same. 197 took them
                                 * for px a TICK and scaled by 1.5 (205). */
                                {
                                    long osp = (avx > avy ? avx + avy / 2
                                                          : avy + avx / 2) / 32768L;
                                    gta_audio_play(&sfx, osp < 7 ? GTA_SND_IMPACT_LIGHT
                                                         : osp < 17 ? GTA_SND_IMPACT_MED
                                                         : GTA_SND_IMPACT_HARD, 64, 0);
                                    /* and above 10, one time in two, the
                                     * driver yells (the original's routine) */
                                    if (osp > 10) {
                                        static unsigned long crash_dice = 1;
                                        crash_dice = crash_dice * 1103515245UL
                                                   + 12345UL;
                                        if ((crash_dice >> 16) % 100 > 50)
                                            street_yell(veh.ox, veh.oy,
                                                        player.layer,
                                                        veh.ox, veh.oy);
                                    }
                                }
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
                            /* THE PLAYER'S OWN SHARE, by the same formula
                             * and his own car's mass - see
                             * gta_traffic_ram(). It used to be one point a
                             * car touched, whatever hit what at whatever
                             * speed. */
                            veh.damage += traffic.pl_damage;
                            /* The panel that took it: the resolution vector
                             * points out of the other body, so the contact is
                             * the other way. */
                            veh.dmg_bits |= 1UL << gta_car_panel_delta(
                                &tiles.cars[veh.model], veh.ox, veh.oy,
                                gta_veh_angle(&veh),
                                veh.ox - rpx * 8, veh.oy - rpy * 8);
                            printf("gta: ram x%d - player dv (%ld,%ld) "
                                   "damage %d (+%d this crash), police cars "
                                   "hit %ld\n", nhit,
                                   rvx >> 16, rvy >> 16, veh.damage,
                                   traffic.pl_damage, traffic.stat_ram_cop);
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
                        long pox_ = player.x, poy_ = player.y;
                        gta_player_update(&player, &map,
                                          (right ? 1 : 0) - (left ? 1 : 0),
                                          (up ? 1 : 0) - (down ? 1 : 0),
                                          fast);
                        ped_car_block(&player, &traffic, &tiles, pox_, poy_);
                        /* THE SPEED-UP: A SECOND STEP. He covers a fixed
                         * distance per tick, so "twice as fast" is two
                         * ticks - and the collision, the layer change and
                         * the walk cycle all run again with it, which
                         * simply doubling the step would not. He does not
                         * turn twice: that would spin him. */
                        /* THE ENDLESS WEAPON'S CLOCK. When it runs out the
                         * player gets back exactly what he was carrying, which
                         * is why it was saved rather than merely counted. */
                        if (script_frenzy_stop_req) {
                            script_frenzy_stop_req = 0;
                            if (inf_w) inf_ticks = 1;   /* runs out below, now */
                        }
                        if (inf_w) {
                            if (--inf_ticks <= 0) {
                                int k3;
                                for (k3 = 0; k3 < GTA_WEAPON_COUNT; k3++)
                                    ammo[k3] = inf_save_ammo[k3];
                                weapon = ammo[inf_save_w] > 0 || inf_save_w == 0
                                       ? inf_save_w : GTA_WEAPON_FIST;
                                printf("gta: the endless weapon runs out - "
                                       "back to %d\n", weapon);
                                fflush(stdout);
                                inf_w = 0;
                            }
                        }
                        if (player_speed > 0) {
                            player_speed--;
                            pox_ = player.x; poy_ = player.y;
                            gta_player_update(&player, &map, 0,
                                              (up ? 1 : 0) - (down ? 1 : 0),
                                              fast);
                            ped_car_block(&player, &traffic, &tiles, pox_, poy_);
                            if (player_speed == 0) {
                                printf("gta: the speed wears off\n");
                                fflush(stdout);
                            }
                        }
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
                            gta_audio_play(&sfx, GTA_SND_PUNCH, 64, 0);
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
                            /* THE SHOT. One sound per weapon, the original's
                             * own ids (WEAPONS.md's summary table). The
                             * machine gun and the flamethrower fire every
                             * tick, so this is also the busiest caller the
                             * audio layer has - which is what the channel
                             * stealing is for. */
                            gta_audio_play(&sfx, weapon_snd[weapon], 64, 0);
                            gta_peds_panic(&peds, player.x, player.y,
                                           player.layer);
                            panic_screams(player.x, player.y);
                            /* The machine gun and the flamethrower get
                             * five shots out of one unit; the pistol and the
                             * rocket launcher spend one each. */
                            if (inf_w == weapon) {
                                /* endless: the rounds are not counted */
                            } else if (ammo_sub[weapon] > 0) {
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
                    script_in_car = in_car;
                    script_veh_model = in_car ? veh.model : -1;
                    script_veh_damage = in_car ? veh.damage : 0;
                    script_veh_x = veh.ox;
                    script_veh_y = veh.oy;
                    {
                        /* THE CAR'S SPEED is its velocity, which the physics
                         * keeps as a vector of 16.16 pixels per step; the
                         * larger component is enough to say "stopped". */
                        long ax_ = veh.vx < 0 ? -veh.vx : veh.vx;
                        long ay_ = veh.vy < 0 ? -veh.vy : veh.vy;
                        script_veh_speed = ax_ > ay_ ? ax_ : ay_;
                    }
                    /* THE PHONE RINGING - see GTA_SND_PHONE: the NEAREST
                     * ringing phone, at the distance's volume (the
                     * positional rule: sixteen blocks, louder nearer), and
                     * again when the ring before it has run out. */
                    {
                        static int ring_left;
                        long px_ = in_car ? veh.ox : player.x;
                        long py_ = in_car ? veh.oy : player.y;
                        long bd_ = 0x1ffffffL;
                        int k_, best_ = -1;
                        if (ring_left > 0)
                            ring_left--;
                        for (k_ = 0; k_ < script.n_phones; k_++) {
                            const gta_placed *ph_ = &script.phone[k_];
                            long dx_, dy_;
                            if (!ph_->ring || ph_->dead)
                                continue;
                            dx_ = ph_->x - px_; if (dx_ < 0) dx_ = -dx_;
                            dy_ = ph_->y - py_; if (dy_ < 0) dy_ = -dy_;
                            if (dx_ + dy_ < bd_) { bd_ = dx_ + dy_; best_ = k_; }
                        }
                        if (best_ >= 0 && ring_left == 0) {
                            const gta_placed *rp_ = &script.phone[best_];
                            int rv_ = (int)((0x1ffffffL - bd_) >> 19);
                            if (covered(rp_->x, rp_->y, rp_->layer)) rv_ >>= 1;
                            gta_audio_play(&sfx, GTA_SND_PHONE, rv_, 0);
                            /* the ring's own length, in 50 Hz ticks */
                            ring_left = (GTA_SND_PHONE < sfx.count &&
                                         sfx.entry[GTA_SND_PHONE].rate > 0)
                                ? (int)(sfx.entry[GTA_SND_PHONE].length * 50UL
                                        / sfx.entry[GTA_SND_PHONE].rate) + 1
                                : 105;
                        }
                    }
                    /* A BURNING WRECK - see GTA_SND_WRECK_FIRE: the
                     * explosion sets the wreck alight for good (+0xf9) and
                     * the car sound loop plays 0x4b for it; the nearest
                     * wreck within the loop's sixteen blocks crackles, the
                     * sample re-played as it runs out. */
                    {
                        static int crackle_left;
                        long px_ = in_car ? veh.ox : player.x;
                        long py_ = in_car ? veh.oy : player.y;
                        long bd_ = 0x1ffffffL;
                        int k_, best_ = -1;
                        if (crackle_left > 0)
                            crackle_left--;
                        for (k_ = 0; k_ < traffic.n; k_++) {
                            const gta_car *wc_ = &traffic.cars[k_];
                            long dx_, dy_;
                            if (wc_->done || !wc_->wrecked)
                                continue;
                            dx_ = wc_->x - px_; if (dx_ < 0) dx_ = -dx_;
                            dy_ = wc_->y - py_; if (dy_ < 0) dy_ = -dy_;
                            if (dx_ + dy_ < bd_) { bd_ = dx_ + dy_; best_ = k_; }
                        }
                        if (best_ >= 0 && crackle_left == 0) {
                            const gta_car *wb_ = &traffic.cars[best_];
                            int wv_ = (int)((0x1ffffffL - bd_) >> 19);
                            if (covered(wb_->x, wb_->y, wb_->layer)) wv_ >>= 1;
                            gta_audio_play(&sfx, GTA_SND_WRECK_FIRE, wv_, 0);
                            crackle_left = (GTA_SND_WRECK_FIRE < sfx.count &&
                                            sfx.entry[GTA_SND_WRECK_FIRE].rate > 0)
                                ? (int)(sfx.entry[GTA_SND_WRECK_FIRE].length * 50UL
                                        / sfx.entry[GTA_SND_WRECK_FIRE].rate) + 1
                                : 31;
                        }
                    }
                    /* THE FRENZY'S CLOCKS, at the original's 25 a second:
                     * this runs at 50. */
                    {
                        static int kf_half;
                        int k_;
                        kf_half ^= 1;
                        if (!kf_half)
                            for (k_ = 0; k_ < 2; k_++)
                                if (kf_ticks[k_] >= 0) kf_ticks[k_]--;
                    }
                    /* THE CHEER. the original's routine ends every kill it scores
                     * with the original's routine when the frenzy's clock is running
                     * or the frenzy's weapon is in hand: one of lines
                     * 7..10 at volume 60..99 of 127, both from one throw
                     * of the dice - and nothing if a line is being said. */
                    {
                        static unsigned long kills_seen;
                        if (score.kills != kills_seen) {
                            kills_seen = score.kills;
                            if (kf_ticks[0] != -1 || inf_w) {
                                int x_ = voice_rand();
                                gta_audio_speak(&voice, 7 + (x_ & 3),
                                                (x_ % 40 + 60) / 2, 0);
                            }
                        }
                    }
                    script_pl_x = player.x;
                    script_pl_y = player.y;
                    gta_script_tick(&script, in_car ? veh.ox : player.x,
                                    in_car ? veh.oy : player.y,
                                    !in_car && !enter_anim);
                    if (script_respray_to >= 0) {
                        if (in_car) {
                            veh.remap = script_respray_to;
                            veh.dmg_bits = 0;      /* out of the shop clean */
                            veh.damage = 0;
                            gta_score_clear_heat(&score);
                            printf("gta: resprayed - colour %d, and the heat "
                                   "is gone\n", script_respray_to);
                            fflush(stdout);
                        }
                        script_respray_to = -1;
                    }
                    arrow_tick(&view, in_car ? veh.ox : player.x,
                               in_car ? veh.oy : player.y,
                               in_car, veh.len);
                    gta_traffic_set_wanted(&traffic, score.level,
                                           in_car || enter_anim == 2);
                    gta_peds_set_player(&peds, in_car ? veh.ox : player.x,
                                        in_car ? veh.oy : player.y,
                                        player.layer, in_car,
                                        in_car ? veh.len / 2 : 0,
                                        in_car ? veh.wid / 2 : 0,
                                        in_car ? gta_veh_angle(&veh) : 0);
                    {
                        long cx_, cy_;
                        int cl_, ca_;
                        if (gta_traffic_cop_out(&traffic, &cx_, &cy_, &cl_, &ca_)) {
                            if (gta_peds_spawn_cop(&peds, cx_, cy_, cl_, ca_)) {
                                printf("gta: police - a cop is on foot at"
                                       " (%ld,%ld), %d out\n", cx_ >> 16, cy_ >> 16,
                                       gta_peds_cops_out(&peds));
                                /* the cop-car AI's state 0x6f, letting the
                                 * cop out: the original's routine(cop, 0x12) */
                                cop_shout(cx_, cy_, cl_, in_car ? veh.ox : player.x,
                                          in_car ? veh.oy : player.y, 0x12,
                                          peds.last_index);
                            } else
                                printf("gta: police - no room for the cop\n");
                            fflush(stdout);
                        }
                    }
                    if (peds.stat_cops_killed != cops_killed_seen) {
                        /* A COP KILLED. Every car standing with its driver
                         * out gives up - and that is all: killing a cop
                         * costs exactly what killing anybody costs, which
                         * the weapon has already filed. The extra hundred
                         * that used to be added here was read off the
                         * original's cop bonus, and that bonus is passed
                         * the DEAD COP's ped index rather than the
                         * killer's, so it lands on no player at all
                         * at all. A bug in the original is not a rule to
                         * copy. */
                        cops_killed_seen = peds.stat_cops_killed;
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
                        /* the jingle - the original's routine's the original's routine(0xc) */
                        gta_audio_speak(&voice, GTA_VOICE_BUSTED_WASTED, 64, 1);
                        kf_ticks[0] = -1;   /* the original's routine clears it */
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
                            if (!leave_car(veh.model, veh.ox, veh.oy, a_,
                                           player.layer, veh.remap,
                                           veh.damage))
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
                        /* and the cars that were chasing him go back to
                         * their routes - see gta_traffic_police_reset() */
                        gta_traffic_police_reset(&traffic);
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
                                            (render_w() / 2) / zoom_eff() + 1);
                    amiga_wd_set(AMIGA_WD_PHASE_TRAFFIC);
                    gta_traffic_tick(&traffic, &map, view.cam_x, view.cam_y);
                    amiga_wd_set(AMIGA_WD_PHASE_SIM);
                }
                /* The spawner puts people on the edge of the view ahead of
                 * the player: it needs the view in blocks and his heading
                 * (the car's when he drives). */
                gta_peds_set_view(&peds,
                                  (render_w() / 2) / zoom_eff() + 1,
                                  (SCREEN_H / 2) / zoom_eff() + 1,
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
                /* THE SIREN - see GTA_SND_SIREN: the nearest police car
                 * with its siren on (a DISPATCHED one here, cop >= 2 - the
                 * same cars whose lights flash). 206 corrected 195: there
                 * is no 60-frame counter - the byte 195 read as one is the
                 * model's `horn` - and with the lights on (+0x113 == 1)
                 * the car sound loop holds 0x43 for as long as they are,
                 * so it is played again when it runs out (its own length,
                 * 2.65 s), within the loop's sixteen blocks and louder the
                 * nearer. (+0x113 == 2, "sounding", plays 0x42 instead -
                 * the police AI's reason for it is not ported yet.) */
                {
                    static int siren_wait = 0;
                    if (siren_wait > 0) {
                        siren_wait--;
                    } else {
                        long plx = in_car ? veh.ox : player.x;
                        long ply = in_car ? veh.oy : player.y;
                        long bd = -1;
                        int j, bj = -1;
                        for (j = 0; j < traffic.n; j++) {
                            const gta_car *c = &traffic.cars[j];
                            long ddx, ddy, d;
                            if (c->done || c->cop < 2 || c->wrecked)
                                continue;
                            ddx = (c->x - plx) >> 16; ddy = (c->y - ply) >> 16;
                            if (ddx < 0) ddx = -ddx;
                            if (ddy < 0) ddy = -ddy;
                            d = ddx + ddy;
                            if (d < 512 && (bd < 0 || d < bd)) { bd = d; bj = j; }
                        }
                        if (bd >= 0) {
                            /* SOUNDING (+0x113 == 2) when something is in
                             * its way: the original's routine switches it on while
                             * the original's routine's look-ahead finds a car or a
                             * man under three blocks in front, and the
                             * police model (horn 127) then plays 0x42, the
                             * yelp. The port's look-ahead is gap_ahead():
                             * lead_kind != 0 is "something in front". */
                            int snd = traffic.cars[bj].lead_kind != 0
                                    ? GTA_SND_SIREN_YELP : GTA_SND_SIREN;
                            int vol = (int)((512 - bd) / 8);
                            if (covered(traffic.cars[bj].x, traffic.cars[bj].y,
                                        traffic.cars[bj].layer)) vol >>= 1;
                            if (vol < 1) vol = 1;
                            gta_audio_play(&sfx, snd, vol, 0);
                            siren_wait = (snd < sfx.count &&
                                          sfx.entry[snd].rate > 0)
                                ? (int)(sfx.entry[snd].length * 50UL
                                        / sfx.entry[snd].rate) + 1
                                : 133;
                        }
                    }
                }
                /* THE AI CARS' HORNS AND YELLS - see gta_snd.h. The fleet
                 * decides (gta_traffic: honk, yell_req); this is the loop's
                 * half: the nearest honking car with a horn (< 60) within
                 * sixteen blocks sounds its model's horn, 0x3a + horn / 10
                 * at its rate + (horn % 10) x 512, on every step of its
                 * pattern that turns the horn ON. */
                {
                    static const char *const honk_rows[10] = {
                        "########################################..........",
                        "##########....####....####....##########..........",
                        "##########.............#################..........",
                        "############...#####....################..........",
                        "######.....###################...#######..........",
                        "########################.....###########..........",
                        "#######....######....####...############..........",
                        "######....#############...........................",
                        "######...###..#######.............................",
                        "#############...###..########..###................"
                    };
                    static unsigned long honk_serial;
                    static int honk_was_on, honk_left;
                    long plx = in_car ? veh.ox : player.x;
                    long ply = in_car ? veh.oy : player.y;
                    long bd = 0x1ffffffL;
                    int j, bj = -1;
                    if (traffic.yell_req) {
                        traffic.yell_req = 0;
                        street_yell(traffic.yell_x, traffic.yell_y,
                                    traffic.yell_layer, plx, ply);
                    }
                    for (j = 0; j < traffic.n; j++) {
                        const gta_car *c = &traffic.cars[j];
                        long ddx, ddy;
                        if (c->done || c->honk == 0 || c->honk > 40 ||
                            c->honk_pat == 0 ||
                            tiles.cars[c->model].horn >= 60)
                            continue;
                        ddx = c->x - plx; if (ddx < 0) ddx = -ddx;
                        ddy = c->y - ply; if (ddy < 0) ddy = -ddy;
                        if (ddx + ddy < bd) { bd = ddx + ddy; bj = j; }
                    }
                    if (bj < 0) {
                        honk_was_on = 0;
                    } else {
                        const gta_car *c = &traffic.cars[bj];
                        int on = honk_rows[c->honk_pat - 1][49 - c->honk] == '#';
                        if (c->serial != honk_serial) { honk_serial = c->serial; honk_was_on = 0; }
                        /* HELD, not tapped: the loop keeps the horn on its
                         * list for every frame the pattern is on, so the
                         * short sample is played again as it runs out for
                         * as long as the step lasts (the first version
                         * tapped it once a step - 215). */
                        if (honk_left > 0)
                            honk_left--;
                        if (on && (!honk_was_on || honk_left == 0)) {
                            int h = tiles.cars[c->model].horn;
                            int n = 0x3a + h / 10;
                            long hz = (n < sfx.count ? (long)sfx.entry[n].rate : 11025L)
                                    + (long)(h % 10) * 512L;
                            int hv = (int)((0x1ffffffL - bd) >> 19);
                            if (covered(c->x, c->y, c->layer)) hv >>= 1;
                            gta_audio_play_hz(&sfx, n, hv, hz);
                            honk_left = n < sfx.count && hz > 0
                                ? (int)(sfx.entry[n].length * 50UL
                                        / (unsigned long)hz) + 1
                                : 6;
                        }
                        honk_was_on = on;
                    }
                }
                alarms_tick(in_car ? veh.ox : player.x,
                            in_car ? veh.oy : player.y);
                bombs_tick(&veh, in_car && script_in_car);
                cranes_tick(in_car ? veh.ox : player.x, in_car ? veh.oy : player.y);
                /* EVERY BLAST IS HEARD. Until 2026-09-18 none was: the
                 * rocket, the wrecked car and now the script's explosions
                 * all go through gta_weapons_explode(), which counts them,
                 * and nothing played GTA_SND_EXPLOSION (the original's
                 * the original's routine does it for every one). */
                {
                    static long heard_expl = 0;
                    if (weapons.stat_expl != heard_expl) {
                        heard_expl = weapons.stat_expl;
                        gta_audio_play(&sfx, GTA_SND_EXPLOSION, 64, 0);
                    }
                }
                amiga_wd_set(AMIGA_WD_PHASE_PLAYER);
                if (pickups.n > 0 && bust_timer == 0) {
                    long px_ = in_car ? veh.ox : player.x;
                    long py_ = in_car ? veh.oy : player.y;
                    int kind_, amount_;
                    gta_pickups_open_at(&pickups, px_, py_, player.layer,
                                        in_car ? 22 : 10);
                    /* CAN HE USE IT? The original asks before it consumes
                     * (WEAPONS.md, the original's routine's guard), and a crate it
                     * cannot use is LEFT STANDING - which matters in a game
                     * where they do not come back. */
                    {
                        /* ONE AT A TIME, so a crate he cannot use does not
                         * stand in front of one he can - which is exactly
                         * what the first version did: a refused armour crate
                         * hid every crate behind it for ever. */
                        int idx_ = -1, want_ = -1, from_ = 0;
                        for (;;) {
                        int refuse = 0;
                        idx_ = gta_pickups_peek_from(&pickups, px_, py_,
                                                     player.layer,
                                                     in_car ? 22 : 10, from_,
                                                     &kind_, &amount_);
                        if (idx_ < 0) break;
                        from_ = idx_ + 1;
                        if (kind_ >= 1 && kind_ <= 4) {
                            if (amount_ < 100) {
                                /* an ordinary crate, and that ammo is full */
                                refuse = ammo[kind_] >= 99;
                            } else {
                                /* an endless one, and one is already running */
                                refuse = inf_w != 0;
                            }
                        } else if (kind_ == GTA_PICKUP_ARMOUR) {
                            refuse = player_armour >= 3;
                        } else if (kind_ == GTA_PICKUP_JAILFREE) {
                            refuse = jail_free != 0;
                        }
                        if (refuse) {
                            static int said;
                            if (said != kind_) {
                                said = kind_;
                                printf("gta: crate kind %d left where it is - "
                                       "nothing to gain from it\n", kind_);
                                fflush(stdout);
                            }
                            continue;       /* try the next one in reach */
                        }
                        want_ = idx_;
                        break;
                        }
                        if (want_ >= 0
                            && gta_pickups_take_index(&pickups, want_,
                                                      &kind_, &amount_)) {
                        static const char *const kind_name[16] = {
                            "?", "pistol", "machine gun", "rocket", "flame",
                            "?", "speed", "speed", "speed", "bribe", "armour",
                            "multiplier", "jail free", "life", "kill frenzy", "life" };
                        printf("gta: picked up %s %d\n",
                               kind_ >= 0 && kind_ < 16 ? kind_name[kind_] : "?",
                               amount_);
                        /* WEAPONS.md: "Pickup sounds: weapons 2, other kinds
                         * 4 (even) / 3 (odd)" - the original's own rule, and
                         * the split is on the KIND, not on the crate. */
                        gta_audio_play(&sfx,
                                       (kind_ >= 1 && kind_ <= 4)
                                           ? GTA_SND_PICKUP_GUN
                                           : ((kind_ & 1) ? GTA_SND_PICKUP_ODD
                                                          : GTA_SND_PICKUP_EVEN),
                                       64, 0);
                        if (kind_ >= 1 && kind_ <= 4) {
                            /* The original: += amount capped at 99, 100+
                             * means infinite for (amount-100) ticks, and the
                             * picked weapon is selected. No infinite yet:
                             * it is 99 rounds. */
                            if (amount_ >= 100) {
                                /* ENDLESS, for (amount - 100) ticks. What he
                                 * was carrying is put aside and given back
                                 * when the timer runs out - the original
                                 * saves it at +0x158 and restores it in
                                 * the original's routine. */
                                int k2;
                                if (!inf_w) {
                                    for (k2 = 0; k2 < GTA_WEAPON_COUNT; k2++)
                                        inf_save_ammo[k2] = ammo[k2];
                                    inf_save_w = weapon;
                                }
                                inf_w = kind_;
                                inf_ticks = amount_ - 100;
                                ammo[kind_] = 99;
                                weapon = kind_;
                                /* the original's routine opens with the original's routine(3):
                                 * the frenzy's weapon is announced */
                                gta_audio_speak(&voice, GTA_VOICE_FRENZY, 64, 1);
                                printf("gta: endless weapon %d for %d ticks\n",
                                       inf_w, inf_ticks);
                            } else {
                                int a_ = amount_;
                                if (a_ == 0)
                                    a_ = kind_ == 3 ? 5 : kind_ == 4 ? 10 : 20;
                                ammo[kind_] += a_;
                                if (ammo[kind_] > 99) ammo[kind_] = 99;
                                /* ...and the picked weapon is selected UNLESS
                                 * an endless one is running: the original
                                 * will not take an infinite gun out of your
                                 * hands for twenty pistol rounds. */
                                if (!inf_w)
                                    weapon = kind_;
                            }
                        } else if (kind_ == GTA_PICKUP_BRIBE) {
                            gta_score_clear_heat(&score);
                        } else if (kind_ == GTA_PICKUP_MULTIPLIER) {
                            score.multiplier++;
                        } else if (kind_ == GTA_PICKUP_JAILFREE) {
                            jail_free = 1;
                        } else if (kind_ >= GTA_PICKUP_SPEED && kind_ <= 8) {
                            if (in_car) {
                                /* "car speed!" - the car keeps it. */
                                veh.vmax += veh.vmax / 4;
                                printf("gta: car speed!\n");
                            } else {
                                player_speed = SPEED_TICKS;
                                printf("gta: speed! for %d ticks\n",
                                       player_speed);
                            }
                        } else if (kind_ == GTA_PICKUP_FRENZY) {
                            /* THE KILL FRENZY IS THE SCRIPT'S. Case 0xe of
                             * the original's handler only puts a message on
                             * the brief line; what arms the frenzy is the
                             * script noticing the crate has gone
                             * (`9006 IS_POWERUP_DONE 271 0 9025 200 0`),
                             * which this port already answers. */
                            printf("gta: kill frenzy crate taken - the script "
                                   "has it now\n");
                        } else if (kind_ == GTA_PICKUP_ARMOUR) {
                            player_armour = 3;
                        } else if (kind_ == GTA_PICKUP_LIFE || kind_ == 15) {
                            player_lives++;
                            /* the original's routine case 0xf: [life+] and line 0xb */
                            gta_audio_speak(&voice, GTA_VOICE_EXTRA_LIFE, 64, 1);
                        } else {
                            /* armour, speed, life, kill frenzy: not yet -
                             * nothing reads them. Taken all the same. */
                        }
                        fflush(stdout);
                        }
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
                        if (gta_weapons_fire(&weapons, 1, sx_, sy_, sl_,
                                             sa_, 0, si_))
                            gta_audio_play(&sfx, GTA_SND_PISTOL, 56, 0);
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
                            /* the original's routine opens with the original's routine(0xc) -
                             * the same line as BUSTED */
                            gta_audio_speak(&voice, GTA_VOICE_BUSTED_WASTED, 64, 1);
                            kf_ticks[0] = -1;   /* and clears the clock */
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
                        gta_audio_play(&sfx, GTA_SND_DEATH, 64, 0);
                        for (k = 0; k < ph; k++) {
                            award = gta_score_event(&score,
                                        GTA_SCORE_TYPE_CIVILIAN,
                                        GTA_SCORE_REASON_RUNOVER);
                            /* ONE CRIME, NOT TWO. This used to file the
                             * death as well, 150 of the 151 heat the first
                             * head costs, so two pedestrians - and they
                             * spawn in pairs - were two heads. The original
                             * files kind 3 "Hit 'n' Run", +50, and nothing
                             * else: its kind 8 "Murder One" is reachable
                             * only from a bullet, a fire or a blast
                             * - every caller of the crime report was
                             * checked. Four run-overs are one
                             * head, which is the game the developer
                             * remembers.
                             *
                             * A MOTORCYCLE FILES NOTHING - the original
                             * skips the report when the vehicle is one. */
                            if (tiles.cars[veh.model].vtype != GTA_VEH_BIKE)
                                gta_score_crime(&score, GTA_CRIME_RUNOVER);
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
                text_displays_tick();

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
                    gta_audio_report();
                    gta_script_report(&script);
                    if (!script_triggers_listed) {
                        script_triggers_listed = 1;
                        gta_script_live_triggers(&script,
                                                 in_car ? veh.ox : player.x,
                                                 in_car ? veh.oy : player.y, 25);
                    }
                    printf("gta: crates - %d out, %ld made by the script, "
                           "%ld already there, %ld opened, %ld taken\n",
                           pickups.n, script_crates_made,
                           script_crates_refused, pickups.stat_opened,
                           pickups.stat_taken);
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
                                 "occ %lu, drive %lu, route %lu, spawn %lu, "
                                 "half %ld, still %ld, cruise %ld",
                                 tot / 250UL,
                                 traffic.prof_us[0] / 250UL,
                                 traffic.prof_us[1] / 250UL,
                                 traffic.prof_us[2] / 250UL,
                                 traffic.prof_us[3] / 250UL,
                                 traffic.prof_us[4] / 250UL,
                                 traffic.stat_halfrate / 250L,
                                 traffic.stat_still / 250L,
                                 traffic.stat_cruise / 250L);
                        traffic.stat_halfrate = 0;
                        traffic.stat_still = 0;
                        traffic.stat_cruise = 0;
                        log_line(ln);
                        {
                            /* raw E-clock ticks -> us per tick: x * 1000 /
                             * (freq / 1000), and the cost of the reads
                             * themselves from a hundred back to back */
                            unsigned long kf = amiga_uclock_freq() / 1000UL;
                            unsigned long pd[9], ci, ca, cb;
                            if (kf == 0) kf = 709;
                            for (ci = 0; ci < 9; ci++)
                                pd[ci] = traffic.prof_dn ? traffic.prof_d[ci] * 1000UL / kf / traffic.prof_dn : 0;
                            ca = amiga_uclock_raw();
                            for (ci = 0; ci < 100; ci++)
                                cb = amiga_uclock_raw();
                            snprintf(ln, sizeof ln,
                                     "gta: driveprof early %lu, layer %lu, turn %lu, "
                                     "route %lu, corner %lu, ahead %lu, speed %lu, "
                                     "move %lu (us per drive_one call, %lu sampled; one read in situ %lu us, 100 in a loop %lu us)",
                                     pd[0], pd[1], pd[2], pd[3], pd[4], pd[5], pd[6], pd[7],
                                     traffic.prof_dn, pd[8], (cb - ca) * 1000UL / kf);
                            memset(traffic.prof_d, 0, sizeof traffic.prof_d);
                            traffic.prof_dn = 0;
                        }
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
                /* THE STUCK LOGGER's PICTURE - see gta_ped.blk_x. The first
                 * report for a man who cannot get out of his block comes
                 * with the frame the developer is looking at. */
                if (peds.stuck_report) {
                    peds.stuck_report = 0;
                    dump_frame(GTA_DIR "pedstuck.raw", chunky, pitch,
                               SCREEN_W, SCREEN_H, tiles.palette);
                }
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
        /* THE MISSION CARS ARRIVE WHEN HE DOES. One a frame at most, and
         * each only once - see gta_mcar for why they cannot just be parked
         * at load time. */
        {
            int mc_ = gta_script_due_car(&script,
                            in_car ? veh.ox : player.x,
                            in_car ? veh.oy : player.y, 11);
            if (mc_ >= 0) {
                const gta_mcar *c_ = &script.mcar[mc_];
                unsigned long h_ = gta_traffic_abandon(&traffic, c_->model,
                                        c_->x, c_->y, c_->angle, c_->layer,
                                        -1, 0);
                if (h_) {
                    /* AND THE DECLARATION LEARNS WHICH CAR IT IS. It never
                     * did: every PARKED car went into the fleet nameless,
                     * so SETBOMB 297, IS_GOAL_DEAD 297 and ARROWCAR 297 all
                     * asked about a handle of 0 and got nothing - found by
                     * the speed-bomb test, 2026-09-25. A PARKED_ON that runs
                     * later sees the handle and does not make it twice. */
                    gta_decl *d_ = (gta_decl *)gta_script_by_line(&script,
                                                                  c_->line);
                    if (d_ && !d_->handle)
                        d_->handle = h_;
                    printf("gta: script - mission car (line %d, model %d) "
                           "put down at (%ld,%ld), car %lu\n", c_->line,
                           c_->model, c_->x >> 16, c_->y >> 16, h_);
                } else
                    printf("gta: script - no room for mission car line %d\n",
                           c_->line);
                fflush(stdout);
            }
        }
        gta_pickups_draw(&pickups, &view, 12);
        gta_script_draw(&script, &view, 12);
        cranes_draw(&view);
        red_arrow_draw(&view, in_car ? veh.ox : player.x,
                       in_car ? veh.oy : player.y, player.layer,
                       in_car, veh.len);
        arrow_draw(&view, in_car ? veh.ox : player.x,
                   in_car ? veh.oy : player.y, player.layer,
                   in_car, veh.len);
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
            if (car_shown && pi->sprite_index >= 0) {
                int car_pal = car_remap >= 0 && car_remap < GTA_CAR_REMAPS
                            ? (int)pi->remap8[car_remap] : 0;
                int car_art = (car_ang + GTA_SPRITE_ART_SOUTH) & 255;
                if (use_veh && veh_air) {
                    /* IN THE AIR: the height is the port's own, not something
                     * the renderer can read off a block - there is no block
                     * under it. A grid level is a block tall, so the whole
                     * levels go into `grid` and the remainder into eighths,
                     * and the car grows all the way up the arc and shrinks
                     * all the way down it. */
                    long zp = veh_z >> 16;
                    int gz = player.layer + (int)(zp / GTA_TILE_DIM);
                    int sub = (int)(((zp % GTA_TILE_DIM) * 8) / GTA_TILE_DIM);
                    if (gz >= GTA_GRID_LEVELS - 1) {
                        gz = GTA_GRID_LEVELS - 1;
                        sub = 0;
                    }
                    gta_render_add_sprite_air(&view, car_x, car_y,
                                              GTA_MAP_LAYERS - 1, gz, sub,
                                              pi->sprite_index, car_art,
                                              car_pal, door_now, veh.dmg_bits);
                } else {
                    /* THE RAMP PROBE (opts.txt `rampdbg 1`). The developer
                     * reported a car PULSING up a ramp instead of growing,
                     * and the height it is drawn at is `grid` plus `sub`
                     * eighths - so the fault is in the SEQUENCE of those two
                     * over a few seconds and no screenshot can show a
                     * sequence. One line per change, which is what makes a
                     * sawtooth obvious in a log. */
                    if (opt_rampdbg && in_car) {
                        static int last_sub = -99, last_grid = -99;
                        int sub_now = gta_render_sub_at(&view, car_x, car_y,
                                                        player.layer);
                        if (sub_now != last_sub || player.layer != last_grid) {
                            last_sub = sub_now;
                            last_grid = player.layer;
                            printf("gta: ramp - block (%d,%d) grid %d sub %d "
                                   "slope %d  in-block x %d y %d\n",
                                   (int)(car_x >> 21), (int)(car_y >> 21),
                                   player.layer, sub_now,
                                   gta_render_slope_at(&view, car_x, car_y,
                                                       player.layer),
                                   (int)(car_x >> 16) & 31,
                                   (int)(car_y >> 16) & 31);
                            fflush(stdout);
                        }
                    }
                    gta_render_add_sprite_dm(&view, car_x, car_y, player.layer,
                                          player.layer, pi->sprite_index,
                                          car_art, car_pal, door_now,
                                          use_veh ? veh.dmg_bits : 0);
                }
            }
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
#ifdef __MORPHOS__
            /* MORPHOS SLEEPS THE LEFTOVER INSTEAD OF SPINNING ON IT, and the
             * difference is not a micro-optimisation.
             *
             * A 68020 that has finished a frame early has no spare capacity
             * worth donating, so the busy-wait below costs it nothing. A G4
             * finishes a frame in a fraction of the 16 ms budget and would
             * then burn the whole remainder pinning a core at 100% - Ambient
             * crawls, the fans spin up, and a game that is running perfectly
             * looks like one that has locked the machine. The wait is on
             * timer.device, so the time goes back to the system.
             *
             * Same wrap-safe unsigned subtraction as below. */
            unsigned long spent = (unsigned long)(amiga_uclock_us() - frame_t0);
            if (spent < (unsigned long)FRAME_CAP_US)
                amiga_uclock_sleep_us((unsigned long)FRAME_CAP_US - spent);
#else
            while ((unsigned long)(amiga_uclock_us() - frame_t0)
                       < (unsigned long)FRAME_CAP_US)
                ;
#endif
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

    /* each step logged: ESC once looked like a hang to the developer, and a
     * silent shutdown cannot say which part it stopped in */
    log_line("gta: exit - screen");
    amigagfx_close();
    gta_render_free(&view);
    log_line("gta: exit - audio");
    gta_audio_close();
    gta_sfx_free(&sfx);
    gta_sfx_free(&voice);
    log_line("gta: exit - data");
    gta_map_free(&map);
    gta_tiles_free(&tiles);
    log_line("gta: clean exit");
    /* 5 is WARN to AmigaDOS: the run script's `if warn` restarts the game.
     * A player's ESC returns 0 and the script falls through. */
    return g_reload ? 5 : 0;
}
