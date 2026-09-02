/* Round-trip test for gta_prefs.c, on the host, in a second.
 *
 * The bug this exists to catch: the writer emits multi-word comments and the
 * first reader was word-based, so the file came back as all defaults with no
 * error at all. That took a two-minute emulator round trip to notice. */
#include <stdio.h>
#include <string.h>
#include "../native/gta_prefs.h"

static int fails = 0;

static void check(const char *what, int got, int want)
{
    if (got != want) { printf("FAIL %s: got %d want %d\n", what, got, want); fails++; }
    else             printf("ok   %s = %d\n", what, got);
}

int main(void)
{
    gta_prefs a, b;
    FILE *f;

    remove("gta.prefs");
    remove("backend.txt");

    /* 1. no file at all -> defaults, and load() says it read nothing */
    check("load(missing) returns 0", gta_prefs_load("", &a), 0);
    check("default audio", a.audio, GTA_AUDIO_AUTO);
    check("default gfx",   a.gfx,   GTA_GFX_AUTO);

    /* 2. round trip through the file the writer actually produces, comments
     *    and all - this is the case that was broken */
    a.audio = GTA_AUDIO_AHI;
    a.gfx   = GTA_GFX_WB;
    a.music_vol = 30;
    a.sfx_vol   = 64;
    check("save", gta_prefs_save("", &a), 1);
    check("load returns 1", gta_prefs_load("", &b), 1);
    check("audio survives", b.audio, GTA_AUDIO_AHI);
    check("gfx survives",   b.gfx,   GTA_GFX_WB);
    check("musicvol survives", b.music_vol, 30);
    check("sfxvol survives",   b.sfx_vol,   64);

    /* 3. backend.txt is kept in step, and holds exactly what the game reads */
    f = fopen("backend.txt", "r");
    if (f == NULL) { printf("FAIL backend.txt missing\n"); fails++; }
    else {
        char w[16];
        w[0] = 0;
        if (fscanf(f, "%15s", w) != 1) w[0] = 0;
        fclose(f);
        if (strcmp(w, "wb") != 0) { printf("FAIL backend.txt is \"%s\" not \"wb\"\n", w); fails++; }
        else printf("ok   backend.txt = wb\n");
    }

    /* 4. AUTO must leave the game with no override */
    a.gfx = GTA_GFX_AUTO;
    gta_prefs_save("", &a);
    f = fopen("backend.txt", "r");
    if (f == NULL) { printf("FAIL backend.txt missing after AUTO\n"); fails++; }
    else {
        char w[16];
        int n = fscanf(f, "%15s", w);
        fclose(f);
        if (n == 1) { printf("FAIL backend.txt still says \"%s\" after AUTO\n", w); fails++; }
        else printf("ok   backend.txt empty after AUTO (game reads no override)\n");
    }

    /* 5. hand-edited file: comments, blank lines, odd spacing, mixed case */
    f = fopen("gta.prefs", "w");
    fprintf(f, "# a comment with a great many words in it indeed yes\n");
    fprintf(f, "\n");
    fprintf(f, "   audio   PAULA   \n");
    fprintf(f, "; another comment style\n");
    fprintf(f, "gfx Rtg\n");
    fprintf(f, "nonsense 12\n");
    fprintf(f, "musicvol 200\n");   /* out of range, must clamp */
    fclose(f);
    gta_prefs_load("", &b);
    check("hand-edited audio", b.audio, GTA_AUDIO_PAULA);
    check("hand-edited gfx",   b.gfx,   GTA_GFX_RTG);
    check("musicvol clamped",  b.music_vol, 64);

    /* 6. the word tables */
    check("word auto",  gta_prefs_audio_from_word("auto"),  GTA_AUDIO_AUTO);
    check("word AHI",   gta_prefs_audio_from_word("AHI"),   GTA_AUDIO_AHI);
    check("word junk",  gta_prefs_audio_from_word("banana"), -1);
    check("word wb",    gta_prefs_gfx_from_word("WB"),      GTA_GFX_WB);
    check("word prefix rejected", gta_prefs_gfx_from_word("a"), -1);

    remove("gta.prefs");
    remove("backend.txt");
    printf(fails ? "\n%d FAILURES\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
