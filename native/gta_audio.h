/* THE SOUND PLAYER - Phase 6, the half that makes a noise.
 *
 * `gta_sfx.c` is the DATA layer and knows nothing about the machine: it hands
 * out a pointer into a megabyte of signed 8-bit samples and a Paula period.
 * This is the other half - the one that owns audio.device, the four hardware
 * channels and the Chip RAM, and it is Amiga-only for that reason. Nothing
 * here is portable and nothing here parses anything.
 *
 * WHY THERE IS A CACHE AND NOT JUST A POINTER
 * -------------------------------------------
 * Paula's DMA can only read CHIP RAM, and the bank is 1 044 432 bytes - most
 * of what an A1200 has in Chip altogether. PLAN.md calls this "the open
 * constraint, and it is real", and it is the only real design decision in the
 * whole of phase 6. The bank itself is loaded wherever malloc puts it, which
 * on any machine worth targeting is Fast RAM, and a sound is copied into a
 * small Chip cache the first time it is played.
 *
 * The numbers say the cache can be small. The 131 sounds of level001 run from
 * 60 to 38 524 bytes and average about 8 KB, and a scene uses a handful of
 * them over and over - an engine, a skid, a gunshot, a door. GTA_AUDIO_CHIP
 * of budget holds a dozen or so of those, and a sound that falls out of the
 * cache costs one memcpy the next time it is heard, not a stall.
 *
 * WHAT A CHANNEL IS FOR
 * ---------------------
 * Paula routes channels 0 and 3 to the LEFT jack and 1 and 2 to the RIGHT,
 * and PLAN.md reserves 2 and 3 for streamed music. So effects prefer 0 and 1 -
 * one ear each - and use 2 and 3 only while nothing is streaming, which is
 * also what amiga_audio.c enforces on its own side.
 *
 * A NEW SOUND NEVER WAITS. When every usable channel is busy the oldest one
 * is stopped and reused, because in a game the sound that has just happened
 * matters more than the one that is finishing. PLAN.md asks for a fast
 * fade-out on the stolen channel rather than a hard stop; that is not written
 * yet - see the note on gta_audio_play().
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_AUDIO_H
#define GTA_AUDIO_H

#include "gta_sfx.h"

/* How much Chip RAM the effect cache may hold. Twelve to sixteen of the
 * bank's sounds, which is more than any one scene uses. */
#define GTA_AUDIO_CHIP   (128L * 1024)
/* How many sounds may be resident at once, whatever their size. */
#define GTA_AUDIO_SLOTS  24

/* Open audio.device and take the four channels. `bank` must outlive the
 * player - nothing is copied out of it except into the Chip cache. Returns 1
 * when sound is available, 0 when it is not; every call below is safe and
 * silent afterwards either way, so a caller never has to test again.
 *
 * `enabled` is the player's own choice out of gta.prefs (GTA_AUDIO_OFF means
 * do not even open the device). */
int  gta_audio_open(const gta_sfx *bank, int enabled);
void gta_audio_close(void);
int  gta_audio_available(void);

/* Play bank sound `n` at `volume` 0..64. `pan` is -1 left, +1 right, 0 either.
 * Returns the channel it went to, or -1 when it could not be played at all
 * (no such sound, no Chip RAM, audio not open).
 *
 * STEALING: when nothing is idle the channel that started longest ago is
 * stopped and taken. The original's own rule - and PLAN.md's - is to fade
 * that channel out over a few milliseconds first, which needs either a volume
 * ramp on a timer or a tail written into the buffer; neither is here yet, so
 * a steal is currently a hard cut. It is audible only when four effects
 * overlap. */
int  gta_audio_play(const gta_sfx *bank, int n, int volume, int pan);
/* The same at a rate of the caller's - the car sound loop plays several of
 * its samples at a rate it computes (the skid: 8000 + 150 x the slide).
 * Clamped to Paula's range. */
int  gta_audio_play_hz(const gta_sfx *bank, int n, int volume, long hz);

/* SAY entry `n` of the speech bank (vocalcom, opened with
 * gta_sfx_open_index() - its samples are read off the disk here, one line
 * at a time, into a single Chip buffer). ONE VOICE AT A TIME, as the
 * original, which has two kinds of caller:
 *   cut 0 - the positional ones (the cops, the street, the frenzy's cheer)
 *           ask the original's routine whether a line is still playing and do nothing
 *           if it is: the new line is DROPPED;
 *   cut 1 - the original's routine, the announcer, asks nothing: the line playing is
 *           CUT and the new one said.
 * Returns the channel, or -1 (dropped, audio off, no such line). Every
 * request is counted even with audio off, like gta_audio_play(). */
int  gta_audio_speak(const gta_sfx *voice, int n, int volume, int cut);
/* Is a line still being said? The callers' own guard, for the ones that
 * must not even draw their random number while one is (the original's routine and
 * friends advance their countdowns only when the voice is free). */
int  gta_audio_speaking(void);

/* Once a frame. Reaps finished channels - which is what makes them idle
 * again - and times them, so the log can show that a sound really occupied
 * the hardware for its own duration rather than merely being started. */
void gta_audio_tick(void);

/* The five-second report: how many sounds were played, stolen and cached, and
 * how much Chip RAM the cache is holding. */
void gta_audio_report(void);

/* For the acceptance test: how long the last finished sound actually ran, in
 * milliseconds, and how long it should have. 0 when nothing has finished. */
void gta_audio_last_duration(int *sound, long *ran_ms, long *want_ms);

/* ---- THE RADIO ----------------------------------------------------------
 *
 * A TRACK COMES IN TWO SHAPES and the file says which by its first twelve
 * bytes, not by its name:
 *
 *   IFF 8SVX (`radioN.8svx`) - signed 8-bit mono at 22050 Hz, which is what
 *     Paula's DMA reads. There is no decoder: the refill is an fread into
 *     the Chip buffer the hardware is about to play. This is what the port's
 *     own first-run extractor writes (native/gta_iff.c) and it is what an
 *     Amiga can make for itself.
 *   IMA-ADPCM WAV (`radioN.mus`) - mono, 22050 Hz, blockAlign 1024, made by
 *     `tools/gtamusic.c` on a PC. Half the disk, a table lookup and three
 *     shifts per sample to play.
 *
 * A player who has both gets the 8SVX. Either way it is STREAMED - a
 * three-minute track is 4 MB raw and there is no question of loading one -
 * through the double-buffered pair of Chip buffers amiga_audio.c keeps on
 * channels 2 and 3, refilled once a frame from the main loop and never from
 * an interrupt.
 *
 * While a track plays, effects confine themselves to channels 0 and 1; that
 * is enforced on the amiga_audio.c side, so nothing here has to remember it.
 *
 * `loop` restarts the track at its end instead of stopping. Returns 1 when it
 * started. */
int  gta_audio_music_start(const char *path, int loop);
void gta_audio_music_stop(void);
/* Once a frame, beside gta_audio_tick(). Cheap when nothing needs refilling. */
void gta_audio_music_tick(void);
int  gta_audio_music_playing(void);
void gta_audio_music_volume(int vol);

/* ---- THE CAR RADIO ------------------------------------------------------
 *
 * GTA 1 plays music when you are in a car and stops when you get out, which
 * is the "radio" phase 6's acceptance test names. Which STATION a given car
 * carries is not in the notes and was not found in the the original, so this
 * port takes the next track in turn instead - stated here rather than
 * pretended: it is a stand-in for a table nobody has read yet.
 *
 * `gta_audio_radio_scan()` looks once for GTADATA/radio1.8svx, then
 * radio1.mus, and so on upwards, and remembers how many there are; with
 * none, every call below does nothing and the car is quiet. It must be
 * called AFTER the first-run extraction, or the run that just made the
 * stations is the one run that does not find them. */
void gta_audio_radio_scan(const char *dir);
int  gta_audio_radio_count(void);
/* Next station, looping. Returns 1 when one started. */
int  gta_audio_radio_next(void);

/* THE POLICE BAND, and it is not a station: a POLICE CAR carries it and
 * nothing else does. The first version had no such distinction and the
 * numbering put the band on the ordinary car radio - the developer heard
 * dispatch chatter in a saloon and reported it, which is right: the original
 * never did that. `police.8svx` (or `.mus`); with none, a police car falls
 * back to a music station. */
int  gta_audio_radio_police(void);

/* ---- THE ENGINE NOTE ----------------------------------------------------
 *
 * One sample of the bank, looped in the hardware on a reserved channel, with
 * its rate following the car's speed. THE ORIGINAL HAS IT - PROGRESS 168
 * said it did not, and was wrong (205): the car sound loop the original's routine
 * plays sample 0x2d + the car's `engine` byte (gta_car_info.engine) for a
 * car whose engine runs, at a rate taken from the speed by the car's
 * `sound_function`. The caller works the rate out (gta_main.c,
 * engine_rate()); this only plays it.
 *
 * Starting an engine that is already running on that sound does nothing.
 * `hz` is the ORIGINAL'S playback rate for the whole sample, up to 77000
 * (the loop keeps a third of the samples so Paula can reach it); `volume`
 * 0..64. */
int  gta_audio_engine_start(const gta_sfx *bank, int n);
void gta_audio_engine_stop(void);
void gta_audio_engine_rate(long hz, int volume);
int  gta_audio_engine_playing(void);

#endif /* GTA_AUDIO_H */
