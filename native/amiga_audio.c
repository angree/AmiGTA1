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
#include <exec/semaphores.h>
#include <dos/dostags.h>
#include <proto/dos.h>

#include "amiga_audio.h"

/* ---- AHI (2026-10-29, AmiFFE) -------------------------------------------
 *
 * The second backend: ahi.device through its DEVICE interface (CMD_WRITE on
 * an AHIRequest), which needs no library calls and so no AHI SDK - the one
 * structure is declared below exactly as devices/ahi.h has it. Every function
 * of this file keeps its contract; only the request type and five places
 * differ (open, clone, write, cancel, close). Why it exists: MorphOS's
 * audio.device emulation never replied an aborted write (the game hung in
 * WaitIO on the splash, measured in QEMU pegasos2), while AHI is MorphOS's
 * native sound system; on classic machines it also serves sound cards.
 *
 * Differences from Paula that matter:
 *  - several outstanding writes on one unit play AT THE SAME TIME (AHI mixes
 *    them on its channels); a write that must follow another names it in
 *    ahir_Link. The music queue therefore links each buffer to the previous
 *    one on the same logical channel.
 *  - AbortIO works on a playing write, so cancelling is AbortIO + WaitIO (a
 *    CMD_FLUSH would cancel every request of the opener, music included).
 *  - no Chip RAM needed, frequency in Hz (PAL clock / period), volume and
 *    position as 16.16 fixed point. */
struct AA_AHIRequest {
    struct IOStdReq ahir_Std;
    UWORD ahir_Version;
    UWORD ahir_Pad1;
    ULONG ahir_Private[2];
    ULONG ahir_Type;
    ULONG ahir_Frequency;
    LONG  ahir_Volume;
    LONG  ahir_Position;
    struct AA_AHIRequest *ahir_Link;
};
#define AA_AHIST_M8S 0UL
#define AA_PAL_CLOCK 3546895UL
/* request blocks are allocated at the larger of the two sizes */
#define AA_REQ_SIZE (sizeof(struct AA_AHIRequest) > sizeof(struct IOAudio) ? \
                     sizeof(struct AA_AHIRequest) : sizeof(struct IOAudio))
static int aa_want_ahi = 0;      /* AmigaAudio_SetBackend, before Open */
static int aa_ahi = 0;           /* the OPEN device is ahi.device */

void AmigaAudio_SetBackend(int ahi) { aa_want_ahi = ahi ? 1 : 0; }
int  AmigaAudio_IsAHI(void) { return aa_ahi; }

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
 * slack for 8 KB of Chip RAM, and each refill still mixes just one buffer.
 * Ami MoM 0.7.5: 16 (gracz: AdLib live gubi chwilami nuty - 8 x 4096 przy
 * 22 kHz to tylko 1,5 s zapasu na wczytywanie z dysku; 16 = 3 s, 64 KB Chip). */
#define AA_MUS_BUFS 16

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

/* Ami MoM 0.4.4: muzyke dolewa osobny proces o wyzszym priorytecie niz gra
 * (gracz: AdLib na zywo tnie sie przy wczytywaniu - glowna petla wtedy nie
 * wola MusicService). Wszystkie funkcje Music* biora ten semafor; proces
 * co tik (Delay(1) = 20 ms) wola MusicService. Koniec procesu: CTRL_F
 * (CTRL_C sprawdza __chkabort libnix - nie wolno go uzywac). */
static struct SignalSemaphore aa_mus_sem;
static int aa_mus_sem_ok = 0;
static struct Task *aa_thr_rodzic = NULL;
static volatile int aa_thr_zyje = 0;
static struct Task *aa_thr_task = NULL;
static void MusLock(void)
{
    if (!aa_mus_sem_ok) { InitSemaphore(&aa_mus_sem); aa_mus_sem_ok = 1; }
    ObtainSemaphore(&aa_mus_sem);
}
static void MusUnlock(void) { ReleaseSemaphore(&aa_mus_sem); }
static struct IOAudio *aa_mus_req[2][AA_MUS_BUFS];
static int   aa_mus_busy[2][AA_MUS_BUFS];

int AmigaAudio_Open(void)
{
    int i;

    if (aa_ready) return 1;

    aa_port = CreateMsgPort();
    if (aa_port == NULL) goto fail;

    aa_opener = (struct IOAudio *)AllocVec(AA_REQ_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (aa_opener == NULL) goto fail;

    aa_ahi = 0;
    if (aa_want_ahi) {
        struct AA_AHIRequest *a = (struct AA_AHIRequest *)aa_opener;
        a->ahir_Std.io_Message.mn_ReplyPort = aa_port;
        a->ahir_Std.io_Message.mn_Length    = sizeof(struct AA_AHIRequest);
        a->ahir_Version = 4;
        if (OpenDevice((STRPTR)"ahi.device", 0 /* AHI_DEFAULT_UNIT */,
                       (struct IORequest *)a, 0) != 0)
            goto fail;   /* no AHI: run silent (the caller may retry Paula) */
        aa_ahi = 1;
        aa_devopen = 1;
        for (i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
            aa_req[i] = (struct IOAudio *)AllocVec(AA_REQ_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
            if (aa_req[i] == NULL) goto fail;
            CopyMem(aa_opener, aa_req[i], AA_REQ_SIZE);
            aa_busy[i] = 0;
        }
        aa_ready = 1;
        return 1;
    }

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
        aa_req[i] = (struct IOAudio *)AllocVec(AA_REQ_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
        if (aa_req[i] == NULL) goto fail;
        CopyMem(aa_opener, aa_req[i], AA_REQ_SIZE);
        aa_req[i]->ioa_Request.io_Unit = (struct Unit *)(1UL << i);
        aa_busy[i] = 0;
    }

    aa_ready = 1;
    return 1;

fail:
    AmigaAudio_Close();
    return 0;
}

static void AudFlush(ULONG unitmask);   /* defined below, see the comment there */

void AmigaAudio_Close(void)
{
    int i;

    aa_ready = 0;

    AmigaAudio_MusicThreadStop();

    /* Stop music first: aborts channels 2 & 3 and frees their buffers. */
    AmigaAudio_MusicStop();

    /* Cancel and dequeue any outstanding write before freeing its request. */
    AudFlush(15UL);                      /* all four channels */
    for (i = 0; i < AMIGA_AUDIO_CHANNELS; i++) {
        if (aa_req[i] != NULL && aa_busy[i]) {
            WaitIO((struct IORequest *)aa_req[i]);
            aa_busy[i] = 0;
        }
    }

    if (aa_devopen && aa_ahi) {
        CloseDevice((struct IORequest *)aa_opener);
        aa_devopen = 0;
    } else if (aa_devopen) {
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
    aa_ahi = 0;
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

/* AmiFFE (2026-10-24): this file is the ReMoM port's audio layer (music
 * refill process, 16 queued buffers) with the one FFE addition below, which
 * the earlier FFE copy of this file had. */
int AmigaAudio_Stop(int ch)
{
    if (!aa_ready || ch < 0 || ch >= AMIGA_AUDIO_CHANNELS) return 0;
    if (aa_mus_active && (ch == 2 || ch == 3)) return 0;  /* held by music */
    if (!aa_busy[ch]) return 1;                           /* already silent */

    /* AbortIO then WaitIO is the documented way to cancel an audio.device
     * write; WaitIO cannot block afterwards, the device replies the aborted
     * request at once. The FFE sound layer needs a channel back NOW: to take
     * it from a lower-priority sound, for SoundStopAllSamples, and to retune
     * the engine noise while it plays. */
    /* 2026-10-29: on Paula, CMD_FLUSH of this one channel, not AbortIO -
     * audio.device ignores AbortIO on a write it has begun (see AudFlush),
     * and the WaitIO after it then never returns: that was the MorphOS hang
     * on the splash. AHI aborts a playing write properly. */
    if (aa_ahi) AbortIO((struct IORequest *)aa_req[ch]);
    else        AudFlush(1UL << ch);
    WaitIO((struct IORequest *)aa_req[ch]);
    aa_busy[ch] = 0;
    return 1;
}

/* One CMD_WRITE on channel ch (both backends). link: AHI only, the request
 * this one must follow (NULL = play now). */
static void AudWrite(struct IOAudio *io, int ch, void *data, ULONG len,
                     int period, int volume, struct IOAudio *link)
{
    if (aa_ahi) {
        struct AA_AHIRequest *a = (struct AA_AHIRequest *)io;
        a->ahir_Std.io_Command = CMD_WRITE;
        a->ahir_Std.io_Flags   = 0;
        a->ahir_Std.io_Data    = data;
        a->ahir_Std.io_Length  = len;
        a->ahir_Std.io_Offset  = 0;
        a->ahir_Type      = AA_AHIST_M8S;
        a->ahir_Frequency = AA_PAL_CLOCK / (ULONG)period;
        a->ahir_Volume    = (LONG)volume << 10;          /* 64 -> 1.0 */
        /* Paula's wiring: channels 0 and 3 left, 1 and 2 right */
        a->ahir_Position  = (ch == 0 || ch == 3) ? 0L : 0x10000L;
        a->ahir_Link      = (struct AA_AHIRequest *)link;
        BeginIO((struct IORequest *)a);
        return;
    }
    io->ioa_Request.io_Command = CMD_WRITE;
    io->ioa_Request.io_Flags   = ADIOF_PERVOL;
    io->ioa_Request.io_Unit    = (struct Unit *)(1UL << ch);
    io->ioa_Data   = (UBYTE *)data;
    io->ioa_Length = len;
    io->ioa_Period = (UWORD)period;
    io->ioa_Volume = (UWORD)volume;
    io->ioa_Cycles = 1;                  /* play once, then the channel stops */
    /* MUST be BeginIO - see the file header. */
    BeginIO((struct IORequest *)io);
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
    /* Paula wants an even byte count */
    AudWrite(io, ch, chipdata, bytes & ~1UL, period, volume, NULL);
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
/* Cancel everything queued on the given channels.
 *
 * audio.device does NOT cancel a CMD_WRITE it has already begun: AbortIO
 * leaves the request neither aborted nor replied, and the WaitIO that follows
 * never returns. Measured on 2026-09-01 with the request state logged either
 * side of the AbortIO: ln_Type 5 (NT_MESSAGE) before and after, CheckIO 0 both
 * times. That was the freeze on every music change, and the reason it got
 * likelier the faster the CPU ran - the window is exactly the time the device
 * spends playing a buffer.
 *
 * CMD_FLUSH is the documented way: it aborts every request queued on the
 * channel, the one in progress included, and replies them all. Only then is
 * WaitIO safe. Never replace this with AbortIO again. */
static void AudFlush(ULONG unitmask)
{
    struct IOAudio flush;
    if (!aa_devopen || aa_opener == NULL) return;
    if (aa_ahi) {
        /* AHI: abort what is outstanding on these channels; the callers
         * WaitIO each busy request afterwards, exactly as after a flush */
        int ch, mc, p;
        for (ch = 0; ch < AMIGA_AUDIO_CHANNELS; ch++)
            if ((unitmask & (1UL << ch)) && aa_req[ch] != NULL && aa_busy[ch])
                AbortIO((struct IORequest *)aa_req[ch]);
        for (mc = 0; mc < 2; mc++)
            if (unitmask & (1UL << aa_mus_chan[mc]))
                for (p = 0; p < AA_MUS_BUFS; p++)
                    if (aa_mus_req[mc][p] != NULL && aa_mus_busy[mc][p])
                        AbortIO((struct IORequest *)aa_mus_req[mc][p]);
        return;
    }
    flush = *aa_opener;
    flush.ioa_Request.io_Command = CMD_FLUSH;
    flush.ioa_Request.io_Flags   = 0;          /* reply through the port */
    flush.ioa_Request.io_Unit    = (struct Unit *)unitmask;
    DoIO((struct IORequest *)&flush);
}

static void MusWrite(int mc, int ping)
{
    struct IOAudio *io = aa_mus_req[mc][ping];
    struct IOAudio *link = NULL;
    int prev = (ping + AA_MUS_BUFS - 1) % AA_MUS_BUFS;
    if (io == NULL || aa_mus_len[ping] < 2) return;
    /* AHI plays outstanding writes together: chain this buffer behind the
     * previous one of the same channel while that one is still queued */
    if (aa_ahi && aa_mus_busy[mc][prev]) link = aa_mus_req[mc][prev];
    AudWrite(io, aa_mus_chan[mc], aa_mus_buf[ping],
             (ULONG)(aa_mus_len[ping] & ~1UL), aa_mus_period, aa_mus_vol, link);
    aa_mus_busy[mc][ping] = 1;
}

static void MusFreeReqs(void)
{
    int mc, p;
    /* Flush both music channels FIRST - see AudFlush. Waiting on a write the
     * device is still playing is what froze the game on every tune change. */
    AudFlush((1UL << aa_mus_chan[0]) | (1UL << aa_mus_chan[1]));
    for (mc = 0; mc < 2; mc++) {
        for (p = 0; p < AA_MUS_BUFS; p++) {
            if (aa_mus_req[mc][p] == NULL) continue;
            if (aa_mus_busy[mc][p]) {
                WaitIO((struct IORequest *)aa_mus_req[mc][p]);
                aa_mus_busy[mc][p] = 0;
            }
            FreeVec(aa_mus_req[mc][p]);
            aa_mus_req[mc][p] = NULL;
        }
    }
}

static void MusStopL(void)
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

static int MusStartL(int period, int chunk_samples,
                          int (*refill)(void *, signed char *, int), void *ud)
{
    int mc, p, b, primed;
    if (!aa_ready || refill == NULL) return 0;

    MusStopL();

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

    /* Reclaim channels 2 & 3 from any in-flight SFX write. Flush, then wait:
     * an SFX the device has already begun ignores AbortIO exactly as a music
     * write does. */
    AudFlush((1UL << aa_mus_chan[0]) | (1UL << aa_mus_chan[1]));
    for (mc = 0; mc < 2; mc++) {
        int ch = aa_mus_chan[mc];
        if (aa_req[ch] != NULL && aa_busy[ch]) {
            WaitIO((struct IORequest *)aa_req[ch]);
            aa_busy[ch] = 0;
        }
    }

    /* One byte per 8-bit sample; the two channels share these buffers. */
    for (b = 0; b < AA_MUS_BUFS; b++) {
        aa_mus_buf[b] = (signed char *)AllocVec((ULONG)chunk_samples, MEMF_CHIP);
        if (aa_mus_buf[b] == NULL) { MusStopL(); return 0; }
        aa_mus_len[b] = 0;
    }

    /* One request per channel per buffer, cloned from the opener so they
     * carry ioa_AllocKey. */
    for (mc = 0; mc < 2; mc++) {
        for (p = 0; p < AA_MUS_BUFS; p++) {
            struct IOAudio *io = (struct IOAudio *)AllocVec(AA_REQ_SIZE,
                                                            MEMF_PUBLIC | MEMF_CLEAR);
            if (io == NULL) { MusStopL(); return 0; }
            CopyMem(aa_opener, io, AA_REQ_SIZE);
            if (!aa_ahi) io->ioa_Request.io_Unit = (struct Unit *)(1UL << aa_mus_chan[mc]);
            aa_mus_req[mc][p] = io;
            aa_mus_busy[mc][p] = 0;
        }
    }

    /* Fill the whole queue before starting DMA: the first seconds of a
     * tune are exactly when the game is busiest changing state. */
    primed = 0;
    /* Ami MoM 0.4.4: przy dzialajacym procesie muzyki tylko 2 bufory na start
     * (reszte dolewa proces) - zmiana utworu nie zatrzymuje gry na dlugo */
    for (b = 0; b < (aa_thr_zyje ? 2 : AA_MUS_BUFS); b++) {
        int n = refill(ud, aa_mus_buf[b], chunk_samples);
        if (n <= 0) { aa_mus_len[b] = 0; aa_mus_ended = 1; break; }
        aa_mus_len[b] = n & ~1;
        primed++;
    }
    if (primed == 0) { MusStopL(); return 0; }   /* nothing to play */

    aa_mus_active = 1;
    aa_mus_next = primed % AA_MUS_BUFS;
    for (b = 0; b < primed; b++)
        for (mc = 0; mc < 2; mc++) MusWrite(mc, b);
    return 1;
}

static unsigned long aa_mus_glod = 0;   /* 0.4.4: ile razy kolejka byla pusta (slychac przerwe) */
unsigned long AmigaAudio_MusicUnderruns(void) { return aa_mus_glod; }
int AmigaAudio_MusicThreadRunning(void) { return aa_thr_zyje; }   /* 0.7.5 */

static void MusServiceL(void)
{
    int pass, mc;
    if (!aa_mus_active) return;
    if (!aa_mus_ended) {
        int b2, gra = 0;
        for (b2 = 0; b2 < AA_MUS_BUFS && !gra; b2++)
            for (mc = 0; mc < 2; mc++)
                if (aa_mus_busy[mc][b2] && CheckIO((struct IORequest *)aa_mus_req[mc][b2]) == NULL) gra = 1;
        if (!gra) aa_mus_glod++;
    }

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

static int MusFinishedL(void)
{
    int mc, p;
    if (!aa_mus_active) return 1;
    if (!aa_mus_ended)  return 0;
    /* Reap completed writes so a drained stream is reported finished even if
     * MusicService did not run this frame. */
    for (mc = 0; mc < 2; mc++) {
        for (p = 0; p < AA_MUS_BUFS; p++) {
            if (aa_mus_busy[mc][p] &&
                CheckIO((struct IORequest *)aa_mus_req[mc][p]) != NULL) {
                WaitIO((struct IORequest *)aa_mus_req[mc][p]);
                aa_mus_busy[mc][p] = 0;
            }
        }
    }
    for (mc = 0; mc < 2; mc++)
        for (p = 0; p < AA_MUS_BUFS; p++)
            if (aa_mus_busy[mc][p]) return 0;
    return 1;
}

/* ---- publiczne wejscia pod semaforem (Ami MoM 0.4.4) ---- */
void AmigaAudio_MusicStop(void) { MusLock(); MusStopL(); MusUnlock(); }

int AmigaAudio_MusicStart(int period, int chunk_samples,
                          int (*refill)(void *, signed char *, int), void *ud)
{
    int r;
    MusLock(); r = MusStartL(period, chunk_samples, refill, ud); MusUnlock();
    return r;
}

void AmigaAudio_MusicService(void) { MusLock(); MusServiceL(); MusUnlock(); }

int AmigaAudio_MusicFinished(void)
{
    int r;
    MusLock(); r = MusFinishedL(); MusUnlock();
    return r;
}

static void MusThread(void)
{
    while (!(SetSignal(0, 0) & SIGBREAKF_CTRL_F)) {
        MusLock();
        MusServiceL();
        MusUnlock();
        Delay(1);
    }
    /* Forbid trwa do konca procesu - rodzic nie zwolni kodu przed nami */
    Forbid();
    aa_thr_zyje = 0;
    Signal(aa_thr_rodzic, SIGBREAKF_CTRL_F);
}

int AmigaAudio_MusicThreadStart(int pri_wzgledem_gry)
{
    struct Process *pr;
    if (aa_thr_zyje) return 1;
    if (!aa_mus_sem_ok) { InitSemaphore(&aa_mus_sem); aa_mus_sem_ok = 1; }
    aa_thr_rodzic = FindTask(NULL);
    SetSignal(0, SIGBREAKF_CTRL_F);
    aa_thr_zyje = 1;
    pr = CreateNewProcTags(NP_Entry, (ULONG)MusThread,
                           NP_Name, (ULONG)"AmiFFE music",
                           NP_Priority, (LONG)(aa_thr_rodzic->tc_Node.ln_Pri + pri_wzgledem_gry),
                           NP_StackSize, 32768UL,
                           TAG_DONE);
    if (pr == NULL) { aa_thr_zyje = 0; return 0; }
    aa_thr_task = (struct Task *)pr;
    return 1;
}

void AmigaAudio_MusicThreadStop(void)
{
    if (!aa_thr_zyje) return;
    Signal(aa_thr_task, SIGBREAKF_CTRL_F);
    while (aa_thr_zyje) Wait(SIGBREAKF_CTRL_F);
    SetSignal(0, SIGBREAKF_CTRL_F);
}

/* ---- GTA: THE ENGINE NOTE (Paula only) ---------------------------------
 * Carried over from the GTA port's own audio file when the AmiFFE version
 * (AHI backend, CMD_FLUSH instead of AbortIO on Paula) replaced it. AHI's
 * device interface has neither a loop-for-ever write nor ADCMD_PERVOL, so
 * under AHI (MorphOS) the engine note is simply not played. */
int AmigaAudio_PlayLoop(int ch, void *chipdata, unsigned long bytes,
                        int period, int volume)
{
    struct IOAudio *io;

    if (!aa_ready || ch < 0 || ch >= AMIGA_AUDIO_CHANNELS) return 0;
    if (aa_ahi) return 0;    /* no loop-for-ever write in AHI's device interface */
    if (aa_mus_active && (ch == 2 || ch == 3)) return 0;
    if (chipdata == NULL || bytes < 2) return 0;
    if (!AmigaAudio_ChannelIdle(ch)) return 0;

    if (bytes > AA_MAX_BYTES) bytes = AA_MAX_BYTES;
    if (period < 124)   period = 124;
    if (period > 65535) period = 65535;
    if (volume < 0)  volume = 0;
    if (volume > 64) volume = 64;

    io = aa_req[ch];
    io->ioa_Request.io_Command = CMD_WRITE;
    io->ioa_Request.io_Flags   = ADIOF_PERVOL;
    io->ioa_Request.io_Unit    = (struct Unit *)(1UL << ch);
    io->ioa_Data   = (UBYTE *)chipdata;
    io->ioa_Length = bytes & ~1UL;
    io->ioa_Period = (UWORD)period;
    io->ioa_Volume = (UWORD)volume;
    io->ioa_Cycles = 0;                  /* 0 = for ever */

    BeginIO((struct IORequest *)io);
    aa_busy[ch] = 1;
    return 1;
}

/* CHANGE THE PITCH OF WHAT IS ALREADY PLAYING, without restarting it.
 *
 * ADCMD_PERVOL is the one audio.device command that reaches a channel while
 * a write is outstanding; it writes AUDxPER and AUDxVOL and returns. That is
 * what makes an engine rev instead of stutter: restarting the sample every
 * time the speed changed would click sixty times a second.
 *
 * It is sent with DoIO rather than BeginIO deliberately - it completes
 * immediately and queuing it on the channel's own request would clobber the
 * CMD_WRITE that is playing. It gets a request of its own. */
void AmigaAudio_SetPeriod(int ch, int period, int volume)
{
    struct IOAudio io;

    if (!aa_ready || ch < 0 || ch >= AMIGA_AUDIO_CHANNELS) return;
    if (aa_ahi) return;
    if (period < 124)   period = 124;
    if (period > 65535) period = 65535;
    if (volume < 0)  volume = 0;
    if (volume > 64) volume = 64;

    io = *aa_req[ch];                    /* the device and unit fields */
    io.ioa_Request.io_Command = ADCMD_PERVOL;
    io.ioa_Request.io_Flags   = 0;
    io.ioa_Request.io_Unit    = (struct Unit *)(1UL << ch);
    io.ioa_Period = (UWORD)period;
    io.ioa_Volume = (UWORD)volume;
    DoIO((struct IORequest *)&io);
}

