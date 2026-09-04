/* gtaprefs - the settings editor for AmiGTA. An Intuition window, and a
 * command line for the machines where there is no usable pointer.
 *
 *     gtaprefs                       open the window
 *     gtaprefs SHOW                  print the settings and what this
 *                                    machine has, then exit
 *     gtaprefs AUDIO=AHI GFX=WB      set those and save, no window
 *     gtaprefs SCREEN=320x240        the size the game opens at
 *     gtaprefs ?                     usage
 *
 * WHY THIS IS A SEPARATE PROGRAM and not a menu inside the game.
 *
 * The settings it edits are the ones that decide whether the game can open
 * anything at all. A player whose machine comes up as colour noise - which is
 * what MorphOS did - cannot reach an in-game menu to fix it, because the menu
 * would be drawn by the display that is broken. The choice has to be made
 * BEFORE the game opens a screen, so it has to be made somewhere else.
 *
 * Until now "somewhere else" meant creating a file called backend.txt by hand
 * containing the two letters `wb`, which is a workaround printed in a README
 * rather than a program. This is the program.
 *
 * IT ALSO WORKS WITHOUT A MOUSE, on purpose. Every gadget has a key (A, G, R,
 * S, Esc) and the whole thing can be done from the command line instead. That is
 * not thoroughness for its own sake: the entire MorphOS investigation was run
 * on a machine whose pointer did not work, and a settings editor that needs a
 * pointer would have been useless in exactly the case it exists for.
 *
 * GadTools rather than hand-drawn gadgets, and a window on the Workbench
 * screen rather than a screen of its own: it has to look native, it has to
 * work on a 640x256 PAL Workbench and on a 1920x1080 MorphOS one, and it must
 * never do the one thing that is under suspicion here - open a display.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <intuition/intuition.h>
#include <intuition/gadgetclass.h>
#include <libraries/gadtools.h>
#include <graphics/gfxbase.h>
#include <graphics/text.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/gadtools.h>

#include <stdio.h>
#include <string.h>

#include "gta_prefs.h"

/* __attribute__((used)) or -O1 drops it and `version gtaprefs` finds
 * nothing - the string is never read by any code, which is the whole
 * point of a version cookie. */
static const char verstag[] __attribute__((used)) =
    "$VER: gtaprefs 0.1.0 (04.09.2026)";

/* The settings live beside the executable, exactly like the game's own data.
 * PROGDIR: is the drawer this binary was loaded from and AmigaOS sets it for
 * a Workbench double-click as well as for a shell, which is the whole reason
 * v0.0.2 stopped hard-coding Work: - see LEFTOFF.md. */
#define GTA_DIR "PROGDIR:"

struct Library *GadToolsBase = NULL;

/* ------------------------------------------------------------------------ */
/* WHAT THIS MACHINE ACTUALLY HAS.                                          */
/*                                                                          */
/* All three probes are READ-ONLY. None of them opens audio.device, allocates*/
/* a channel or opens a screen, because this program runs when the player is */
/* already in trouble and it must not be able to make the trouble worse. The */
/* results are shown as information next to the choice, never used to remove */
/* a choice: detection here is a hint, and the player's own machine is a      */
/* better authority than a heuristic.                                        */
/* ------------------------------------------------------------------------ */

/* AGA: the Alice revision bit in GfxBase. This is what tells an AGA Amiga
 * apart from a machine that only has a graphics card - and on the latter,
 * Paula does not exist either, which is exactly the case this program is for. */
static int have_aga(void)
{
    struct GfxBase *g = (struct GfxBase *)GfxBase;
    if (g == NULL) return 0;
    return (g->ChipRevBits0 & GFXF_AA_ALICE) ? 1 : 0;
}

/* RTG: cybergraphics.library, opened at ANY version and closed again.
 * Any version because asking for v41 is how the game failed to find it on
 * MorphOS - see the three failed attempts in tools/morphos/README.md. */
static int have_rtg(void)
{
    struct Library *b = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 0L);
    if (b == NULL) return 0;
    CloseLibrary(b);
    return 1;
}

/* AHI: is ahi.device there?
 *
 * Deliberately NOT by OpenDevice. Opening AHI takes a unit and can disturb
 * whatever is already playing, and the AHI developer headers are not part of
 * the NDK, so the request structure would have to be guessed - a guessed
 * structure handed to a device is how you write off a machine, not how you
 * probe one.
 *
 * So: the exec device list first, which finds it when something has already
 * loaded it (MorphOS keeps it resident), and DEVS: second, which finds it on
 * a 68k machine where it is installed but not yet loaded. */
static int have_ahi(void)
{
    struct Node *n;
    BPTR l;

    Forbid();
    n = FindName(&SysBase->DeviceList, (STRPTR)"ahi.device");
    Permit();
    if (n != NULL) return 1;

    l = Lock((CONST_STRPTR)"DEVS:ahi.device", ACCESS_READ);
    if (l != 0) { UnLock(l); return 1; }
    return 0;
}

/* One line describing the machine, built once and shown in the window and by
 * SHOW. snprintf, never sprintf - see CLAUDE.md defect 4. */
static void machine_line(char *dst, int cap)
{
    snprintf(dst, (size_t)cap,
             "This machine:  AGA %s   RTG %s   AHI %s",
             have_aga() ? "yes" : "no",
             have_rtg() ? "yes" : "no",
             have_ahi() ? "yes" : "no");
    dst[cap - 1] = 0;
}

/* ------------------------------------------------------------------------ */
/* The one-line explanation under each choice. This is where a player finds  */
/* out that Paula is not a thing on their machine, which is the single most  */
/* useful sentence in the program.                                          */
/* ------------------------------------------------------------------------ */

static const char *audio_hint(int a)
{
    switch (a) {
    case GTA_AUDIO_OFF:   return "No sound at all.";
    case GTA_AUDIO_PAULA: return "Amiga chipset only - not on MorphOS or OS4.";
    case GTA_AUDIO_AHI:   return "Sound cards, MorphOS, OS4. Costs some CPU.";
    default:              return "Paula where there is a chipset, AHI where not.";
    }
}

static const char *gfx_hint(int g)
{
    switch (g) {
    case GTA_GFX_AGA: return "Planar screen and c2p. Real Amiga chipset only.";
    case GTA_GFX_RTG: return "CyberGraphX 8-bit screen. Needs a graphics card.";
    case GTA_GFX_WB:  return "A window on Workbench. Slowest, works anywhere.";
    default:          return "An AGA screen. Try this first.";
    }
}

/* THE SIZE THE GAME OPENS AT - three separate binaries until v0.0.4.
 *
 * gta-aga, gta-rtg240 and gta-rtg480 were one program built with three sets
 * of -D flags, so a player picked their screen size by picking a program and
 * had no way of knowing which one their machine wanted. It is a setting now
 * and this is where it is set. */
static const char *screen_hint(int s)
{
    switch (s) {
    case GTA_SCR_320200: return "The reference size. Every speed figure.";
    case GTA_SCR_320240: return "More of the city. Wants an RTG screen.";
    case GTA_SCR_640480:
        return "Really drawn at 640x480. Sharp, and four times the work.";
    case GTA_SCR_640480X2:
        return "320x240 doubled. Fills the screen, costs almost nothing.";
    default:             return "320x200, or 320x240 if graphics is RTG.";
    }
}

/* ------------------------------------------------------------------------ */
/* THE WINDOW                                                               */
/* ------------------------------------------------------------------------ */

enum { GID_AUDIO = 1, GID_AUDIOHINT, GID_GFX, GID_GFXHINT,
       GID_SCREEN, GID_SCREENHINT,
       GID_MACHINE, GID_KEYS, GID_SAVE, GID_CANCEL };

/* THE KEYS GO IN THE WINDOW, NOT IN THE TITLE BAR.
 *
 * They were in the title first, and the first screenshot showed why that was
 * wrong: the window is sized from its contents, the title is longer than the
 * contents, and Intuition simply clips it - the line ended at "S save" and the
 * way out of the program was the part that got cut off. A window that must
 * work for somebody with no pointer cannot hide the keyboard instructions in
 * the one piece of text it does not control the width of. */
#define KEYS_LINE \
    "Keys:  A audio   G graphics   R screen   S save   Esc cancel"

/* Text width in pixels on the screen's own font. Measured rather than assumed
 * as "characters times 8": the Workbench font is the player's choice and can
 * be proportional, and a window sized for topaz.8 that is opened under a 15
 * pixel font has its labels running out of the frame. */
static int text_w(struct Screen *scr, const char *s)
{
    return (int)TextLength(&scr->RastPort, (CONST_STRPTR)s, (ULONG)strlen(s));
}

static int max_hint_w(struct Screen *scr)
{
    int i, w = 0, t;
    for (i = 0; i < GTA_AUDIO_COUNT; i++) {
        t = text_w(scr, audio_hint(i));
        if (t > w) w = t;
    }
    for (i = 0; i < GTA_GFX_COUNT; i++) {
        t = text_w(scr, gfx_hint(i));
        if (t > w) w = t;
    }
    for (i = 0; i < GTA_SCR_COUNT; i++) {
        t = text_w(scr, screen_hint(i));
        if (t > w) w = t;
    }
    return w;
}

/* Returns 1 if the player saved, 0 if they cancelled, -1 if the window could
 * not be opened at all (the caller then falls back to the command line, which
 * is better than exiting with nothing said). */
static int run_window(gta_prefs *p)
{
    struct Screen *scr;
    APTR vi = NULL;
    struct Gadget *glist = NULL, *gad, *g_audio = NULL, *g_gfx = NULL;
    struct Gadget *g_screen = NULL;
    struct Gadget *g_ahint = NULL, *g_ghint = NULL, *g_shint = NULL;
    struct Window *win = NULL;
    struct NewGadget ng;
    STRPTR audio_labels[GTA_AUDIO_COUNT + 1];
    STRPTR gfx_labels[GTA_GFX_COUNT + 1];
    STRPTR screen_labels[GTA_SCR_COUNT + 1];
    char machine[96];
    int i, cw, fh, gh, lm, gap, labw, gadw, hintw, innerw, innerh;
    int leftb, topb, x, y, btnw;
    int result = -1, done = 0;

    scr = LockPubScreen(NULL);
    if (scr == NULL) return -1;

    vi = GetVisualInfo(scr, TAG_END);
    if (vi == NULL) { UnlockPubScreen(NULL, scr); return -1; }

    /* One source of truth for the words: the same table the file parser and
     * the command line use, read back through gta_prefs_*_name(). */
    for (i = 0; i < GTA_AUDIO_COUNT; i++)
        audio_labels[i] = (STRPTR)gta_prefs_audio_name(i);
    audio_labels[GTA_AUDIO_COUNT] = NULL;
    for (i = 0; i < GTA_GFX_COUNT; i++)
        gfx_labels[i] = (STRPTR)gta_prefs_gfx_name(i);
    gfx_labels[GTA_GFX_COUNT] = NULL;
    for (i = 0; i < GTA_SCR_COUNT; i++)
        screen_labels[i] = (STRPTR)gta_prefs_screen_name(i);
    screen_labels[GTA_SCR_COUNT] = NULL;

    machine_line(machine, (int)sizeof machine);

    /* Everything is measured off the screen's font, so the window is the
     * right size under topaz.8 on a PAL Workbench and under a 15 pixel font
     * on a 1920x1080 MorphOS one. */
    cw = scr->RastPort.TxWidth;   if (cw < 6) cw = 6;
    fh = scr->RastPort.TxHeight;  if (fh < 8) fh = 8;
    gh  = fh + 6;                 /* a GadTools gadget is the font plus frame */
    lm  = cw * 2;
    gap = fh / 2;  if (gap < 4) gap = 4;

    labw = text_w(scr, "Graphics:") + cw;
    /* The cycle gadget must hold the longest word AND the arrow box, which
     * GadTools draws inside the gadget on the left. */
    gadw = text_w(scr, "Window") + cw * 2 + 24;
    for (i = 0; i < GTA_AUDIO_COUNT; i++) {
        int t = text_w(scr, (const char *)audio_labels[i]) + cw * 2 + 24;
        if (t > gadw) gadw = t;
    }
    /* "640x480 (x2)" is the longest label in the window, so it decides the
     * gadget width - which is why it is measured rather than assumed. */
    for (i = 0; i < GTA_SCR_COUNT; i++) {
        int t = text_w(scr, (const char *)screen_labels[i]) + cw * 2 + 24;
        if (t > gadw) gadw = t;
    }
    hintw = max_hint_w(scr);

    innerw = lm + labw + gadw + lm;
    if (lm + hintw + lm > innerw)                innerw = lm + hintw + lm;
    if (lm + text_w(scr, machine) + lm > innerw) innerw = lm + text_w(scr, machine) + lm;
    if (lm + text_w(scr, KEYS_LINE) + lm > innerw)
        innerw = lm + text_w(scr, KEYS_LINE) + lm;

    leftb = scr->WBorLeft;
    topb  = scr->WBorTop + scr->Font->ta_YSize + 1;

    if (CreateContext(&glist) == NULL) {
        FreeVisualInfo(vi); UnlockPubScreen(NULL, scr); return -1;
    }

    memset(&ng, 0, sizeof ng);
    ng.ng_TextAttr   = scr->Font;
    ng.ng_VisualInfo = vi;

    y = gap;
    x = lm + labw;

    /* --- audio ---------------------------------------------------------- */
    ng.ng_LeftEdge   = leftb + x;
    ng.ng_TopEdge    = topb + y;
    ng.ng_Width      = gadw;
    ng.ng_Height     = gh;
    ng.ng_GadgetText = (STRPTR)"Audio:";
    ng.ng_GadgetID   = GID_AUDIO;
    ng.ng_Flags      = PLACETEXT_LEFT;
    gad = CreateGadget(CYCLE_KIND, glist, &ng,
                       GTCY_Labels, (ULONG)audio_labels,
                       GTCY_Active, (ULONG)p->audio,
                       TAG_END);
    g_audio = gad;
    y += gh + 2;

    ng.ng_LeftEdge   = leftb + lm;
    ng.ng_TopEdge    = topb + y;
    ng.ng_Width      = innerw - lm * 2;
    ng.ng_Height     = fh;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID   = GID_AUDIOHINT;
    ng.ng_Flags      = 0;
    gad = CreateGadget(TEXT_KIND, gad, &ng,
                       GTTX_Text, (ULONG)audio_hint(p->audio),
                       TAG_END);
    g_ahint = gad;
    y += fh + gap;

    /* --- graphics ------------------------------------------------------- */
    ng.ng_LeftEdge   = leftb + x;
    ng.ng_TopEdge    = topb + y;
    ng.ng_Width      = gadw;
    ng.ng_Height     = gh;
    ng.ng_GadgetText = (STRPTR)"Graphics:";
    ng.ng_GadgetID   = GID_GFX;
    ng.ng_Flags      = PLACETEXT_LEFT;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
                       GTCY_Labels, (ULONG)gfx_labels,
                       GTCY_Active, (ULONG)p->gfx,
                       TAG_END);
    g_gfx = gad;
    y += gh + 2;

    ng.ng_LeftEdge   = leftb + lm;
    ng.ng_TopEdge    = topb + y;
    ng.ng_Width      = innerw - lm * 2;
    ng.ng_Height     = fh;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID   = GID_GFXHINT;
    ng.ng_Flags      = 0;
    gad = CreateGadget(TEXT_KIND, gad, &ng,
                       GTTX_Text, (ULONG)gfx_hint(p->gfx),
                       TAG_END);
    g_ghint = gad;
    y += fh + gap;

    /* --- screen size ----------------------------------------------------- */
    ng.ng_LeftEdge   = leftb + x;
    ng.ng_TopEdge    = topb + y;
    ng.ng_Width      = gadw;
    ng.ng_Height     = gh;
    ng.ng_GadgetText = (STRPTR)"Screen:";
    ng.ng_GadgetID   = GID_SCREEN;
    ng.ng_Flags      = PLACETEXT_LEFT;
    gad = CreateGadget(CYCLE_KIND, gad, &ng,
                       GTCY_Labels, (ULONG)screen_labels,
                       GTCY_Active, (ULONG)p->screen,
                       TAG_END);
    g_screen = gad;
    y += gh + 2;

    ng.ng_LeftEdge   = leftb + lm;
    ng.ng_TopEdge    = topb + y;
    ng.ng_Width      = innerw - lm * 2;
    ng.ng_Height     = fh;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID   = GID_SCREENHINT;
    ng.ng_Flags      = 0;
    gad = CreateGadget(TEXT_KIND, gad, &ng,
                       GTTX_Text, (ULONG)screen_hint(p->screen),
                       TAG_END);
    g_shint = gad;
    y += fh + gap + gap;

    /* --- what the machine has ------------------------------------------- */
    ng.ng_LeftEdge   = leftb + lm;
    ng.ng_TopEdge    = topb + y;
    ng.ng_Width      = innerw - lm * 2;
    ng.ng_Height     = fh;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID   = GID_MACHINE;
    ng.ng_Flags      = 0;
    gad = CreateGadget(TEXT_KIND, gad, &ng, GTTX_Text, (ULONG)machine, TAG_END);
    y += fh + 2;

    ng.ng_TopEdge  = topb + y;
    ng.ng_GadgetID = GID_KEYS;
    gad = CreateGadget(TEXT_KIND, gad, &ng, GTTX_Text, (ULONG)KEYS_LINE, TAG_END);
    y += fh + gap + gap;

    /* --- save / cancel --------------------------------------------------- */
    btnw = text_w(scr, "Cancel") + cw * 4;
    if (btnw < cw * 10) btnw = cw * 10;

    ng.ng_LeftEdge   = leftb + lm;
    ng.ng_TopEdge    = topb + y;
    ng.ng_Width      = btnw;
    ng.ng_Height     = gh;
    ng.ng_GadgetText = (STRPTR)"Save";
    ng.ng_GadgetID   = GID_SAVE;
    ng.ng_Flags      = PLACETEXT_IN;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);

    ng.ng_LeftEdge   = leftb + innerw - lm - btnw;
    ng.ng_GadgetText = (STRPTR)"Cancel";
    ng.ng_GadgetID   = GID_CANCEL;
    gad = CreateGadget(BUTTON_KIND, gad, &ng, TAG_END);
    y += gh + gap;

    innerh = y;

    if (gad == NULL) {   /* any CreateGadget failing leaves NULL from there on */
        FreeGadgets(glist); FreeVisualInfo(vi); UnlockPubScreen(NULL, scr);
        return -1;
    }

    win = OpenWindowTags(NULL,
        WA_Title,       (ULONG)"AmiGTA Settings",
        WA_InnerWidth,  (ULONG)innerw,
        WA_InnerHeight, (ULONG)innerh,
        WA_Left,        (ULONG)(scr->Width  > innerw ? (scr->Width - innerw) / 2 : 0),
        WA_Top,         (ULONG)(scr->Height > innerh ? (scr->Height - innerh) / 3 : 0),
        WA_DragBar,     TRUE,
        WA_DepthGadget, TRUE,
        WA_CloseGadget, TRUE,
        WA_Activate,    TRUE,
        WA_SmartRefresh, TRUE,
        WA_PubScreen,   (ULONG)scr,
        WA_Gadgets,     (ULONG)glist,
        WA_IDCMP,       IDCMP_CLOSEWINDOW | IDCMP_GADGETUP |
                        IDCMP_REFRESHWINDOW | IDCMP_VANILLAKEY,
        TAG_END);

    if (win == NULL) {
        FreeGadgets(glist); FreeVisualInfo(vi); UnlockPubScreen(NULL, scr);
        return -1;
    }

    GT_RefreshWindow(win, NULL);

    while (!done) {
        struct IntuiMessage *msg;
        WaitPort(win->UserPort);
        while ((msg = GT_GetIMsg(win->UserPort)) != NULL) {
            ULONG cls  = msg->Class;
            UWORD code = msg->Code;
            struct Gadget *src = (struct Gadget *)msg->IAddress;
            GT_ReplyIMsg(msg);

            switch (cls) {
            case IDCMP_CLOSEWINDOW:
                result = 0; done = 1;
                break;

            case IDCMP_REFRESHWINDOW:
                GT_BeginRefresh(win);
                GT_EndRefresh(win, TRUE);
                break;

            case IDCMP_GADGETUP:
                switch (src->GadgetID) {
                case GID_AUDIO:
                    p->audio = (int)code;
                    GT_SetGadgetAttrs(g_ahint, win, NULL,
                                      GTTX_Text, (ULONG)audio_hint(p->audio),
                                      TAG_END);
                    break;
                case GID_GFX:
                    p->gfx = (int)code;
                    GT_SetGadgetAttrs(g_ghint, win, NULL,
                                      GTTX_Text, (ULONG)gfx_hint(p->gfx),
                                      TAG_END);
                    break;
                case GID_SCREEN:
                    p->screen = (int)code;
                    GT_SetGadgetAttrs(g_shint, win, NULL,
                                      GTTX_Text, (ULONG)screen_hint(p->screen),
                                      TAG_END);
                    break;
                case GID_SAVE:   result = 1; done = 1; break;
                case GID_CANCEL: result = 0; done = 1; break;
                default: break;
                }
                break;

            /* THE KEYBOARD PATH. Not a convenience: this program's whole
             * reason to exist is machines where the display or the pointer is
             * not behaving, and one that could only be driven by mouse would
             * be unusable in precisely those cases. */
            case IDCMP_VANILLAKEY:
                switch (code) {
                case 'a': case 'A':
                    p->audio = (p->audio + 1) % GTA_AUDIO_COUNT;
                    GT_SetGadgetAttrs(g_audio, win, NULL,
                                      GTCY_Active, (ULONG)p->audio, TAG_END);
                    GT_SetGadgetAttrs(g_ahint, win, NULL,
                                      GTTX_Text, (ULONG)audio_hint(p->audio),
                                      TAG_END);
                    break;
                case 'g': case 'G':
                    p->gfx = (p->gfx + 1) % GTA_GFX_COUNT;
                    GT_SetGadgetAttrs(g_gfx, win, NULL,
                                      GTCY_Active, (ULONG)p->gfx, TAG_END);
                    GT_SetGadgetAttrs(g_ghint, win, NULL,
                                      GTTX_Text, (ULONG)gfx_hint(p->gfx),
                                      TAG_END);
                    break;
                case 'r': case 'R':
                    p->screen = (p->screen + 1) % GTA_SCR_COUNT;
                    GT_SetGadgetAttrs(g_screen, win, NULL,
                                      GTCY_Active, (ULONG)p->screen, TAG_END);
                    GT_SetGadgetAttrs(g_shint, win, NULL,
                                      GTTX_Text, (ULONG)screen_hint(p->screen),
                                      TAG_END);
                    break;
                case 's': case 'S': case 13:
                    result = 1; done = 1; break;
                case 'c': case 'C': case 27:
                    result = 0; done = 1; break;
                default: break;
                }
                break;

            default:
                break;
            }
        }
    }

    CloseWindow(win);
    FreeGadgets(glist);
    FreeVisualInfo(vi);
    UnlockPubScreen(NULL, scr);
    return result;
}

/* ------------------------------------------------------------------------ */
/* THE COMMAND LINE                                                         */
/* ------------------------------------------------------------------------ */

static void usage(void)
{
    printf("gtaprefs - settings editor for AmiGTA\n\n");
    printf("  gtaprefs                  open the window\n");
    printf("  gtaprefs SHOW             print the settings and this machine\n");
    printf("  gtaprefs AUDIO=<word>     auto | off | paula | ahi\n");
    printf("  gtaprefs GFX=<word>       auto | aga | rtg | wb\n");
    printf("  gtaprefs SCREEN=<word>    auto | 320x200 | 320x240 |\n");
    printf("                            640x480 | 640x480x2\n\n");
    printf("Giving any of those saves straight away and opens no window,\n");
    printf("which is how the settings are changed on a machine whose pointer\n");
    printf("does not work. Settings are written to " GTA_DIR "gta.prefs\n");
    printf("(and backend.txt is kept in step with GFX).\n");
}

static void show(const gta_prefs *p)
{
    char machine[96];
    machine_line(machine, (int)sizeof machine);
    printf("%s\n", machine);
    printf("audio %s   - %s\n",
           gta_prefs_audio_name(p->audio), audio_hint(p->audio));
    printf("gfx   %s   - %s\n",
           gta_prefs_gfx_name(p->gfx), gfx_hint(p->gfx));
    {
        int w = 0, h = 0, x2 = 0;
        gta_prefs_screen_size(p->screen, p->gfx, &w, &h, &x2);
        printf("screen %s   - %s\n",
               gta_prefs_screen_name(p->screen), screen_hint(p->screen));
        printf("       the game will open %dx%d%s\n",
               w, h, x2 ? " and render a quarter of it" : "");
    }
    printf("musicvol %d   sfxvol %d\n", p->music_vol, p->sfx_vol);
}

/* KEY=VALUE, hand-parsed rather than through ReadArgs.
 *
 * ReadArgs is the idiom and it would give `?` help for free, but it must not
 * be called when there is no CLI - and this program is meant to be
 * double-clicked. Twenty lines here removes a conditional that would only
 * ever be exercised on one of the two launch paths. */
static int split_kv(char *arg, char **key, char **val)
{
    char *eq = strchr(arg, '=');
    if (eq == NULL) return 0;
    *eq = 0;
    *key = arg;
    *val = eq + 1;
    return 1;
}

static int eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

int main(int argc, char **argv)
{
    gta_prefs p;
    int i, changed = 0, do_show = 0, r;

    gta_prefs_load(GTA_DIR, &p);

    /* argc is 0 when Workbench started us - argv is then the WBStartup
     * message, not a string array, and must not be walked. */
    for (i = 1; i < argc; i++) {
        char *key, *val;
        if (argv[i][0] == '?' || eq_ci(argv[i], "HELP")) { usage(); return 0; }
        if (eq_ci(argv[i], "SHOW")) { do_show = 1; continue; }
        if (split_kv(argv[i], &key, &val)) {
            int v;
            if (eq_ci(key, "AUDIO")) {
                v = gta_prefs_audio_from_word(val);
                if (v < 0) { printf("gtaprefs: AUDIO must be auto, off, "
                                    "paula or ahi\n"); return 20; }
                p.audio = v; changed = 1; continue;
            }
            if (eq_ci(key, "GFX")) {
                v = gta_prefs_gfx_from_word(val);
                if (v < 0) { printf("gtaprefs: GFX must be auto, aga, rtg "
                                    "or wb\n"); return 20; }
                p.gfx = v; changed = 1; continue;
            }
            if (eq_ci(key, "SCREEN")) {
                v = gta_prefs_screen_from_word(val);
                if (v < 0) { printf("gtaprefs: SCREEN must be auto, 320x200, "
                                    "320x240, 640x480 or 640x480x2\n"); return 20; }
                p.screen = v; changed = 1; continue;
            }
        }
        printf("gtaprefs: do not understand \"%s\"\n\n", argv[i]);
        usage();
        return 20;
    }

    if (changed) {
        if (!gta_prefs_save(GTA_DIR, &p)) {
            printf("gtaprefs: COULD NOT WRITE " GTA_DIR "gta.prefs - is the "
                   "drawer write protected?\n");
            return 20;
        }
        show(&p);
        printf("saved to " GTA_DIR "gta.prefs\n");
        return 0;
    }
    if (do_show) { show(&p); return 0; }

    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37L);
    if (GadToolsBase == NULL) {
        printf("gtaprefs: no gadtools.library v37 - no window here.\n"
               "Use the command line instead:\n\n");
        usage();
        return 20;
    }

    r = run_window(&p);
    CloseLibrary(GadToolsBase);
    GadToolsBase = NULL;

    if (r < 0) {
        /* Not a failure worth being silent about: the player asked for a
         * window and did not get one, so say what they can do instead. */
        printf("gtaprefs: could not open the window (screen too small, or no\n"
               "public screen). Use the command line instead:\n\n");
        usage();
        return 20;
    }
    if (r == 1) {
        if (!gta_prefs_save(GTA_DIR, &p)) {
            printf("gtaprefs: COULD NOT WRITE " GTA_DIR "gta.prefs\n");
            return 20;
        }
        show(&p);
        printf("saved to " GTA_DIR "gta.prefs\n");
    }
    return 0;
}
