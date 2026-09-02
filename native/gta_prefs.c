/* Player settings: read, write, and the word <-> value tables.
 *
 * See gta_prefs.h for the file format and for why this is not opts.txt.
 *
 * NO sprintf ANYWHERE IN THIS FILE. It produces nonsense on this libc -
 * wrong values, shifted arguments, empty %s - and it lies inside your own
 * diagnostics while it does it. snprintf and fprintf are correct. This is
 * toolchain defect 4 in CLAUDE.md and it has cost the sibling ports real
 * days.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gta_prefs.h"

#define PREFS_NAME   "gta.prefs"
/* The game has read this since the first RTG measurement and MorphOS players
 * were publicly told to create it by hand. See gta_prefs_save(). */
#define BACKEND_NAME "backend.txt"

/* One table per setting, so the file parser, the command line and the cycle
 * gadget cannot disagree about what the words are. Order matches the
 * GTA_AUDIO_* / GTA_GFX_* values and the cycle gadget's label array. */
static const char *const audio_words[] = { "auto", "off", "paula", "ahi" };
static const char *const gfx_words[]   = { "auto", "aga",  "rtg",   "wb"  };

/* Shown to a human: capitalised, and "wb" spelled out as what it actually
 * does, because "WB" means nothing to somebody who was told to type it. */
static const char *const audio_names[] = { "Auto", "Off", "Paula", "AHI" };
static const char *const gfx_names[]   = { "Auto", "AGA", "RTG", "Window" };

#define NAUDIO ((int)(sizeof audio_words / sizeof audio_words[0]))
#define NGFX   ((int)(sizeof gfx_words   / sizeof gfx_words[0]))

/* tolower() without <ctype.h>: the ctype tables are one more thing that has to
 * behave identically on three toolchains, for a job that is four lines. */
static int lc(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int word_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lc((unsigned char)*a) != lc((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static int lookup(const char *const *table, int n, const char *word)
{
    int i;
    if (word == NULL) return -1;
    for (i = 0; i < n; i++)
        if (word_eq(table[i], word)) return i;
    return -1;
}

int gta_prefs_audio_from_word(const char *word)
{
    return lookup(audio_words, NAUDIO, word);
}

int gta_prefs_gfx_from_word(const char *word)
{
    return lookup(gfx_words, NGFX, word);
}

const char *gta_prefs_audio_name(int audio)
{
    return (audio >= 0 && audio < NAUDIO) ? audio_names[audio] : "?";
}

const char *gta_prefs_gfx_name(int gfx)
{
    return (gfx >= 0 && gfx < NGFX) ? gfx_names[gfx] : "?";
}

void gta_prefs_defaults(gta_prefs *p)
{
    if (p == NULL) return;
    p->audio     = GTA_AUDIO_AUTO;
    p->gfx       = GTA_GFX_AUTO;
    p->music_vol = 48;   /* music sits under the effects, as in the original */
    p->sfx_vol   = 64;   /* Paula's maximum */
}

static void clamp_vol(int *v)
{
    if (*v < 0)  *v = 0;
    if (*v > 64) *v = 64;
}

/* One path from a directory prefix and a name. The prefix is used verbatim,
 * so "PROGDIR:" works and so does "" - see the header.
 *
 * Concatenated by hand rather than with snprintf, which is C99: this file is
 * built with -std=c89 -pedantic on the host (see tools/bin/build_host.sh) so
 * that it cannot pick up anything bebbo's 68k GCC would reject later, and it
 * is not worth an exception to that for joining two strings. */
static void build_path(char *dst, int cap, const char *dir, const char *name)
{
    int n = 0, i;
    if (dir == NULL) dir = "";
    for (i = 0; dir[i] != 0 && n < cap - 1; i++)  dst[n++] = dir[i];
    for (i = 0; name[i] != 0 && n < cap - 1; i++) dst[n++] = name[i];
    dst[n] = 0;
}

int gta_prefs_load(const char *dir, gta_prefs *p)
{
    char path[256];
    char line[256];
    char key[32], val[32];
    FILE *f;

    if (p == NULL) return 0;
    gta_prefs_defaults(p);

    build_path(path, (int)sizeof path, dir, PREFS_NAME);
    f = fopen(path, "r");
    if (f == NULL) return 0;

    /* LINE BY LINE, not `while (fscanf(f, "%s %s"))`.
     *
     * It was written the second way first, to match the opts.txt loop the game
     * already has, and it was wrong the moment this file grew a comment.
     * fscanf("%s") does not know what a line is: it just takes the next word.
     * So a four-word comment line does not cost one pair, it shifts every
     * following pair out of phase, and the file this very function writes was
     * then read back as all defaults - with no error, because every misaligned
     * pair is simply an unrecognised key.
     *
     * Reading a line at a time makes a comment cost exactly one line, which is
     * what a comment is. opts.txt gets away with the other form only because
     * nothing ever put a comment in it. */
    while (fgets(line, (int)sizeof line, f) != NULL) {
        int v;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';' || *s == '\n' || *s == '\r' || *s == 0)
            continue;
        if (sscanf(s, "%31s %31s", key, val) != 2) continue;
        if (word_eq(key, "audio")) {
            v = gta_prefs_audio_from_word(val);
            if (v >= 0) p->audio = v;
        } else if (word_eq(key, "gfx")) {
            v = gta_prefs_gfx_from_word(val);
            if (v >= 0) p->gfx = v;
        } else if (word_eq(key, "musicvol")) {
            p->music_vol = (int)strtol(val, NULL, 10);
        } else if (word_eq(key, "sfxvol")) {
            p->sfx_vol = (int)strtol(val, NULL, 10);
        }
    }
    fclose(f);

    clamp_vol(&p->music_vol);
    clamp_vol(&p->sfx_vol);
    return 1;
}

int gta_prefs_save(const char *dir, const gta_prefs *p)
{
    char path[256];
    FILE *f;
    gta_prefs q;

    if (p == NULL) return 0;
    q = *p;
    clamp_vol(&q.music_vol);
    clamp_vol(&q.sfx_vol);
    if (q.audio < 0 || q.audio >= NAUDIO) q.audio = GTA_AUDIO_AUTO;
    if (q.gfx   < 0 || q.gfx   >= NGFX)   q.gfx   = GTA_GFX_AUTO;

    build_path(path, (int)sizeof path, dir, PREFS_NAME);
    f = fopen(path, "w");
    if (f == NULL) return 0;

    fprintf(f, "# AmiGTA settings - written by gtaprefs.\n");
    fprintf(f, "# Editing this by hand is fine; the words are the ones the\n");
    fprintf(f, "# editor shows. audio: auto off paula ahi.  gfx: auto aga\n");
    fprintf(f, "# rtg wb.  Volumes are 0 to 64.\n");
    fprintf(f, "audio %s\n", audio_words[q.audio]);
    fprintf(f, "gfx %s\n",   gfx_words[q.gfx]);
    fprintf(f, "musicvol %d\n", q.music_vol);
    fprintf(f, "sfxvol %d\n",   q.sfx_vol);
    fclose(f);

    /* AND KEEP backend.txt IN STEP - see the header for why this is here and
     * not left to the player.
     *
     * Deleting it for AUTO is done by writing nothing rather than by remove():
     * an empty backend.txt reads as "no word found" in the game's fscanf and
     * therefore means exactly the same as no file, and it avoids depending on
     * remove() behaving the same on three C libraries for a file that may be
     * open, absent, or on a read-only volume. */
    build_path(path, (int)sizeof path, dir, BACKEND_NAME);
    f = fopen(path, "w");
    if (f != NULL) {
        if (q.gfx == GTA_GFX_AGA)      fprintf(f, "aga\n");
        else if (q.gfx == GTA_GFX_RTG) fprintf(f, "rtg\n");
        else if (q.gfx == GTA_GFX_WB)  fprintf(f, "wb\n");
        /* GTA_GFX_AUTO: empty file, which the game reads as no override. */
        fclose(f);
    }
    return 1;
}
