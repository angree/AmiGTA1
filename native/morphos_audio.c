/* AHI audio for MorphOS - the AmigaAudio_* contract, on a machine with no Paula.
 *
 * This is the MorphOS counterpart of native/amiga_audio.c and it implements the
 * SAME amiga_audio.h API, so native/gta_audio.c - the mixer the game actually
 * talks to, and which is portable - is byte-for-byte the code the 68k build
 * uses. Only this file changes. Exactly the arrangement morphos_gfx.c has with
 * amiga_gfx.c, for exactly the same reason.
 *
 * WHY NOT amiga_audio.c. It does not drive an audio API, it drives PAULA:
 * ADCMD_ALLOCATE over four hardware channels, one CMD_WRITE per effect played
 * by DMA, sample data that must live in Chip RAM "because Paula DMA reads
 * nothing else", and a period register counted in the PAL colour clock. None
 * of that exists on a PowerPC machine. MorphOS carries an audio.device for
 * compatibility, so linking the Paula file produced something that might have
 * made a noise and might have sat silent - and either way it would have been
 * the wrong thing to ship on a system whose actual sound API is AHI.
 *
 * WHAT MAPS AND WHAT DOES NOT.
 *
 * AHI's low-level API is a close relative of Paula's model - allocate audio,
 * load sounds into slots, play a sound on a channel, change its frequency and
 * volume while it runs - so most of the contract carries over directly:
 *
 *   Paula period      ->  a frequency in Hz. period is a divisor of the PAL
 *                         colour clock, so freq = 3546895 / period. The engine
 *                         note's SetPeriod becomes AHI_SetFreq and still revs.
 *   volume 0..64      ->  AHI's Fixed 0..0x10000, which is volume << 10.
 *   Chip RAM          ->  plain memory. AHI mixes in software and reads from
 *                         anywhere; AllocSample is an AllocVec here.
 *   channels 0,3 L    ->  AHIP_Pan 0 and 1,2 -> 0x10000. Mirrored rather than
 *                         centred, because the SFX allocator in gta_audio.c
 *                         picks channels expecting that stereo layout.
 *
 * ONE THING DOES NOT MAP, and it is worth being plain about: knowing when a
 * one-shot has FINISHED. audio.device replies its IORequest and amiga_audio.c
 * polls CheckIO. AHI's equivalent is an end-of-sound Hook (AHIA_SoundFunc),
 * and a Hook called from AHI on MorphOS must be a PPC/68k ABI trampoline -
 * EmulLibEntry - which is precisely the kind of thing that cannot be got right
 * without a machine to try it on. So AmigaAudio_ChannelIdle() works the
 * duration out instead: bytes and frequency give the length, amiga_uclock_us()
 * gives the clock, and a channel is idle once that long has passed. It is
 * exact for a sound that plays to its end, which is every sound the game
 * starts, and it errs on the side of "still busy" rather than cutting one off.
 *
 * NONE OF THIS HAS BEEN HEARD. There is no MorphOS machine on the build host.
 * It compiles, the AHI calls are against the SDK's own prototypes, and the
 * arithmetic above is checkable by reading - but nobody has played a sound
 * with it. Treat the first run as the test.
 *
 * MIT, like the rest of the port.
 */

#ifdef __MORPHOS__

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/ahi.h>

#include <proto/exec.h>
#include <proto/ahi.h>

#include <string.h>
#include <stdio.h>

#include "amiga_audio.h"
#include "amiga_uclock.h"

/* Declared extern by <proto/ahi.h>; ours to define and to fill. */
struct Library *AHIBase;

/* PAL colour clock, the number a Paula period divides. The callers speak in
 * periods because the Amiga does, and gta_audio.c computes them from sample
 * rates; this is the one constant that turns them back into Hz. */
#define PAL_CLOCK 3546895UL

/* Sound slots. 0..3 are the four channels' one-shots - a channel reloads its
 * own slot on every Play, so a slot is never shared between channels and a
 * sound can never be replaced out from under a channel that is playing it.
 * 4 and 5 are the music double buffer. */
#define SND_MUSIC_A  AMIGA_AUDIO_CHANNELS
#define SND_MUSIC_B  (AMIGA_AUDIO_CHANNELS + 1)
#define SND_COUNT    (AMIGA_AUDIO_CHANNELS + 2)

/* Which channels music takes, and it is the two amiga_audio.h documents: the
 * same mono stream on a left and a right channel so it is never stuck in one
 * ear. gta_audio.c asks AmigaAudio_MusicActive() and keeps off these two. */
#define MUSIC_CH_L 3
#define MUSIC_CH_R 2

static struct MsgPort    *g_port;
static struct AHIRequest *g_req;
static struct AHIAudioCtrl *g_ctrl;
static int  g_open;

/* Per-channel bookkeeping for the duration-based idle test. g_end_us is the
 * microsecond reading at which the sound stops; 0 means the channel is free.
 * g_loop marks a channel started with PlayLoop, which never ends on its own
 * and is therefore never idle until Stop. */
static unsigned long g_end_us[AMIGA_AUDIO_CHANNELS];
static int           g_loop[AMIGA_AUDIO_CHANNELS];

/* ---- helpers ------------------------------------------------------------ */

static ULONG period_to_freq(int period)
{
    if (period < 1) period = 1;
    return PAL_CLOCK / (ULONG)period;
}

/* 0..64 -> AHI's Fixed, where 0x10000 is unity. 64 << 10 is exactly 0x10000,
 * so full Paula volume is full AHI volume and nothing is scaled away. */
static Fixed vol_to_fixed(int volume)
{
    if (volume < 0)  volume = 0;
    if (volume > 64) volume = 64;
    return (Fixed)((ULONG)volume << 10);
}

/* Paula wires channels 0 and 3 to the left jack, 1 and 2 to the right. */
static Fixed pan_for(int ch)
{
    return (ch == 0 || ch == 3) ? (Fixed)0 : (Fixed)0x10000;
}

/* How long `bytes` 8-bit mono samples last at this period, in microseconds. */
static unsigned long duration_us(unsigned long bytes, int period)
{
    ULONG freq = period_to_freq(period);
    if (freq == 0) return 0;
    return (unsigned long)((unsigned long long)bytes * 1000000ULL /
                           (unsigned long long)freq);
}

static int ch_valid(int ch)
{
    return g_open && ch >= 0 && ch < AMIGA_AUDIO_CHANNELS;
}

/* Register a buffer in a sound slot. AHI counts ahisi_Length in SAMPLES, not
 * bytes - for 8-bit mono the two are the same, which is the only reason this
 * can pass `bytes` straight through. */
static int load_sound(int slot, void *data, unsigned long bytes)
{
    struct AHISampleInfo si;

    si.ahisi_Type    = AHIST_M8S;      /* mono, 8-bit signed */
    si.ahisi_Address = (APTR)data;
    si.ahisi_Length  = (ULONG)bytes;

    return AHI_LoadSound((UWORD)slot, AHIST_SAMPLE, (APTR)&si, g_ctrl) == 0;
}

/* ---- open / close ------------------------------------------------------- */

int AmigaAudio_Open(void)
{
    if (g_open) return 1;

    g_port = CreateMsgPort();
    if (g_port == NULL) {
        fprintf(stdout, "morphos: audio: CreateMsgPort failed - silent\n");
        fflush(stdout);
        return 0;
    }

    g_req = (struct AHIRequest *)
            CreateIORequest(g_port, sizeof(struct AHIRequest));
    if (g_req == NULL) {
        DeleteMsgPort(g_port); g_port = NULL;
        fprintf(stdout, "morphos: audio: CreateIORequest failed - silent\n");
        fflush(stdout);
        return 0;
    }

    /* Version 4 is the first with the low-level API this file uses. */
    g_req->ahir_Version = 4;
    if (OpenDevice((CONST_STRPTR)AHINAME, AHI_NO_UNIT,
                   (struct IORequest *)g_req, 0) != 0) {
        DeleteIORequest((struct IORequest *)g_req); g_req = NULL;
        DeleteMsgPort(g_port); g_port = NULL;
        fprintf(stdout, "morphos: audio: no ahi.device v4 - silent\n");
        fflush(stdout);
        return 0;
    }
    AHIBase = (struct Library *)g_req->ahir_Std.io_Device;

    /* AHI_DEFAULT_ID and AHI_DEFAULT_FREQ mean "whatever the user set up in
     * the AHI preferences", which is the right answer for a game: the machine
     * owner has already chosen their output and its quality, and a game that
     * overrode it would be the one program on the system that did.
     *
     * No AHIA_SoundFunc - see the note at the top of this file about why the
     * end-of-sound Hook is not used. */
    g_ctrl = AHI_AllocAudio(AHIA_AudioID,  AHI_DEFAULT_ID,
                                AHIA_MixFreq,  AHI_DEFAULT_FREQ,
                                AHIA_Channels, AMIGA_AUDIO_CHANNELS,
                                AHIA_Sounds,   SND_COUNT,
                                TAG_DONE);
    if (g_ctrl == NULL) {
        CloseDevice((struct IORequest *)g_req);
        DeleteIORequest((struct IORequest *)g_req); g_req = NULL;
        DeleteMsgPort(g_port); g_port = NULL;
        AHIBase = NULL;
        fprintf(stdout, "morphos: audio: AHI_AllocAudio failed - silent\n");
        fflush(stdout);
        return 0;
    }

    AHI_ControlAudio(g_ctrl, AHIC_Play, TRUE, TAG_DONE);

    memset((void *)g_end_us, 0, sizeof g_end_us);
    memset((void *)g_loop,   0, sizeof g_loop);
    g_open = 1;

    fprintf(stdout, "morphos: audio: AHI open, %d channels, %d sound slots\n",
            AMIGA_AUDIO_CHANNELS, SND_COUNT);
    fflush(stdout);
    return 1;
}

void AmigaAudio_MusicStop(void);   /* used by Close, defined below */

void AmigaAudio_Close(void)
{
    int i;

    if (g_ctrl != NULL) {
        AmigaAudio_MusicStop();
        for (i = 0; i < AMIGA_AUDIO_CHANNELS; i++)
            AHI_SetSound((UWORD)i, AHI_NOSOUND, 0, 0, g_ctrl, AHISF_IMM);
        AHI_ControlAudio(g_ctrl, AHIC_Play, FALSE, TAG_DONE);
        AHI_FreeAudio(g_ctrl);          /* unloads every sound with it */
        g_ctrl = NULL;
    }
    if (g_req != NULL) {
        CloseDevice((struct IORequest *)g_req);
        DeleteIORequest((struct IORequest *)g_req);
        g_req = NULL;
    }
    if (g_port != NULL) { DeleteMsgPort(g_port); g_port = NULL; }
    AHIBase = NULL;
    g_open = 0;
}

/* ---- sample memory ------------------------------------------------------ */

/* No Chip RAM, and no alignment rule either: AHI mixes in software and reads
 * the buffer like any other memory. MEMF_PUBLIC because the mixing may happen
 * on another task's time. The even-length rule the Paula version documents is
 * Paula's; nothing here cares. */
void *AmigaAudio_AllocSample(unsigned long bytes)
{
    if (bytes == 0) return NULL;
    return AllocVec(bytes, MEMF_PUBLIC | MEMF_CLEAR);
}

void AmigaAudio_FreeSample(void *p)
{
    if (p != NULL) FreeVec(p);
}

/* ---- one-shots ---------------------------------------------------------- */

int AmigaAudio_ChannelIdle(int ch)
{
    if (!ch_valid(ch)) return 0;
    if (g_loop[ch])    return 0;        /* a loop ends only at Stop */
    if (g_end_us[ch] == 0) return 1;

    /* Wrap-safe: amiga_uclock_us() is 32 bits and turns over every 71 minutes,
     * so the comparison is a subtraction against the deadline rather than a
     * "now > end", which would go wrong for the rest of a wrap. */
    if ((unsigned long)(amiga_uclock_us() - g_end_us[ch]) < 0x80000000UL) {
        g_end_us[ch] = 0;
        return 1;
    }
    return 0;
}

static int play_common(int ch, void *data, unsigned long bytes,
                       int period, int volume, int loop)
{
    ULONG freq;

    if (!ch_valid(ch) || data == NULL || bytes == 0) return 0;
    if (!AmigaAudio_ChannelIdle(ch)) return 0;
    if (!load_sound(ch, data, bytes)) return 0;

    freq = period_to_freq(period);

    /* AHIP_LoopSound is what makes the engine note go round and round. Setting
     * it to the same slot as AHIP_Sound means "when this sound ends, play it
     * again from the top", which is audio.device's ioa_Cycles = 0. */
    if (loop) {
        AHI_Play(g_ctrl,
                 AHIP_BeginChannel, ch,
                 AHIP_Freq,      freq,
                 AHIP_Vol,       vol_to_fixed(volume),
                 AHIP_Pan,       pan_for(ch),
                 AHIP_Sound,     ch,
                 AHIP_LoopFreq,  freq,
                 AHIP_LoopVol,   vol_to_fixed(volume),
                 AHIP_LoopPan,   pan_for(ch),
                 AHIP_LoopSound, ch,
                 AHIP_EndChannel, 0,
                 TAG_DONE);
        g_loop[ch]   = 1;
        g_end_us[ch] = 0;
    } else {
        AHI_Play(g_ctrl,
                 AHIP_BeginChannel, ch,
                 AHIP_Freq,   freq,
                 AHIP_Vol,    vol_to_fixed(volume),
                 AHIP_Pan,    pan_for(ch),
                 AHIP_Sound,  ch,
                 AHIP_EndChannel, 0,
                 TAG_DONE);
        g_loop[ch]   = 0;
        g_end_us[ch] = amiga_uclock_us() + duration_us(bytes, period);
        if (g_end_us[ch] == 0) g_end_us[ch] = 1;   /* 0 means "free" */
    }
    return 1;
}

int AmigaAudio_Play(int ch, void *chipdata, unsigned long bytes,
                    int period, int volume)
{
    return play_common(ch, chipdata, bytes, period, volume, 0);
}

int AmigaAudio_PlayLoop(int ch, void *chipdata, unsigned long bytes,
                        int period, int volume)
{
    return play_common(ch, chipdata, bytes, period, volume, 1);
}

/* The engine note revving. AHISF_IMM is "now", not "at the next buffer", which
 * is what makes this track the throttle instead of lagging it. */
void AmigaAudio_SetPeriod(int ch, int period, int volume)
{
    if (!ch_valid(ch)) return;
    AHI_SetFreq((UWORD)ch, period_to_freq(period), g_ctrl, AHISF_IMM);
    AHI_SetVol((UWORD)ch, vol_to_fixed(volume), pan_for(ch), g_ctrl, AHISF_IMM);
}

void AmigaAudio_Stop(int ch)
{
    if (!ch_valid(ch)) return;
    AHI_SetSound((UWORD)ch, AHI_NOSOUND, 0, 0, g_ctrl, AHISF_IMM);
    g_end_us[ch] = 0;
    g_loop[ch]   = 0;
}

/* ---- streaming music ---------------------------------------------------- */

/* DOUBLE BUFFERED, SWAPPED ON THE CLOCK.
 *
 * The Paula version queues two CMD_WRITEs per channel and lets audio.device
 * chain them, which is gapless by construction. AHI has no queue: a channel
 * plays one sound, and the way to be told it ended is the end-of-sound Hook
 * this file does not use (see the top). So the swap is scheduled the same way
 * a one-shot's end is: work out when the chunk runs out, and in
 * AmigaAudio_MusicService - called once a frame - start the next one when that
 * time has passed.
 *
 * WHAT THAT COSTS, stated rather than glossed: the next chunk starts up to one
 * frame late, so there can be a seam of a few milliseconds between chunks
 * where Paula would have been seamless. With the caller's chunk of a fifth of
 * a second that is a swap every 200 ms serviced every 16 ms. It may be
 * inaudible, it may click - that is exactly the kind of thing that needs a
 * machine to judge, and this has not been on one.
 */
static signed char *g_mus_buf[2];
static int   g_mus_slot[2]  = { SND_MUSIC_A, SND_MUSIC_B };
static int   g_mus_len[2];          /* samples actually filled */
static int   g_mus_cur;             /* which buffer is playing */
static int   g_mus_chunk;           /* buffer capacity in samples */
static int   g_mus_period;
static int   g_mus_vol = 64;
static int   g_mus_active;
static int   g_mus_drained;         /* refill returned 0 */
static int   g_mus_finished;
static unsigned long g_mus_end_us;
static int  (*g_mus_refill)(void *ud, signed char *dst, int max);
static void *g_mus_ud;

/* Start buffer `i` on both music channels from ONE AHI_Play call, so the two
 * are sample-locked rather than started a moment apart. */
static void music_start_buffer(int i)
{
    struct AHISampleInfo si;
    ULONG freq = period_to_freq(g_mus_period);
    Fixed v = vol_to_fixed(g_mus_vol);

    si.ahisi_Type    = AHIST_M8S;
    si.ahisi_Address = (APTR)g_mus_buf[i];
    si.ahisi_Length  = (ULONG)g_mus_len[i];
    if (AHI_LoadSound((UWORD)g_mus_slot[i], AHIST_SAMPLE, (APTR)&si, g_ctrl) != 0)
        return;

    AHI_Play(g_ctrl,
             AHIP_BeginChannel, MUSIC_CH_L,
             AHIP_Freq,  freq,
             AHIP_Vol,   v,
             AHIP_Pan,   (Fixed)0,
             AHIP_Sound, g_mus_slot[i],
             AHIP_EndChannel, 0,
             AHIP_BeginChannel, MUSIC_CH_R,
             AHIP_Freq,  freq,
             AHIP_Vol,   v,
             AHIP_Pan,   (Fixed)0x10000,
             AHIP_Sound, g_mus_slot[i],
             AHIP_EndChannel, 0,
             TAG_DONE);

    g_mus_cur    = i;
    g_mus_end_us = amiga_uclock_us() +
                   duration_us((unsigned long)g_mus_len[i], g_mus_period);
}

static int music_fill(int i)
{
    int n;
    if (g_mus_refill == NULL) return 0;
    n = g_mus_refill(g_mus_ud, g_mus_buf[i], g_mus_chunk);
    if (n < 0) n = 0;
    if (n > g_mus_chunk) n = g_mus_chunk;
    g_mus_len[i] = n;
    return n;
}

int AmigaAudio_MusicStart(int period, int chunk_samples,
                          int (*refill)(void *ud, signed char *dst, int max),
                          void *ud)
{
    int i;

    if (!g_open || refill == NULL) return 0;
    AmigaAudio_MusicStop();

    if (chunk_samples < 256)   chunk_samples = 256;
    if (chunk_samples > 65536) chunk_samples = 65536;

    for (i = 0; i < 2; i++) {
        g_mus_buf[i] = (signed char *)AllocVec((ULONG)chunk_samples,
                                               MEMF_PUBLIC | MEMF_CLEAR);
        if (g_mus_buf[i] == NULL) {
            AmigaAudio_MusicStop();
            return 0;
        }
    }

    g_mus_chunk    = chunk_samples;
    g_mus_period   = period;
    g_mus_refill   = refill;
    g_mus_ud       = ud;
    g_mus_drained  = 0;
    g_mus_finished = 0;

    if (music_fill(0) == 0) {           /* nothing to play at all */
        AmigaAudio_MusicStop();
        return 0;
    }
    music_fill(1);
    if (g_mus_len[1] == 0) g_mus_drained = 1;

    g_mus_active = 1;
    music_start_buffer(0);
    return 1;
}

void AmigaAudio_MusicStop(void)
{
    int i;

    if (g_ctrl != NULL && g_mus_active) {
        AHI_SetSound((UWORD)MUSIC_CH_L, AHI_NOSOUND, 0, 0, g_ctrl, AHISF_IMM);
        AHI_SetSound((UWORD)MUSIC_CH_R, AHI_NOSOUND, 0, 0, g_ctrl, AHISF_IMM);
    }
    for (i = 0; i < 2; i++) {
        if (g_mus_buf[i] != NULL) { FreeVec(g_mus_buf[i]); g_mus_buf[i] = NULL; }
        g_mus_len[i] = 0;
    }
    /* The two channels go back to the SFX pool the moment this clears. */
    g_mus_active   = 0;
    g_mus_drained  = 0;
    g_mus_finished = 0;
    g_mus_refill   = NULL;
    g_mus_ud       = NULL;
    g_mus_end_us   = 0;
}

void AmigaAudio_MusicService(void)
{
    int next;

    if (!g_mus_active) return;

    /* Not yet - wrap-safe, same reasoning as ChannelIdle. */
    if ((unsigned long)(amiga_uclock_us() - g_mus_end_us) >= 0x80000000UL)
        return;

    next = g_mus_cur ^ 1;

    if (g_mus_len[next] == 0) {
        /* The stream ran out and the last buffer has now played out. Report
         * finished and stay quiet; the caller decides whether to loop or move
         * on, and calls MusicStop when it does. */
        g_mus_finished = 1;
        AHI_SetSound((UWORD)MUSIC_CH_L, AHI_NOSOUND, 0, 0, g_ctrl, AHISF_IMM);
        AHI_SetSound((UWORD)MUSIC_CH_R, AHI_NOSOUND, 0, 0, g_ctrl, AHISF_IMM);
        g_mus_end_us = amiga_uclock_us();   /* stop re-entering this branch */
        return;
    }

    music_start_buffer(next);

    /* Refill the one just vacated, unless the stream has already ended - in
     * which case leaving it at length 0 is what tells the branch above that
     * the track is over. */
    if (!g_mus_drained) {
        if (music_fill(g_mus_cur ^ 1) == 0)
            g_mus_drained = 1;
    } else {
        g_mus_len[g_mus_cur ^ 1] = 0;
    }
}

void AmigaAudio_MusicSetVolume(int volume)
{
    g_mus_vol = volume;
    if (!g_mus_active || g_ctrl == NULL) return;
    AHI_SetVol((UWORD)MUSIC_CH_L, vol_to_fixed(volume), (Fixed)0,
               g_ctrl, AHISF_IMM);
    AHI_SetVol((UWORD)MUSIC_CH_R, vol_to_fixed(volume), (Fixed)0x10000,
               g_ctrl, AHISF_IMM);
}

int AmigaAudio_MusicActive(void)   { return g_mus_active; }
int AmigaAudio_MusicFinished(void) { return g_mus_finished; }

#endif /* __MORPHOS__ */
