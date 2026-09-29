/* The sound player - see gta_audio.h.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <string.h>

#include "gta_audio.h"
#include "gta_prefs.h"
#include "amiga_audio.h"
#include "amiga_uclock.h"
#include "amiga_adpcm.h"
#include "gta_iff.h"        /* GTA_IFF_RATE - what an 8SVX defaults to */

#define PAULA_CLOCK_PAL 3546895UL   /* period = clock / rate */

/* One resident sound: a Chip copy of a bank entry, and when it was last
 * wanted. `playing` counts channels currently reading it, so the eviction
 * cannot pull a buffer out from under Paula's DMA - which would not be a
 * glitch, it would be a machine reading freed memory at 22 kHz. */
typedef struct {
    int  sound;
    void *chip;
    unsigned long bytes;
    unsigned long used;         /* the play counter at the last use */
    int  playing;
} gta_chip_slot;

static const gta_sfx *au_bank;
static int  au_ok;
static gta_chip_slot au_slot[GTA_AUDIO_SLOTS];
static int  au_n_slots;
static long au_chip_held;
static unsigned long au_clock;          /* every play increments it */

/* Per channel: which slot it is reading, when it started, and how long the
 * sample it was given should take. */
static int  ch_slot[AMIGA_AUDIO_CHANNELS];
static unsigned long ch_start[AMIGA_AUDIO_CHANNELS];
static long ch_want_ms[AMIGA_AUDIO_CHANNELS];
static int  ch_sound[AMIGA_AUDIO_CHANNELS];
static unsigned long ch_seq[AMIGA_AUDIO_CHANNELS];

static long au_n_played, au_n_stolen, au_n_copied, au_n_evicted, au_n_failed;
/* EVERY REQUEST, BY SOUND ID - counted whether or not audio is open, so a run
 * with `audio 0` (the tests are muted, the developer works beside them)
 * still proves which sounds the game asked for. PROGRESS 194. */
#define AU_REQ_IDS 128
static unsigned short au_req[AU_REQ_IDS];

/* THE ENGINE NOTE OWNS ONE CHANNEL while it runs, and the effect picker has
 * to know that before it is defined - a note stolen mid-corner is worse than
 * no note. The engine itself is at the bottom of this file. */
#define GTA_ENGINE_CH 1
static int en_on;

/* The radio's own clock, at the bottom of this file: it has to be advanced
 * from both tick functions, and they are defined above it. */
static void radio_clock(void);
static int  au_last_sound = -1;
static long au_last_ran, au_last_want;
/* The first few plays are logged in full. After that only the report speaks,
 * or a gunfight would fill the log with one line per shot. */
static int  au_verbose = 8;

/* THE VOICE - see gta_audio_speak(). One Chip buffer, grown to the longest
 * line said so far (62 KB at most: vocalcom's entry 3) and kept, and the
 * channel the current line is on, -1 when none is. */
static void *sp_chip;
static unsigned long sp_cap;
static int  sp_ch = -1;
static long sp_n_said, sp_n_busy, sp_n_cut, sp_n_failed;
#define AU_VOICE_IDS 80
static unsigned short au_vreq[AU_VOICE_IDS];

static void slot_free(int i)
{
    if (au_slot[i].chip) {
        AmigaAudio_FreeSample(au_slot[i].chip);
        au_chip_held -= (long)au_slot[i].bytes;
    }
    au_slot[i].chip = NULL;
    au_slot[i].sound = -1;
    au_slot[i].bytes = 0;
    au_slot[i].playing = 0;
}

int gta_audio_open(const gta_sfx *bank, int enabled)
{
    int i;

    au_bank = bank;
    au_ok = 0;
    au_n_slots = 0;
    au_chip_held = 0;
    au_clock = 0;
    for (i = 0; i < GTA_AUDIO_SLOTS; i++) {
        au_slot[i].chip = NULL;
        au_slot[i].sound = -1;
    }
    for (i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
        ch_slot[i] = -1;
        ch_sound[i] = -1;
    }

    if (enabled == GTA_AUDIO_OFF) {
        printf("gta: audio OFF by the player's own setting\n");
        fflush(stdout);
        return 0;
    }
#ifndef __MORPHOS__
    /* AHI is not written. GTA_AUDIO_AHI is honoured by staying silent rather
     * than by quietly using Paula instead: a machine where the player picked
     * AHI is usually one where Paula is not reachable at all (MorphOS), and
     * banging audio.device there is reported to hang it. */
    if (enabled == GTA_AUDIO_AHI) {
        printf("gta: audio - AHI was chosen and is not written yet; silent\n");
        fflush(stdout);
        return 0;
    }
#else
    /* ON MORPHOS THIS IS THE OTHER WAY ROUND, and the paragraph above says why
     * without meaning to: the machine that cannot reach Paula is this one. AHI
     * IS written here - native/morphos_audio.c implements the whole
     * amiga_audio.h contract over it, and amiga_audio.c is not built at all -
     * so AHI is not a reason to stay silent, it is the only way to make a
     * sound. Every setting except OFF therefore goes on to open it, PAULA
     * included: a drawer shared with an Amiga install will say PAULA, and the
     * player asking for sound should get sound rather than a lecture. */
    if (enabled == GTA_AUDIO_PAULA)
        printf("gta: audio - prefs say Paula; there is none on PowerPC,"
               " using AHI\n");
#endif
    if (!bank || bank->count <= 0 || !bank->data) {
        printf("gta: audio - no sound bank loaded; silent\n");
        fflush(stdout);
        return 0;
    }
    if (!AmigaAudio_Open()) {
#ifdef __MORPHOS__
        printf("gta: audio - AHI would not open; silent\n");
#else
        printf("gta: audio - audio.device would not open; silent\n");
#endif
        fflush(stdout);
        return 0;
    }
    au_ok = 1;
#ifdef __MORPHOS__
    printf("gta: audio ON - AHI, 4 channels, %d sounds, cache %ld KB\n",
           bank->count, (long)(GTA_AUDIO_CHIP / 1024));
#else
    printf("gta: audio ON - Paula, 4 channels, %d sounds, cache %ld KB\n",
           bank->count, (long)(GTA_AUDIO_CHIP / 1024));
#endif
    /* The bank's own pointers and one entry, so that a play which later
     * reads nonsense out of the same table can be told apart from one that
     * never had a table: if these are sane here and garbage there, something
     * between the two wrote over it. */
    printf("gta: audio - bank at %p, entry %p, data %p; sound 33 = %lu bytes "
           "at %lu, period %u, %u Hz\n",
           (void *)bank, (void *)bank->entry, (void *)bank->data,
           bank->entry[33].length, bank->entry[33].offset,
           bank->entry[33].period, bank->entry[33].rate);
    fflush(stdout);
    return 1;
}

void gta_audio_close(void)
{
    int i;
    if (au_ok) {
        AmigaAudio_Close();
        for (i = 0; i < GTA_AUDIO_SLOTS; i++)
            slot_free(i);
        if (sp_chip) AmigaAudio_FreeSample(sp_chip);
    }
    sp_chip = NULL;
    sp_cap = 0;
    sp_ch = -1;
    au_ok = 0;
    au_n_slots = 0;
    au_chip_held = 0;
}

int gta_audio_available(void) { return au_ok; }

/* The Chip copy of sound `n`, made if it is not there. -1 when it cannot be
 * had - no such sound, or Chip RAM exhausted even after evicting. */
static int slot_for(const gta_sfx *bank, int n, unsigned long *len,
                    int *period)
{
    const signed char *src;
    unsigned long bytes;
    int i, victim;

    /* THE TABLE IS READ HERE, NOT THROUGH gta_sfx_sample().
     *
     * That call returned nonsense on the Amiga - measured, not suspected:
     *
     *   audio PROBE - sample(33) -> src 0xa19f8d7, len 117967112,
     *                 period 1539; table says 9898 bytes at 355672
     *
     * All three of its outputs are sample DATA (0x07080808, 0x0603), which
     * means the `sfx` pointer arriving inside the function was not the one
     * passed in - while a direct read of `bank->entry[n]` on the very next
     * line, in this same function, gives the right answer. Whatever that is -
     * and it is worth finding, see the note in LEFTOFF.md - it is not a
     * reason for the audio to stay silent: the struct is public, the fields
     * are the ones the header documents, and reading them here is three
     * lines instead of a call.
     *
     * The guards are the ones gta_sfx_sample() applies, kept deliberately:
     * a bad index and a zero-length hole in the table are both normal. */
    if (!bank->data || n < 0 || n >= bank->count) {
        if (au_verbose > 0) {
            printf("gta: audio - sound %d is outside the bank (%d entries)\n",
                   n, bank ? bank->count : -1);
            fflush(stdout);
        }
        return -1;
    }
    bytes = bank->entry[n].length;
    if (period) *period = (int)bank->entry[n].period;
    src = bank->data + bank->entry[n].offset;
    if (bytes < 2) {
        if (au_verbose > 0) {
            printf("gta: audio - sound %d is a hole in the table\n", n);
            fflush(stdout);
        }
        return -1;
    }
    *len = bytes;

    for (i = 0; i < au_n_slots; i++)
        if (au_slot[i].sound == n && au_slot[i].chip) {
            au_slot[i].used = au_clock;
            return i;
        }

    /* Room has to be made BEFORE the allocation, not after a failure: a
     * cache that only evicts when AllocVec says no is a cache that fills
     * Chip RAM and then leaves the machine with none for anything else. */
    while ((au_chip_held + (long)bytes > GTA_AUDIO_CHIP
            || au_n_slots >= GTA_AUDIO_SLOTS)) {
        unsigned long oldest = 0;
        victim = -1;
        for (i = 0; i < au_n_slots; i++) {
            if (!au_slot[i].chip || au_slot[i].playing)
                continue;
            if (victim < 0 || au_slot[i].used < oldest) {
                oldest = au_slot[i].used;
                victim = i;
            }
        }
        if (victim < 0)
            break;              /* everything resident is being played */
        slot_free(victim);
        au_n_evicted++;
        /* Compact: the freed slot is reused below rather than left as a
         * hole, so `au_n_slots` stays the count of live entries. */
        if (victim != au_n_slots - 1)
            au_slot[victim] = au_slot[au_n_slots - 1];
        au_n_slots--;
    }

    if (au_n_slots >= GTA_AUDIO_SLOTS)
        return -1;
    i = au_n_slots;
    au_slot[i].chip = AmigaAudio_AllocSample(bytes);
    if (!au_slot[i].chip) {
        if (au_verbose > 0)
            printf("gta: audio - no Chip RAM for %lu bytes (holding %ld of "
                   "%ld); bank %p entry %p data %p count %d; the table says "
                   "sound %d is %lu bytes at %lu, %u Hz\n",
                   bytes, au_chip_held, (long)GTA_AUDIO_CHIP,
                   (void *)bank, (void *)bank->entry, (void *)bank->data,
                   bank->count, n, bank->entry[n].length,
                   bank->entry[n].offset, bank->entry[n].rate);
        return -1;
    }
    memcpy(au_slot[i].chip, src, (size_t)bytes);

    /* WHAT IS ACTUALLY IN THE BUFFER PAULA WILL READ, for the first few
     * sounds of a session.
     *
     * Every other proof this port has about sound is about LENGTH: a channel
     * that really held the hardware was busy for the sample's own duration.
     * That measurement passes just as well when the pointer is wrong - a
     * channel fed zeroed Chip RAM is silence that runs for exactly the right
     * time. So this reads the copy back through the Chip pointer itself and
     * says what is there. Signal is full-range with a mean near zero; a flat
     * line of zeros or of one repeated byte is the fault, and it is then a
     * fault INSIDE the guest rather than in the emulator's audio or the
     * speakers.
     *
     * `au_n_copied` limits it to the first four, so it costs nothing after
     * the opening seconds. */
    if (au_n_copied < 4) {
        const signed char *q = (const signed char *)au_slot[i].chip;
        long k, mn = 127, mx = -128, nz = 0;
        unsigned long lim = bytes < 4096UL ? bytes : 4096UL;
        for (k = 0; k < (long)lim; k++) {
            if (q[k] < mn) mn = q[k];
            if (q[k] > mx) mx = q[k];
            if (q[k]) nz++;
        }
        printf("gta: audio - CHIP COPY of sound %d at %p: %ld of %lu bytes "
               "non-zero, min %ld max %ld, first 8: %d %d %d %d %d %d %d %d\n",
               n, au_slot[i].chip, nz, lim, mn, mx,
               q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
        fflush(stdout);
    }

    au_slot[i].sound = n;
    au_slot[i].bytes = bytes;
    au_slot[i].used = au_clock;
    au_slot[i].playing = 0;
    au_chip_held += (long)bytes;
    au_n_slots++;
    au_n_copied++;
    return i;
}

/* Which channel to use. Idle first, in the order the pan asks for; then the
 * one that started longest ago. Music holds 2 and 3 when it is running and
 * AmigaAudio_Play refuses those on its own, so they are tried last. */
static int pick_channel(int pan)
{
    static const int left[4]  = { 0, 3, 1, 2 };
    static const int right[4] = { 1, 2, 0, 3 };
    static const int any[4]   = { 0, 1, 3, 2 };
    const int *order = pan < 0 ? left : (pan > 0 ? right : any);
    int k, best = -1;
    unsigned long oldest = 0;

    /* THE MUSIC'S CHANNELS ARE NOT OURS while it plays. ChannelIdle() says
     * 2 and 3 are idle - the stream has requests of its own - so they used
     * to be picked, AmigaAudio_Play() refused them, and the sound was LOST
     * instead of taking channel 0 from whatever was on it: 51 of 2749 in the
     * developer's own half hour (215). They are skipped in both passes. */
    int mus = AmigaAudio_MusicActive();

    for (k = 0; k < AMIGA_AUDIO_CHANNELS; k++) {
        if (en_on && order[k] == GTA_ENGINE_CH)
            continue;                   /* reserved: see the note on en_on */
        if (mus && (order[k] == 2 || order[k] == 3))
            continue;
        if (AmigaAudio_ChannelIdle(order[k]))
            return order[k];
    }

    for (k = 0; k < AMIGA_AUDIO_CHANNELS; k++) {
        int c = order[k];
        if (en_on && c == GTA_ENGINE_CH)
            continue;
        if (mus && (c == 2 || c == 3))
            continue;
        if (best < 0 || ch_seq[c] < oldest) { oldest = ch_seq[c]; best = c; }
    }
    return best;
}

static void channel_done(int c)
{
    if (ch_slot[c] >= 0) {
        if (au_slot[ch_slot[c]].playing > 0)
            au_slot[ch_slot[c]].playing--;
        ch_slot[c] = -1;
    }
}

static int play_at(const gta_sfx *bank, int n, int volume, int pan, long hz);

int gta_audio_play(const gta_sfx *bank, int n, int volume, int pan)
{
    return play_at(bank, n, volume, pan, 0L);
}

int gta_audio_play_hz(const gta_sfx *bank, int n, int volume, long hz)
{
    return play_at(bank, n, volume, 0, hz);
}

/* `hz` 0 plays the sound at its own rate; otherwise at that rate, clamped
 * to what Paula can do. */
static int play_at(const gta_sfx *bank, int n, int volume, int pan, long hz)
{
    unsigned long bytes = 0;
    int period = 0, slot, c;

    if (n >= 0 && n < AU_REQ_IDS && au_req[n] < 65535U)
        au_req[n]++;
    if (!au_ok || !bank)
        return -1;
    slot = slot_for(bank, n, &bytes, &period);
    if (slot < 0) { au_n_failed++; return -1; }
    if (hz > 0) {
        long p = (long)(PAULA_CLOCK_PAL / (unsigned long)hz);
        period = (int)(p < 124 ? 124 : (p > 65535L ? 65535L : p));
    }

    c = pick_channel(pan);
    if (c < 0) { au_n_failed++; return -1; }

    if (!AmigaAudio_ChannelIdle(c)) {
        /* STOLEN. Hard cut for now - see gta_audio_play() in the header. */
        AmigaAudio_Stop(c);
        channel_done(c);
        au_n_stolen++;
    } else {
        channel_done(c);
    }
    if (c == sp_ch)
        sp_ch = -1;             /* the line that was on it is cut */

    if (!AmigaAudio_Play(c, au_slot[slot].chip, bytes, period, volume)) {
        au_n_failed++;
        return -1;
    }

    au_clock++;
    au_slot[slot].used = au_clock;
    au_slot[slot].playing++;
    ch_slot[c] = slot;
    ch_sound[c] = n;
    ch_seq[c] = au_clock;
    ch_start[c] = amiga_uclock_us();
    /* How long it OUGHT to take, from the rate the file carries. This is the
     * acceptance test for phase 6's first step: a channel that really has the
     * hardware occupied stays busy for the sample's own duration, and one
     * that was merely started and dropped does not. */
    {
        long rate = hz > 0 ? hz : (long)bank->entry[n].rate;
        ch_want_ms[c] = rate > 0 ? (long)(bytes * 1000UL / (unsigned long)rate)
                                 : 0;
    }
    au_n_played++;

    if (au_verbose > 0) {
        au_verbose--;
        printf("gta: audio - sound %d on ch%d, %lu bytes, period %d (%d Hz), "
               "vol %d, should run %ld ms\n",
               n, c, bytes, period, bank->entry[n].rate, volume,
               ch_want_ms[c]);
        fflush(stdout);
    }
    return c;
}

int gta_audio_speaking(void)
{
    if (sp_ch < 0)
        return 0;
    if (au_ok && !AmigaAudio_ChannelIdle(sp_ch))
        return 1;
    sp_ch = -1;
    return 0;
}

int gta_audio_speak(const gta_sfx *voice, int n, int volume, int cut)
{
    unsigned long bytes, got;
    int c;

    if (n >= 0 && n < AU_VOICE_IDS && au_vreq[n] < 65535U)
        au_vreq[n]++;
    if (!au_ok || !voice || !voice->file || n < 0 || n >= voice->count)
        return -1;
    if (gta_audio_speaking()) {
        if (!cut) { sp_n_busy++; return -1; }
        /* THE STOP COMPLETES BEFORE THE BUFFER IS WRITTEN - see the engine's
         * stop: Paula must not be reading what the disk read overwrites. */
        AmigaAudio_Stop(sp_ch);
        sp_ch = -1;
        sp_n_cut++;
    }
    bytes = voice->entry[n].length;
    if (bytes < 2) { sp_n_failed++; return -1; }

    if (bytes > sp_cap) {
        if (sp_chip) AmigaAudio_FreeSample(sp_chip);
        sp_cap = 0;
        sp_chip = AmigaAudio_AllocSample(bytes);
        if (!sp_chip) { sp_n_failed++; return -1; }
        sp_cap = bytes;
    }
    /* Read straight into Chip: the line is played once, so a Fast copy
     * first would only double the bytes moved. */
    got = gta_sfx_read(voice, n, (signed char *)sp_chip, sp_cap);
    if (got != bytes) { sp_n_failed++; return -1; }

    c = pick_channel(0);
    if (c < 0) { sp_n_failed++; return -1; }
    if (!AmigaAudio_ChannelIdle(c)) {
        AmigaAudio_Stop(c);
        au_n_stolen++;
    }
    channel_done(c);
    if (!AmigaAudio_Play(c, sp_chip, bytes, (int)voice->entry[n].period,
                         volume)) {
        sp_n_failed++;
        return -1;
    }
    au_clock++;
    ch_sound[c] = -1;
    ch_seq[c] = au_clock;
    sp_ch = c;
    sp_n_said++;
    if (au_verbose > 0) {
        au_verbose--;
        printf("gta: audio - SAID line %d on ch%d, %lu bytes, %u Hz\n",
               n, c, bytes, voice->entry[n].rate);
        fflush(stdout);
    }
    return c;
}

void gta_audio_tick(void)
{
    int c;
    if (!au_ok)
        return;
    radio_clock();
    for (c = 0; c < AMIGA_AUDIO_CHANNELS; c++) {
        if (ch_slot[c] < 0)
            continue;
        if (!AmigaAudio_ChannelIdle(c))
            continue;
        au_last_sound = ch_sound[c];
        au_last_ran = (long)((amiga_uclock_us() - ch_start[c]) / 1000UL);
        au_last_want = ch_want_ms[c];
        if (au_verbose >= 0 && au_n_played <= 8) {
            printf("gta: audio - sound %d on ch%d finished: ran %ld ms, "
                   "expected %ld ms\n", ch_sound[c], c, au_last_ran,
                   au_last_want);
            fflush(stdout);
        }
        channel_done(c);
    }
}

void gta_audio_last_duration(int *sound, long *ran_ms, long *want_ms)
{
    if (sound) *sound = au_last_sound;
    if (ran_ms) *ran_ms = au_last_ran;
    if (want_ms) *want_ms = au_last_want;
}

void gta_audio_report(void)
{
    {
        int i, any = 0;
        for (i = 0; i < AU_REQ_IDS; i++)
            if (au_req[i]) {
                if (!any) printf("gta: audio - asked for (id x count):");
                printf(" %d x%u", i, (unsigned)au_req[i]);
                any = 1;
            }
        if (any) { printf("\n"); fflush(stdout); }
        any = 0;
        for (i = 0; i < AU_VOICE_IDS; i++)
            if (au_vreq[i]) {
                if (!any) printf("gta: audio - voice asked for (line x count):");
                printf(" %d x%u", i, (unsigned)au_vreq[i]);
                any = 1;
            }
        if (any) { printf("\n"); fflush(stdout); }
    }
    if (!au_ok)
        return;
    if (sp_n_said || sp_n_busy || sp_n_failed)
        printf("gta: audio - voice: %ld said, %ld dropped busy, %ld cut short, "
               "%ld failed; buffer %lu bytes\n", sp_n_said, sp_n_busy, sp_n_cut,
               sp_n_failed, sp_cap);
    printf("gta: audio - %ld played, %ld stolen, %ld failed; cache %d sounds, "
           "%ld KB of %ld, %ld copied in, %ld thrown out\n",
           au_n_played, au_n_stolen, au_n_failed, au_n_slots,
           au_chip_held / 1024, (long)(GTA_AUDIO_CHIP / 1024),
           au_n_copied, au_n_evicted);
    fflush(stdout);
}

/* ======================================================================== */
/* THE ENGINE NOTE                                                          */
/* ======================================================================== */

/* THE ORIGINAL'S OWN ENGINE (205).
 *
 * PROGRESS 168 said the original game had no continuous engine sound, having
 * looked only at the one-shot dispatcher and the speech call. There is a
 * third consumer: the car sound loop the original's routine, which every frame puts
 * each running car's engine on a positional list - sample 0x2d + the car
 * info's `engine` byte (45..57), at a rate the car's `sound_function` takes
 * from its speed. The autocorrelation shortlist 168 made (57, 49, 70) was a
 * guess at a question the binary answers; 57 is the BOAT's engine. The
 * choice of sample and the rate are now the caller's (gta_main.c), and
 * `engine <id>` in opts.txt still forces one sample for listening.
 *
 * THE CHANNEL IS RESERVED while the engine runs, because a note that gets
 * stolen mid-corner is worse than no note. Effects keep channel 0 (and 2/3
 * when no music is streaming); the engine owns 1. */
#define EN_DECIM 3              /* see gta_audio_engine_start() */
static void *en_chip;
static unsigned long en_bytes;
static int   en_base;                   /* the sample's own Paula period */
static int   en_sound = -1;
static int   en_fail = -1;              /* the last sample that would not start */
static int   en_period, en_vol;         /* what the hardware was last told */

int gta_audio_engine_start(const gta_sfx *bank, int n)
{
    unsigned long bytes;
    const signed char *src;

    if (!au_ok || !bank || !bank->data || n < 0 || n >= bank->count)
        return 0;
    if (en_on && en_sound == n)
        return 1;
    /* The caller asks every frame; a sample that could not be started is
     * not tried again (and the one playing not stopped) until it changes. */
    if (en_fail == n)
        return 0;
    gta_audio_engine_stop();
    en_fail = n;

    /* The table read directly, not through gta_sfx_sample() - see the note
     * on slot_for() for why that call cannot be trusted on this build. */
    bytes = bank->entry[n].length;
    src   = bank->data + bank->entry[n].offset;
    en_base = (int)bank->entry[n].period;
    if (bytes < 64 || en_base < 124)
        return 0;
    /* Paula's length register counts words and tops out at 131070 bytes;
     * a longer engine loop is simply truncated, which is inaudible in a
     * sample that is meant to repeat anyway. */
    if (bytes > 120000UL)
        bytes = 120000UL;

    /* A THIRD OF THE SAMPLES, AVERAGED. The original plays the engine at an
     * absolute rate out of a table that tops out at 77000 Hz (see
     * gta_audio_engine_rate); Paula stops at 28.6 kHz. Keeping every third
     * sample (the mean of each three) and asking Paula for a third of the
     * rate gives the same pitch at 25.7 kHz at most - and an engine's
     * rumble loses nothing it had above a third of its sample rate. */
    {
        unsigned long out = (bytes / EN_DECIM) & ~1UL, k;
        signed char *dst;
        en_chip = AmigaAudio_AllocSample(out);
        if (!en_chip) {
            printf("gta: engine - no Chip RAM for %lu bytes\n", out);
            fflush(stdout);
            return 0;
        }
        dst = (signed char *)en_chip;
        for (k = 0; k < out; k++) {
            const signed char *q = src + k * EN_DECIM;
            dst[k] = (signed char)(((int)q[0] + q[1] + q[2]) / 3);
        }
        bytes = out;
    }
    en_bytes = bytes;
    en_period = en_base * 3 > 65535 ? 65535 : en_base * 3;
    en_vol = 32;
    AmigaAudio_Stop(GTA_ENGINE_CH);
    if (sp_ch == GTA_ENGINE_CH)
        sp_ch = -1;
    if (!AmigaAudio_PlayLoop(GTA_ENGINE_CH, en_chip, en_bytes,
                             en_period, en_vol)) {
        AmigaAudio_FreeSample(en_chip);
        en_chip = NULL;
        printf("gta: engine - channel %d would not take the loop\n",
               GTA_ENGINE_CH);
        fflush(stdout);
        return 0;
    }
    en_on = 1;
    en_fail = -1;
    en_sound = n;
    printf("gta: engine - sound %d looping on ch%d, %lu bytes, period %d\n",
           n, GTA_ENGINE_CH, en_bytes, en_base);
    fflush(stdout);
    return 1;
}

void gta_audio_engine_stop(void)
{
    if (!en_on && !en_chip)
        return;
    if (au_ok)
        AmigaAudio_Stop(GTA_ENGINE_CH);
    /* THE STOP MUST COMPLETE BEFORE THE BUFFER GOES. AmigaAudio_Stop aborts
     * the write and waits for the reply, which is exactly what makes this
     * safe: freeing Chip RAM that DMA is still reading is a machine playing
     * whatever lands there next. */
    AmigaAudio_FreeSample(en_chip);
    en_chip = NULL;
    en_bytes = 0;
    en_on = 0;
    en_sound = -1;
}

int gta_audio_engine_playing(void) { return en_on; }

void gta_audio_engine_rate(long hz, int volume)
{
    long p;
    int v;

    if (!au_ok || !en_on)
        return;
    if (hz < 1000L) hz = 1000L;
    /* The rate is the ORIGINAL'S playback rate for the whole sample; the
     * loop holds a third of its samples, so Paula is asked for a third. */
    p = (long)((PAULA_CLOCK_PAL * EN_DECIM) / (unsigned long)hz);
    if (p < 124) p = 124;
    if (p > 65535L) p = 65535L;
    v = volume < 0 ? 0 : (volume > 64 ? 64 : volume);

    /* Only when it actually changed. ADCMD_PERVOL is a DoIO - a round trip
     * into the device - and sending an identical one sixty times a second
     * is pure cost. */
    if (p == en_period && v == en_vol)
        return;
    en_period = (int)p;
    en_vol = v;
    AmigaAudio_SetPeriod(GTA_ENGINE_CH, p, v);
}

/* ======================================================================== */
/* THE RADIO                                                                */
/* ======================================================================== */

/* The stream, and the refill amiga_audio.c pulls once a frame. Nothing here
 * runs from an interrupt: MusicService() is called from the main loop and
 * calls this, so ordinary stdio is safe. */
static AdpcmStream *mu_stream;
static int  mu_loop;
static long mu_samples;                 /* how many have been handed over */
static char mu_path[128];
static unsigned long mu_t0;
static unsigned long mu_said;   /* when the running report last spoke */
static int  mu_rate;

/* ---- THE OTHER KIND OF TRACK: an IFF 8SVX -------------------------------
 *
 * `.8svx` is what the first-run extractor writes (native/gta_iff.c), and it
 * is signed 8-bit mono at 22050 Hz - which is EXACTLY what Paula's DMA
 * fetches. So there is no decoder here at all: the refill is an fread
 * straight into the Chip buffer the hardware is about to read. Against the
 * ADPCM path that is a table lookup and three shifts per sample saved, at
 * the price of twice the disk; on a 68020 that trade is not close.
 *
 * Both kinds are supported on purpose. `.mus` is what a host-side convert
 * produces and half the size, `.8svx` is what an Amiga can make for itself
 * and cheaper to play; a player may have either, and the file says which by
 * its first twelve bytes rather than by its name. */
static FILE *mu_raw;
static long  mu_raw_start, mu_raw_left, mu_raw_len;

/* THE FIRST CHUNK HANDED TO THE MUSIC CHANNELS, read back out of the buffer
 * the hardware is about to play. Same argument as the effects probe above:
 * the rate report proves the stream is being PULLED at 22050 samples a
 * second and says nothing at all about what is in it. Once per track. */
static void music_probe(const signed char *dst, int n)
{
    long k, mn = 127, mx = -128, nz = 0;
    int lim = n < 4096 ? n : 4096;
    for (k = 0; k < lim; k++) {
        if (dst[k] < mn) mn = dst[k];
        if (dst[k] > mx) mx = dst[k];
        if (dst[k]) nz++;
    }
    printf("gta: music - FIRST CHUNK at %p: %ld of %d bytes non-zero, "
           "min %ld max %ld, first 8: %d %d %d %d %d %d %d %d\n",
           (const void *)dst, nz, lim, mn, mx,
           dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7]);
    fflush(stdout);
}

static int music_refill(void *ud, signed char *dst, int max)
{
    int n;
    (void)ud;
    if (mu_raw) {
        long want = (long)max < mu_raw_left ? (long)max : mu_raw_left;
        n = want > 0 ? (int)fread(dst, 1, (size_t)want, mu_raw) : 0;
        mu_raw_left -= n;
        if (n == 0 && mu_loop) {
            fseek(mu_raw, mu_raw_start, SEEK_SET);
            mu_raw_left = mu_raw_len;
            want = (long)max < mu_raw_left ? (long)max : mu_raw_left;
            n = (int)fread(dst, 1, (size_t)want, mu_raw);
            mu_raw_left -= n;
        }
        if (mu_samples == 0 && n > 0)
            music_probe(dst, n);
        mu_samples += n;
        return n;
    }
    if (!mu_stream)
        return 0;
    n = Adpcm_Decode(mu_stream, dst, max);
    if (n == 0 && mu_loop) {
        /* THE END, AND ROUND AGAIN. Rewinding costs a seek to the first
         * block, not a reload: the stream reads from disk block by block. */
        Adpcm_Rewind(mu_stream);
        n = Adpcm_Decode(mu_stream, dst, max);
    }
    if (mu_samples == 0 && n > 0)
        music_probe(dst, n);
    mu_samples += n;
    return n;
}

/* Open an 8SVX and leave the file positioned at the first BODY byte.
 * Returns its sample rate, or 0 when the file is not one. */
static int svx_open(const char *path)
{
    FILE *f;
    unsigned char hdr[12], ch[8], vhdr[20];
    int rate = 0;

    f = fopen(path, "rb");
    if (!f)
        return 0;
    if (fread(hdr, 1, 12, f) != 12 ||
        hdr[0] != 'F' || hdr[1] != 'O' || hdr[2] != 'R' || hdr[3] != 'M' ||
        hdr[8] != '8' || hdr[9] != 'S' || hdr[10] != 'V' || hdr[11] != 'X') {
        fclose(f);
        return 0;
    }
    while (fread(ch, 1, 8, f) == 8) {
        unsigned long clen = ((unsigned long)ch[4] << 24)
                           | ((unsigned long)ch[5] << 16)
                           | ((unsigned long)ch[6] << 8) | ch[7];
        if (ch[0] == 'V' && ch[1] == 'H' && ch[2] == 'D' && ch[3] == 'R'
            && clen >= 20) {
            if (fread(vhdr, 1, 20, f) != 20) break;
            rate = ((int)vhdr[12] << 8) | vhdr[13];
            /* sCompression must be 0: a Fibonacci-delta 8SVX would play as
             * noise, and playing noise is worse than refusing the file. */
            if (vhdr[15] != 0) {
                printf("gta: music - %s is a compressed 8SVX (%d); not "
                       "played\n", path, vhdr[15]);
                fflush(stdout);
                break;
            }
            if (clen > 20) fseek(f, (long)(clen - 20), SEEK_CUR);
            if (clen & 1UL) fseek(f, 1L, SEEK_CUR);
        } else if (ch[0] == 'B' && ch[1] == 'O' && ch[2] == 'D' &&
                   ch[3] == 'Y') {
            if (rate < 1) rate = GTA_IFF_RATE;
            mu_raw = f;
            mu_raw_start = ftell(f);
            mu_raw_len = mu_raw_left = (long)clen;
            return rate;
        } else {
            fseek(f, (long)(clen + (clen & 1UL)), SEEK_CUR);
        }
    }
    fclose(f);
    return 0;
}

/* Both kinds of source, closed the same way, because every failure path
 * below has to close whichever one got opened. */
static void music_close_source(void)
{
    if (mu_stream) { Adpcm_Close(mu_stream); mu_stream = NULL; }
    if (mu_raw)    { fclose(mu_raw);         mu_raw    = NULL; }
}

/* Where in the track to begin. Set by the radio just before it starts one,
 * consumed and cleared by music_start - which keeps the public entry point
 * the two-argument one every other caller uses. */
static long mu_start_at;

int gta_audio_music_start(const char *path, int loop)
{
    int rate, period;
    long start_at = mu_start_at;

    mu_start_at = 0;
    if (!au_ok || !path)
        return 0;
    gta_audio_music_stop();
    /* WHICH KIND OF TRACK, decided by the file and not by its name: an 8SVX
     * is streamed raw, anything else goes to the ADPCM decoder. */
    rate = svx_open(path);
    if (!rate) {
        mu_stream = Adpcm_Open(path);
        if (!mu_stream) {
            printf("gta: music - cannot open %s\n", path);
            fflush(stdout);
            return 0;
        }
        rate = Adpcm_Rate(mu_stream);
    }
    if (rate < 1) rate = 22050;
    /* PAL. The same arithmetic gtamusic prints, so a track converted at a
     * rate Paula cannot reach is caught here rather than played fast. */
    period = 3546895 / rate;
    if (period < 124) {
        printf("gta: music - %s is %d Hz, which is past Paula's DMA floor; "
               "not played\n", path, rate);
        fflush(stdout);
        music_close_source();
        return 0;
    }
    /* JOIN THE STATION WHERE IT HAS GOT TO. Both kinds can do it: an 8SVX
     * BODY is seekable to the byte, and IMA ADPCM to the block, because
     * every block carries its own predictor. */
    if (start_at > 0) {
        if (mu_raw) {
            if (start_at < mu_raw_len) {
                fseek(mu_raw, mu_raw_start + start_at, SEEK_SET);
                mu_raw_left = mu_raw_len - start_at;
            }
        } else if (mu_stream) {
            Adpcm_SeekSample(mu_stream, start_at);
        }
    }

    mu_loop = loop;
    mu_samples = 0;
    mu_rate = rate;
    mu_t0 = amiga_uclock_us();
    mu_said = mu_t0;
    strncpy(mu_path, path, sizeof mu_path - 1);
    mu_path[sizeof mu_path - 1] = 0;

    /* A chunk of about a fifth of a second: long enough that a frame's worth
     * of jitter cannot empty it, short enough that stopping is immediate and
     * two of them per channel fit comfortably in Chip. */
    if (!AmigaAudio_MusicStart(period, rate / 5, music_refill, NULL)) {
        printf("gta: music - the channels or the Chip RAM would not come\n");
        fflush(stdout);
        music_close_source();
        return 0;
    }
    printf("gta: music - %s, %d Hz (period %d)%s, %s\n", path, rate, period,
           loop ? ", looping" : "",
           mu_raw ? "8SVX straight to Paula" : "IMA ADPCM");
    fflush(stdout);
    return 1;
}

void gta_audio_music_stop(void)
{
    if (!au_ok)
        return;
    AmigaAudio_MusicStop();
    if (mu_stream || mu_raw) {
        /* THE PROOF THAT IT STREAMED, and it is the same argument the sound
         * effects use: a stream that really fed the hardware handed over
         * samples at the sample rate. One that started and stalled did not.
         * Nobody here can hear the emulator; this is what stands in for it. */
        unsigned long us = amiga_uclock_us() - mu_t0;
        long ms = (long)(us / 1000UL);
        printf("gta: music - handed %ld samples in %ld ms = %ld Hz "
               "(wanted %d)\n", mu_samples, ms,
               ms > 0 ? mu_samples * 1000L / ms : 0L, mu_rate);
        fflush(stdout);
        music_close_source();
    }
}

void gta_audio_music_tick(void)
{
    if (!au_ok)
        return;
    radio_clock();
    AmigaAudio_MusicService();
    /* AND ONCE EVERY FIVE SECONDS, THE RATE IT IS ACTUALLY FEEDING AT.
     * The stop line says it too, but a game sitting on the title screen is
     * never stopped by anything that survives to write a log - the harness
     * reloads it and the log starts again - so the running figure is the one
     * that can be read. A stream that stalled reads far below its rate.
     *
     * BOTH KINDS, and the first version of the 8SVX path tested only the
     * ADPCM one here - so an 8SVX track played with no running report at
     * all, which is the one piece of evidence this port has that music is
     * reaching the hardware. */
    if (mu_stream || mu_raw) {
        unsigned long now = amiga_uclock_us();
        /* Not before the track has been going five seconds. The buffers are
         * PREFILLED before the clock starts - eight chunks, about 36 000
         * samples - so a report taken at sixteen milliseconds reads
         * 2 205 000 Hz and means nothing at all. */
        if (now - mu_said > 5000000UL && now - mu_t0 > 5000000UL) {
            unsigned long us = now - mu_t0;
            long ms = (long)(us / 1000UL);
            mu_said = now;
            printf("gta: music - %ld samples in %ld ms = %ld Hz (wanted %d)\n",
                   mu_samples, ms, ms > 0 ? mu_samples * 1000L / ms : 0L,
                   mu_rate);
            fflush(stdout);
        }
    }
}

int gta_audio_music_playing(void)
{
    return au_ok && (mu_stream != NULL || mu_raw != NULL)
        && AmigaAudio_MusicActive();
}

void gta_audio_music_volume(int vol)
{
    if (au_ok)
        AmigaAudio_MusicSetVolume(vol);
}

/* ---- the car radio ------------------------------------------------------ */

#define GTA_RADIO_MAX 8
static char ra_path[GTA_RADIO_MAX][160];
static int  ra_count;
static int  ra_next;
static char ra_police[160];             /* the police band, or empty */

/* ---- THE STATION CLOCK --------------------------------------------------
 *
 * *"jak pierwszy raz wsiadamy to stacja startuje w losowym miejscu (a potem
 * licznik w tle liczy sekundy tak jakby to sie odtwarzalo zeby dzialala jak
 * prawdziwa stacja - i zeby sie loopowalo)"* - the developer, and it is the
 * right model: a radio station is not a track that starts when you press
 * play. It is a thing that has been running all along, and getting into a
 * car joins it wherever it has got to.
 *
 * So each station carries a random offset chosen once, and one clock counts
 * the milliseconds since the game started. The position is
 *
 *     (offset + elapsed) modulo the track's own length
 *
 * which loops for nothing - the modulo IS the loop - and costs no CPU while
 * nobody is listening, because nothing is decoded: the clock is arithmetic
 * over a `long`, not a silent stream.
 *
 * THE ARITHMETIC IS FOLDED BEFORE IT IS SCALED. Milliseconds times 22050
 * passes 2^31 after twenty-seven hours, and this is a port whose whole point
 * is machines that stay switched on; taking the modulo of the ELAPSED TIME
 * against the track's duration first keeps every product small for ever. */
static long ra_len[GTA_RADIO_MAX];      /* the track's length in samples */
static long ra_off[GTA_RADIO_MAX];      /* its random starting offset */
static int  ra_rate[GTA_RADIO_MAX];
static unsigned long ra_ms;             /* since the scan, accumulated */
static unsigned long ra_ms_last;        /* the microsecond clock, last seen */
static int  ra_ms_have;

/* How long a track is and at what rate, without leaving anything open.
 * Both kinds, told apart the same way the player tells them apart. */
static void track_info(const char *path, long *samples, int *rate)
{
    FILE *f;
    unsigned char hdr[12], ch[8], vhdr[20];

    *samples = 0;
    *rate = GTA_IFF_RATE;

    f = fopen(path, "rb");
    if (!f)
        return;
    if (fread(hdr, 1, 12, f) == 12 &&
        hdr[0] == 'F' && hdr[1] == 'O' && hdr[2] == 'R' && hdr[3] == 'M' &&
        hdr[8] == '8' && hdr[9] == 'S' && hdr[10] == 'V' && hdr[11] == 'X') {
        while (fread(ch, 1, 8, f) == 8) {
            unsigned long clen = ((unsigned long)ch[4] << 24)
                               | ((unsigned long)ch[5] << 16)
                               | ((unsigned long)ch[6] << 8) | ch[7];
            if (ch[0] == 'V' && ch[1] == 'H' && ch[2] == 'D' && ch[3] == 'R'
                && clen >= 20) {
                if (fread(vhdr, 1, 20, f) != 20) break;
                *rate = ((int)vhdr[12] << 8) | vhdr[13];
                if (clen > 20) fseek(f, (long)(clen - 20), SEEK_CUR);
                if (clen & 1UL) fseek(f, 1L, SEEK_CUR);
            } else if (ch[0] == 'B' && ch[1] == 'O' && ch[2] == 'D' &&
                       ch[3] == 'Y') {
                *samples = (long)clen;
                break;
            } else {
                fseek(f, (long)(clen + (clen & 1UL)), SEEK_CUR);
            }
        }
        fclose(f);
    } else {
        AdpcmStream *s;
        fclose(f);
        s = Adpcm_Open(path);
        if (!s)
            return;
        *rate = Adpcm_Rate(s);
        *samples = Adpcm_Samples(s);
        Adpcm_Close(s);
    }
    if (*rate < 1)
        *rate = GTA_IFF_RATE;
}

/* Where station `i` has got to, in samples. */
static long station_pos(int i)
{
    unsigned long dur_ms, el;
    long secs, rem;
    int r;

    if (i < 0 || i >= ra_count || ra_len[i] <= 0)
        return 0;
    r = ra_rate[i] > 0 ? ra_rate[i] : GTA_IFF_RATE;
    dur_ms = (unsigned long)(ra_len[i] / r) * 1000UL
           + (unsigned long)(ra_len[i] % r) * 1000UL / (unsigned long)r;
    if (dur_ms == 0)
        return 0;
    el = ra_ms % dur_ms;                /* fold FIRST - see the note above */
    secs = (long)(el / 1000UL);
    rem  = (long)(el % 1000UL);
    return (ra_off[i] + secs * r + rem * r / 1000) % ra_len[i];
}

static unsigned long ra_seed;

void gta_audio_radio_scan(const char *dir)
{
    int i;
    ra_count = 0;
    ra_next = 0;
    ra_ms = 0;
    ra_ms_have = 0;
    ra_seed = amiga_uclock_us() | 1UL;
    if (!dir)
        return;
    for (i = 1; i <= GTA_RADIO_MAX; i++) {
        FILE *f;
        char p[160];
        /* snprintf, never sprintf: on this libc sprintf shifts its arguments
         * and would build the wrong name in silence. */
        /* THE EXTRACTED TRACK WINS. `radioN.8svx` is what the first-run
         * extractor writes on the Amiga itself; `radioN.mus` is what a
         * host-side gtamusic run produces. A player may have either, and one
         * who has both gets the cheaper of the two to play. */
        snprintf(p, sizeof p, "%sradio%d.8svx", dir, i);
        f = fopen(p, "rb");
        if (!f) {
            snprintf(p, sizeof p, "%sradio%d.mus", dir, i);
            f = fopen(p, "rb");
        }
        if (!f)
            continue;
        fclose(f);
        strncpy(ra_path[ra_count], p, sizeof ra_path[0] - 1);
        ra_path[ra_count][sizeof ra_path[0] - 1] = 0;
        /* AND WHERE THIS STATION IS IN ITS OWN BROADCAST. The offset is
         * drawn once, here, so it is the same for the whole session - a
         * station that jumped somewhere new every time you got into a car
         * would be a shuffle, not a station.
         *
         * The seed is the microsecond clock, which on a machine that has
         * just booted a hardfile and loaded eleven megabytes of data is
         * genuinely unpredictable at the microsecond. The generator is the
         * usual 16-bit-safe LCG: this needs a spread, not statistics. */
        track_info(p, &ra_len[ra_count], &ra_rate[ra_count]);
        ra_seed = ra_seed * 1103515245UL + 12345UL;
        ra_off[ra_count] = ra_len[ra_count] > 0
            ? (long)((ra_seed >> 16) % (unsigned long)ra_len[ra_count]) : 0;
        printf("gta: radio - station %d %s, %ld samples at %d Hz, "
               "starts %ld s in\n", ra_count + 1, p, ra_len[ra_count],
               ra_rate[ra_count],
               ra_rate[ra_count] > 0 ? ra_off[ra_count] / ra_rate[ra_count]
                                     : 0L);
        fflush(stdout);
        ra_count++;
    }
    /* AND THE POLICE BAND, which is NOT one of the stations. It is a
     * different thing that happens in a different car - see
     * gta_audio_radio_police(). */
    ra_police[0] = 0;
    {
        FILE *f;
        char p[160];
        snprintf(p, sizeof p, "%spolice.8svx", dir);
        f = fopen(p, "rb");
        if (!f) {
            snprintf(p, sizeof p, "%spolice.mus", dir);
            f = fopen(p, "rb");
        }
        if (f) {
            fclose(f);
            strncpy(ra_police, p, sizeof ra_police - 1);
            ra_police[sizeof ra_police - 1] = 0;
        }
    }
    printf("gta: radio - %d station%s, police band %s\n", ra_count,
           ra_count == 1 ? "" : "s", ra_police[0] ? "yes" : "no");
    fflush(stdout);
}

int gta_audio_radio_count(void) { return ra_count; }

int gta_audio_radio_next(void)
{
    int i;
    if (!au_ok || ra_count <= 0)
        return 0;
    i = ra_next;
    ra_next = (ra_next + 1) % ra_count;
    /* JOIN IT WHERE IT HAS GOT TO - see the station clock above. */
    mu_start_at = station_pos(i);
    printf("gta: radio - station %d, joining at %ld s\n", i + 1,
           ra_rate[i] > 0 ? mu_start_at / ra_rate[i] : 0L);
    fflush(stdout);
    return gta_audio_music_start(ra_path[i], 1);
}

/* The station clock, advanced from whichever tick the caller has. Both of
 * them call it and that is harmless: it accumulates a DELTA, so being called
 * twice a frame splits one frame's worth in two. The microsecond clock wraps
 * every 71 minutes and the unsigned subtraction rides straight over it. */
static void radio_clock(void)
{
    unsigned long now = amiga_uclock_us();
    if (!ra_ms_have) {
        ra_ms_last = now;
        ra_ms_have = 1;
        return;
    }
    ra_ms += (unsigned long)(now - ra_ms_last) / 1000UL;
    /* Keep the remainder rather than dropping it: at 60 fps a frame is 16 667
     * us and throwing away the 667 would lose four seconds an hour. */
    ra_ms_last = now - (unsigned long)(now - ra_ms_last) % 1000UL;
}

int gta_audio_radio_police(void)
{
    if (!au_ok)
        return 0;
    /* No police band converted: a police car gets an ordinary station
     * rather than silence, which is the lesser of the two wrong answers. */
    if (!ra_police[0])
        return gta_audio_radio_next();
    return gta_audio_music_start(ra_police, 1);
}
