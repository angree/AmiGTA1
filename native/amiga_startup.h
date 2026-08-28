/* Startup-time display choice for the Amiga OpenXcom port.
 *
 * Free of Amiga headers on purpose, like amiga_gfx.h: the C++ side may include
 * this, and must never see <proto/intuition.h>.
 */
#ifndef AMIGA_STARTUP_H
#define AMIGA_STARTUP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Values match AMIGAGFX_BACKEND_* in amiga_gfx.h. */
#define AMIGA_STARTUP_AGA  0
#define AMIGA_STARTUP_RTG  1
#define AMIGA_STARTUP_QUIT (-1)

/* Put an Intuition requester on the Workbench screen asking which display to
 * use, and return the choice. rtg_available 0 removes the RTG button, so a
 * plain AGA machine is never offered a mode it cannot open.
 *
 * This is what the "ask" build of the port calls before opening anything; the
 * AGA and RTG builds never call it. It runs before any screen of ours exists,
 * which is exactly why it is a system requester rather than something drawn
 * into our own display. */
int amigastartup_ask_backend(int rtg_available);

/* Put a one-button Intuition requester on screen. Used for the errors that
 * would otherwise leave the player with a program that simply exited: a
 * missing data folder, a failed screen, a crash. */
void amigastartup_error(const char *text);

/* 20/30/40/60, or 0 for a plain 68000. */
int amigastartup_cpu(void);

/* Decide which display this run uses and tell SDLmini, before anything opens a
 * screen. Called from main() - the one line of Amiga-specific code in the
 * game's own source.
 *
 * Order of precedence: a command-line switch (-aga, -rtg, -ask), then the
 * build's compiled-in default (AMIGA_BACKEND_DEFAULT: 0 AGA, 1 RTG, -1 ask),
 * then, if that says ask, the Intuition requester. Quitting from the requester
 * exits the program then and there. */
void amiga_select_backend(int argc, char **argv);

/* The port's log line (implemented in sdlmini_core.c). Writes to
 * PROGDIR:sdlmini.log immediately and to the game log once one exists.
 * Declared here so the few markers in main.cpp can use it without the game
 * seeing any other part of SDLmini's internals. */
void SDLmini_Log(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* AMIGA_STARTUP_H */
