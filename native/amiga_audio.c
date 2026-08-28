/* Paula audio for OpenTTD on AmigaOS 68k - OS layer implementation.
 *
 * Plain C, the only file in the sound path that includes Amiga headers
 * (they collide with OpenTTD's C++ - same rule as amiga_gfx.c).
 *
 * Every pattern here was proven standalone in native/paulatest.c:
 *  - OpenDevice("audio.device") with io_Length != 0 performs ADCMD_ALLOCATE;
 *    mask 15 = all four channels, ADIOF_NOWAIT = fail fast instead of
 *    blocking if another program holds Paula.
 *  - AUDIO.DEVICE RULE (cost a day of silence): requests MUST be sent with
 *    BeginIO(). SendIO() and DoIO() clear io_Flags, which wipes ADIOF_PERVOL;
 *    the device then keeps the channel's previous volume - 0 right after
 *    allocation - and every write "succeeds" inaudibly with io_Error = 0.
 *  - Completion is polled with CheckIO() and the replied request dequeued
 *    with WaitIO() (which does not block once CheckIO returned non-NULL).
 *    The game is built thread_none: nothing here ever blocks.
 *  - Sample data must be Chip RAM (Paula DMA reads nothing else),
 *    word-aligned (AllocVec guarantees that), even length.
 *
 * Build by hand, same ABI as the rest of the game, and at -O0 - this
 * toolchain has a record of miscompiling read-after-call patterns at -O1:
 *   m68k-amigaos-gcc -O0 -m68040 -msoft-float -noixemul \
 *       -c amiga_audio.c -o amiga_audio.o
 * The object is linked via LIBS in objs/release/Makefile, like amiga_gfx.o.
 */

#include <proto/exec.h>
#include <clib/alib_protos.h>   /* BeginIO - amiga.lib stub (-lamiga) */
#include <exec/memory.h>
#include <exec/io.h>
#include <devices/audio.h>

#include "amiga_audio.h"

/* Paula length register counts 16-bit words: max 65535 words = 131070 bytes. */
#define AA_MAX_BYTES 131070UL

static struct MsgPort *aa_port = NULL;
/* Opener request: owns the four-channel allocation (and the AllocKey). */
static struct IOAudio *aa_opener = NULL;
/* One write request per channel, cloned from the opener. */
static struct IOAudio *aa_req[AMIGA_AUDIO_CHANNELS];
static int aa_busy[AMIGA_AUDIO_CHANNELS];   /* CMD_WRITE outstanding? */
static int aa_devopen = 0;
static int aa_ready = 0;

/* The only allocation we accept: all four channels at once. */
static UBYTE aa_allocmap[] = { 15 };

/* ---- streaming music state (Paula channels 2 & 3) ---------------------- */
#define AA_MUS_MAX_CHUNK 16384          /* 8-bit samples per buffer ceiling */
/* Depth of the queue. Two buffers (the original ping-pong) only held about
 * 90 ms, and a single slow geoscape frame drained it; the mixer then had
 * nothing to hand Paula and the music chopped. Eight buffers give ~0.7 s of
 * slack for 8 KB of Chip RAM, and each refill still mixes just one buffer. */
#define AA_MUS_BUFS 8

static int   aa_mus_active = 0;
static int   aa_mus_ended  = 0;         /* refill returned end-of-stream */
static int   aa_mus_period = 400;
static int   aa_mus_vol    = 48;        /* 0..64 */
static int   aa_mus_chunk  = 0;         /* samples per buffer */
static signed char *aa_mus_buf[AA_MUS_BUFS];        /* Chip, shared by both channels */
static int   aa_mus_len[AA_MUS_BUFS];   /* valid bytes per buffer (even) */
static int   aa_mus_next = 0;           /* next buffer to hand to the mixer */
static int   (*aa_mus_refill)(void *, signed char *, int) = NULL;
static void *aa_mus_ud = NULL;

/* Logical music channel 0 -> Paula ch3 (LEFT), 1 -> ch2 (RIGHT); two
 * one request per queued buffer, so audio.device chains them gaplessly. */
static const int aa_mus_chan[2] = { 3, 2 };
static struct IOAudio *aa_mus_req[2][AA_MUS_BUFS];
static int   aa_mus_busy[2][AA_MUS_BUFS];

int AmigaAudio_Open(void)
{
    int i;

    if (aa_ready) return 1;

    aa_port = CreateMsgPort();
    if (aa_port == NULL) goto fail;

    aa_opener = (struct IOAudio *)AllocVec(sizeof(struct IOAudio),
                                           MEMF_PUBLIC | MEMF_CLEAR);
    if (aa_opener == NULL) goto fail;

    aa_opener->ioa_Request.io_Message.mn_ReplyPort   = aa_port;
    aa_opener->ioa_Request.io_Message.mn_Node.ln_Pri = 0;  /* precedence */
    aa_opener->ioa_Request.io_Message.mn_Length      = sizeof(struct IOAudio);
    /* io_Length != 0 makes OpenDevice do an ADCMD_ALLOCATE with this map;
     * ADIOF_NOWAIT = fail instead of blocking if channels are taken. */
    aa_opener->ioa_Request.io_Flags = ADIOF_NOWAIT;
    aa_opener->ioa_Data   = aa_allocmap;
    aa_opener->ioa_Length = sizeof(aa_allocmap);

    if (OpenDevice((STRPTR)AUDIONAME, 0,
                   (struct IORequest *)aa_opener, 0) != 0) {
        goto fail;   /* channels busy (-11) or no device: run silent */
    }
    aa_devopen = 1;

    /* Clone the opener per channel: copies io_Device, the reply port and,
     * critically, ioa_AllocKey - the proof of ownership every later command
     * must carry. io_Unit is narrowed to the one channel per request. */
    for (i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
        aa_req[i] = (struct IOAudio *)AllocVec(sizeof(struct IOAudio),
                                               MEMF_PUBLIC | MEMF_CLEAR);
        if (aa_req[i] == NULL) goto fail;
        *aa_req[i] = *aa_opener;
        aa_req[i]->ioa_Request.io_Unit = (struct Unit *)(1UL << i);
        aa_busy[i] = 0;
    }

    aa_ready = 1;
    return 1;

fail:
    AmigaAudio_Close();
    return 0;
}

void AmigaAudio_Close(void)
{
    int i;

    aa_ready = 0;

    /* Stop music first: aborts channels 2 & 3 and frees their buffers. */
    AmigaAudio_MusicStop();

    /* Abort and dequeue any outstanding write before freeing its request. */
    for (i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
        if (aa_req[i] != NULL && aa_busy[i]) {
            AbortIO((struct IORequest *)aa_req[i]);
            WaitIO((struct IORequest *)aa_req[i]);
            aa_busy[i] = 0;
        }
    }

    if (aa_devopen) {
        /* Explicit ADCMD_FREE (resets + frees our channels), then
         * CloseDevice - belt and braces, exactly as in paulatest.c. */
        aa_opener->ioa_Request.io_Command = ADCMD_FREE;
        aa_opener->ioa_Request.io_Flags   = 0;  /* reply via port */
        aa_opener->ioa_Request.io_Unit    = (struct Unit *)15;
        BeginIO((struct IORequest *)aa_opener);  /* never DoIO/SendIO */
        WaitIO((struct IORequest *)aa_opener);
        CloseDevice((struct IORequest *)aa_opener);
        aa_devopen = 0;
    }

    for (i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
        if (aa_req[i] != NULL) { FreeVec(aa_req[i]); aa_req[i] = NULL; }
    }
    if (aa_opener != NULL) { FreeVec(aa_opener); aa_opener = NULL; }
    if (aa_port != NULL) { DeleteMsgPort(aa_port); aa_port = NULL; }
}

void *AmigaAudio_AllocSample(unsigned long bytes)
{
    if (bytes == 0) return NULL;
    return AllocVec(bytes, MEMF_CHIP);
}

void AmigaAudio_FreeSample(void *p)
{
    if (p != NULL) FreeVec(p);
}

int AmigaAudio_ChannelIdle(int ch)
{
    if (!aa_ready || ch < 0 || ch >= AMIGA_AUDIO_CHANNELS) return 0;
    if (!aa_busy[ch]) return 1;
    if (CheckIO((struct IORequest *)aa_req[ch]) != NULL) {
        /* Replied: WaitIO just dequeues, it cannot block here. */
        WaitIO((struct IORequest *)aa_req[ch]);
        aa_busy[ch] = 0;
        return 1;
    }
    return 0;
}

int AmigaAudio_Play(int ch, void *chipdata, unsigned long bytes,
                    int period, int volume)
{
    struct IOAudio *io;

    if (!aa_ready || ch < 0 || ch >= AMIGA_AUDIO_CHANNELS) return 0;
    if (aa_mus_active && (ch == 2 || ch == 3)) return 0; /* held by music */
    if (chipdata == NULL || bytes < 2) return 0;
    if (!AmigaAudio_ChannelIdle(ch)) return 0;

    if (bytes > AA_MAX_BYTES) bytes = AA_MAX_BYTES;
    if (period < 124)   period = 124;    /* PAL DMA floor (~28.6 kHz) */
    if (period > 65535) period = 65535;
    if (volume < 0)  volume = 0;
    if (volume > 64) volume = 64;

    io = aa_req[ch];
    io->ioa_Request.io_Command = CMD_WRITE;
    io->ioa_Request.io_Flags   = ADIOF_PERVOL;
    io->ioa_Request.io_Unit    = (struct Unit *)(1UL << ch);
    io->ioa_Data   = (UBYTE *)chipdata;
    io->ioa_Length = bytes & ~1UL;       /* Paula wants an even byte count */
    io->ioa_Period = (UWORD)period;
    io->ioa_Volume = (UWORD)volume;
    io->ioa_Cycles = 1;                  /* play once, then the channel stops */

    /* MUST be BeginIO - see the file header. */
    BeginIO((struct IORequest *)io);
    aa_busy[ch] = 1;
    return 1;
}

/* ======================================================================== */
/* Streaming music: identical mono stream on ch3 (LEFT) and ch2 (RIGHT),    */
/* double-buffered so audio.device chains the next write before the current */
/* one ends. Serviced once per frame from the video main loop.              */
/* ======================================================================== */

/* Queue one CMD_WRITE of buffer 'ping' on logical music channel 'mc'. It is
 * sent while the other ping is still playing, so the device plays them
 * back-to-back with no CPU gap. */
static void MusWrite(int mc, int ping)
{
    struct IOAudio *io = aa_mus_req[mc][ping];
    if (io == NULL || aa_mus_len[ping] < 2) return;
    io->ioa_Request.io_Command = CMD_WRITE;
    io->ioa_Request.io_Flags   = ADIOF_PERVOL;   /* see file header: never lose this */
    io->ioa_Request.io_Unit    = (struct Unit *)(1UL << aa_mus_chan[mc]);
    io->ioa_Data   = (UBYTE *)aa_mus_buf[ping];
    io->ioa_Length = (ULONG)(aa_mus_len[ping] & ~1UL);
    io->ioa_Period = (UWORD)aa_mus_period;
    io->ioa_Volume = (UWORD)aa_mus_vol;
    io->ioa_Cycles = 1;
    BeginIO((struct IORequest *)io);
    aa_mus_busy[mc][ping] = 1;
}

static void MusFreeReqs(void)
{
    int mc, p;
    for (mc = 0; mc < 2; mc++) {
        for (p = 0; p < AA_MUS_BUFS; p++) {
            if (aa_mus_req[mc][p] == NULL) continue;
            if (aa_mus_busy[mc][p]) {
                AbortIO((struct IORequest *)aa_mus_req[mc][p]);
                WaitIO((struct IORequest *)aa_mus_req[mc][p]);
                aa_mus_busy[mc][p] = 0;
            }
            FreeVec(aa_mus_req[mc][p]);
            aa_mus_req[mc][p] = NULL;
        }
    }
}

void AmigaAudio_MusicStop(void)
{
    int b;
    if (aa_mus_req[0][0] != NULL || aa_mus_active) MusFreeReqs();
    for (b = 0; b < AA_MUS_BUFS; b++) {
        if (aa_mus_buf[b] != NULL) { FreeVec(aa_mus_buf[b]); aa_mus_buf[b] = NULL; }
        aa_mus_len[b] = 0;
    }
    aa_mus_next = 0;
    aa_mus_active = 0;
    aa_mus_ended  = 0;
    aa_mus_refill = NULL;
    aa_mus_ud     = NULL;
}

int AmigaAudio_MusicStart(int period, int chunk_samples,
                          int (*refill)(void *, signed char *, int), void *ud)
{
    int mc, p, b, primed;
    if (!aa_ready || refill == NULL) return 0;

    AmigaAudio_MusicStop();

    if (chunk_samples < 512) chunk_samples = 512;
    if (chunk_samples > AA_MUS_MAX_CHUNK) chunk_samples = AA_MUS_MAX_CHUNK;
    chunk_samples &= ~1;
    aa_mus_chunk  = chunk_samples;
    if (period < 124)   period = 124;
    if (period > 65535) period = 65535;
    aa_mus_period = period;
    aa_mus_refill = refill;
    aa_mus_ud     = ud;
    aa_mus_ended  = 0;

    /* Reclaim channels 2 & 3 from any in-flight SFX write. */
    for (mc = 0; mc < 2; mc++) {
        int ch = aa_mus_chan[mc];
        if (aa_req[ch] != NULL && aa_busy[ch]) {
            AbortIO((struct IORequest *)aa_req[ch]);
            WaitIO((struct IORequest *)aa_req[ch]);
            aa_busy[ch] = 0;
        }
    }

    /* One byte per 8-bit sample; the two channels share these buffers. */
    for (b = 0; b < AA_MUS_BUFS; b++) {
        aa_mus_buf[b] = (signed char *)AllocVec((ULONG)chunk_samples, MEMF_CHIP);
        if (aa_mus_buf[b] == NULL) { AmigaAudio_MusicStop(); return 0; }
        aa_mus_len[b] = 0;
    }

    /* One request per channel per buffer, cloned from the opener so they
     * carry ioa_AllocKey. */
    for (mc = 0; mc < 2; mc++) {
        for (p = 0; p < AA_MUS_BUFS; p++) {
            struct IOAudio *io = (struct IOAudio *)AllocVec(sizeof(struct IOAudio),
                                                            MEMF_PUBLIC | MEMF_CLEAR);
            if (io == NULL) { AmigaAudio_MusicStop(); return 0; }
            *io = *aa_opener;
            io->ioa_Request.io_Unit = (struct Unit *)(1UL << aa_mus_chan[mc]);
            aa_mus_req[mc][p] = io;
            aa_mus_busy[mc][p] = 0;
        }
    }

    /* Fill the whole queue before starting DMA: the first seconds of a
     * tune are exactly when the game is busiest changing state. */
    primed = 0;
    for (b = 0; b < AA_MUS_BUFS; b++) {
        int n = refill(ud, aa_mus_buf[b], chunk_samples);
        if (n <= 0) { aa_mus_len[b] = 0; aa_mus_ended = 1; break; }
        aa_mus_len[b] = n & ~1;
        primed++;
    }
    if (primed == 0) { AmigaAudio_MusicStop(); return 0; }   /* nothing to play */

    aa_mus_active = 1;
    aa_mus_next = primed % AA_MUS_BUFS;
    for (b = 0; b < primed; b++)
        for (mc = 0; mc < 2; mc++) MusWrite(mc, b);
    return 1;
}

void AmigaAudio_MusicService(void)
{
    int pass, mc;
    if (!aa_mus_active) return;

    /* Reap whatever finished, then refill in queue order. At most a few
     * buffers per call: mixing is expensive and doing the whole ring in one
     * frame would stall the display worse than the starvation it fixes. */
    for (pass = 0; pass < AA_MUS_BUFS; pass++) {
        int b = aa_mus_next, both_idle = 1;
        for (mc = 0; mc < 2; mc++) {
            if (!aa_mus_busy[mc][b]) continue;
            if (CheckIO((struct IORequest *)aa_mus_req[mc][b]) != NULL) {
                WaitIO((struct IORequest *)aa_mus_req[mc][b]);
                aa_mus_busy[mc][b] = 0;
            } else {
                both_idle = 0;
            }
        }
        if (!both_idle) break;          /* still playing: the queue is full enough */
        if (aa_mus_ended) { aa_mus_len[b] = 0; break; }

        {
            int n = aa_mus_refill(aa_mus_ud, aa_mus_buf[b], aa_mus_chunk);
            if (n <= 0) { aa_mus_ended = 1; aa_mus_len[b] = 0; break; }
            aa_mus_len[b] = n & ~1;
            for (mc = 0; mc < 2; mc++) MusWrite(mc, b);
        }
        aa_mus_next = (b + 1) % AA_MUS_BUFS;
        if (pass >= 2) break;           /* spread the work over frames */
    }
}

void AmigaAudio_MusicSetVolume(int volume)
{
    if (volume < 0)  volume = 0;
    if (volume > 64) volume = 64;
    aa_mus_vol = volume;
}

int AmigaAudio_MusicActive(void) { return aa_mus_active; }

int AmigaAudio_MusicFinished(void)
{
    int mc, p;
    if (!aa_mus_active) return 1;
    if (!aa_mus_ended)  return 0;
    /* Reap completed writes so a drained stream is reported finished even if
     * MusicService did not run this frame. */
    for (mc = 0; mc < 2; mc++) {
        for (p = 0; p < 2; p++) {
            if (aa_mus_busy[mc][p] &&
                CheckIO((struct IORequest *)aa_mus_req[mc][p]) != NULL) {
                WaitIO((struct IORequest *)aa_mus_req[mc][p]);
                aa_mus_busy[mc][p] = 0;
            }
        }
    }
    for (mc = 0; mc < 2; mc++)
        for (p = 0; p < 2; p++)
            if (aa_mus_busy[mc][p]) return 0;
    return 1;
}
