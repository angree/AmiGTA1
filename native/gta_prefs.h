/* Player settings, shared by the game and by the external prefs editor.
 *
 * NO AMIGA HEADERS, plain C89, stdio only - exactly like gta_render.c and for
 * the same reason: the same source has to build for the 68k game, for the
 * PowerPC one, and on the host where it can be tested in a second instead of a
 * two-minute emulator round trip.
 *
 * WHY THERE IS A SETTINGS FILE AT ALL, when there is already opts.txt.
 *
 * opts.txt is the A/B switch board for the test rig - `traffic 0`, `fleet 12`,
 * `benchframes 5`. It is a developer's file, a shipped archive has none, and
 * deploy.sh appends to it. Player settings are a different thing with a
 * different lifetime, so they get their own file and their own editor rather
 * than being mixed into the debug switches and clobbered by the next deploy.
 *
 * The file is `gta.prefs` beside the executable (PROGDIR:), one `key value`
 * per line, values are WORDS and not numbers so it can be fixed with a text
 * editor on a machine whose mouse does not work - which is not hypothetical:
 * the whole MorphOS investigation was run without a usable pointer.
 *
 *     # AmiGTA settings
 *     audio ahi
 *     gfx   wb
 *     musicvol 48
 *     sfxvol   64
 */
#ifndef GTA_PREFS_H
#define GTA_PREFS_H

/* WHICH AUDIO PATH. This is the setting the whole editor exists for.
 *
 * Paula means banging the chip registers through audio.device: four channels,
 * DMA, no mixer, which is what the 68k platform layer inherited from
 * openttd_amiga_68k. It is the fastest thing on real AGA hardware and it is
 * NOT AVAILABLE ANYWHERE ELSE - on MorphOS there is no Paula behind
 * audio.device and touching it is reported to hang the machine.
 *
 * AHI is the Amiga sound API everything since about 1997 speaks: sound cards,
 * MorphOS, AmigaOS 4, and 68k machines with Delfina/Prelude/Melody/Toccata.
 * It costs CPU that Paula does not, because it mixes in software on a plain
 * 020, which is exactly why this is a choice and not a detection.
 *
 * AUTO is the shipped default: use Paula where Paula exists, AHI where it does
 * not. A player only has to touch this when the automatic answer is wrong for
 * their machine - an 020 with a sound card that wants AHI, or an AGA machine
 * where something else already holds the audio channels. */
#define GTA_AUDIO_AUTO  0
#define GTA_AUDIO_OFF   1
#define GTA_AUDIO_PAULA 2
#define GTA_AUDIO_AHI   3

/* How many of each there are. The editor builds its cycle-gadget label arrays
 * from gta_prefs_*_name() and needs the count, so the words in the file, the
 * words on the command line and the words in the gadget all come from one
 * table instead of three that drift apart. */
#define GTA_AUDIO_COUNT 4

/* WHICH DISPLAY PATH, the same four the game already understands.
 *
 * AUTO leaves the binary's own compiled-in default alone (gta-aga is AGA,
 * gta-rtg240/480 are RTG), which is what every existing archive does.
 *
 * The other three are the answer to a machine where the automatic choice is
 * wrong. WB - the game in a window on the Workbench screen - is the one that
 * makes the port work on MorphOS, and until now the only way to select it was
 * to create a file called backend.txt by hand containing the two letters
 * `wb`. That is a workaround printed in a README, which is the same shape of
 * fault as the Work: paths were. */
#define GTA_GFX_AUTO 0
#define GTA_GFX_AGA  1
#define GTA_GFX_RTG  2
#define GTA_GFX_WB   3

#define GTA_GFX_COUNT 4

typedef struct {
    int audio;      /* GTA_AUDIO_*  */
    int gfx;        /* GTA_GFX_*    */
    int music_vol;  /* 0..64, Paula's own hardware scale; AHI is scaled to it */
    int sfx_vol;    /* 0..64 */
} gta_prefs;

/* The shipped state: automatic everything, full volume. */
void gta_prefs_defaults(gta_prefs *p);

/* Read <dir>gta.prefs. Missing file is not an error - it fills in the
 * defaults and returns 0, because no settings and default settings are the
 * same thing. Returns 1 if a file was read, 0 if not.
 *
 * `dir` is prefixed to the name verbatim, so it must already end in a
 * separator: "PROGDIR:" on the Amiga, "" or "./" on the host. */
int gta_prefs_load(const char *dir, gta_prefs *p);

/* Write <dir>gta.prefs. Returns 1 on success, 0 if the file could not be
 * written (a read-only volume, most likely, which is worth telling the
 * player about rather than failing silently).
 *
 * IT ALSO WRITES backend.txt, and that is deliberate rather than tidy.
 * The game reads backend.txt as an override, and it is what MorphOS players
 * were publicly told to create by hand. If the editor left a stale
 * backend.txt in place, the game would keep using it and the editor would be
 * showing a setting that has no effect - a lie with a support cost. So the
 * two are kept in step: a chosen backend writes it, GTA_GFX_AUTO deletes it. */
int gta_prefs_save(const char *dir, const gta_prefs *p);

/* "Auto", "Off", "Paula", "AHI" / "Auto", "AGA", "RTG", "Window" - for the
 * editor's cycle gadgets and for the game's own log line. Never NULL. */
const char *gta_prefs_audio_name(int audio);
const char *gta_prefs_gfx_name(int gfx);

/* Parse one word as a setting value; -1 if it is not one of them. Shared so
 * the file parser and the editor's command line agree by construction rather
 * than by two lists that drift. Case-insensitive. */
int gta_prefs_audio_from_word(const char *word);
int gta_prefs_gfx_from_word(const char *word);

#endif /* GTA_PREFS_H */
