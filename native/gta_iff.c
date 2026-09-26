/* The music extractor - see gta_iff.h for what it writes and why.
 *
 * Portable C89, stdio only, no floats. Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gta_iff.h"

/* ---- little-endian in (WAV), big-endian out (IFF) ----------------------- */

static unsigned long rd32le(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned int rd16le(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static void wr32be(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8) & 0xff);
    p[3] = (unsigned char)(v & 0xff);
}

static void wr16be(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)(v & 0xff);
}

/* ---- opening the source -------------------------------------------------
 *
 * A RIFF/WAVE walk rather than an assumption about where `data` is: the 2002
 * release's tracks carry a LIST/INFO chunk between `fmt ` and `data`, so a
 * reader that trusted the usual 44-byte header would read the credits as
 * music. Odd-sized chunks are followed by a pad byte - forgetting that puts
 * every later chunk one byte out.
 */
static int wav_open(gta_iff *j, const char *src)
{
    unsigned char hdr[12], ch[8], fmt[40];
    int have_fmt = 0;

    j->in = fopen(src, "rb");
    if (!j->in)
        return 1;
    if (fread(hdr, 1, 12, j->in) != 12 || memcmp(hdr, "RIFF", 4) != 0
        || memcmp(hdr + 8, "WAVE", 4) != 0)
        return 1;

    while (fread(ch, 1, 8, j->in) == 8) {
        unsigned long clen = rd32le(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            unsigned long want = clen < (unsigned long)sizeof fmt
                               ? clen : (unsigned long)sizeof fmt;
            if (fread(fmt, 1, (size_t)want, j->in) != (size_t)want)
                return 1;
            if (rd16le(fmt) != 1)                    /* 1 = plain PCM */
                return 1;
            j->channels = (int)rd16le(fmt + 2);
            j->rate     = (int)rd32le(fmt + 4);
            j->bits     = (int)rd16le(fmt + 14);
            if (clen > want)
                fseek(j->in, (long)(clen - want), SEEK_CUR);
            if (clen & 1UL)
                fseek(j->in, 1L, SEEK_CUR);
            have_fmt = 1;
        } else if (memcmp(ch, "data", 4) == 0) {
            if (!have_fmt)
                return 1;
            j->data_total = j->data_left = clen;
            return 0;
        } else {
            fseek(j->in, (long)(clen + (clen & 1UL)), SEEK_CUR);
        }
    }
    return 1;
}

/* ---- the header ---------------------------------------------------------
 *
 * VHDR is twenty bytes and every field of it matters to a player that is not
 * ours: oneShotHiSamples is the length, samplesPerSec the rate, ctOctave 1
 * (this is not a multi-octave instrument), sCompression 0 (none) and volume
 * the 16.16 fixed-point 1.0 = 0x10000. A VHDR with volume 0 loads fine and
 * plays silence, which is the mistake most worth not making here.
 */
static int write_header(gta_iff *j, const char *src)
{
    unsigned char h[64];
    const char *base, *p;
    char anno[96];
    unsigned long annolen, annopad, formsize;
    int n;

    base = src;
    for (p = src; *p; p++)
        if (*p == '/' || *p == ':' || *p == '\\')
            base = p + 1;

    /* snprintf, NEVER sprintf: on this libc sprintf shifts its arguments and
     * would write a nonsense credit in silence (CLAUDE.md, defect 4). */
    snprintf(anno, sizeof anno, "AmiGTA 68k - extracted from %s", base);
    annolen = (unsigned long)strlen(anno);
    annopad = annolen & 1UL;

    j->body_pad = j->out_total & 1UL;
    formsize = 4UL                                   /* "8SVX" */
             + 8UL + 20UL                            /* VHDR */
             + 8UL + annolen + annopad               /* ANNO */
             + 8UL + j->out_total + j->body_pad;     /* BODY */

    n = 0;
    memcpy(h + n, "FORM", 4);              n += 4;
    wr32be(h + n, formsize);               n += 4;
    memcpy(h + n, "8SVX", 4);              n += 4;
    memcpy(h + n, "VHDR", 4);              n += 4;
    wr32be(h + n, 20UL);                   n += 4;
    wr32be(h + n, j->out_total);           n += 4;   /* oneShotHiSamples */
    wr32be(h + n, 0UL);                    n += 4;   /* repeatHiSamples  */
    wr32be(h + n, 0UL);                    n += 4;   /* samplesPerHiCycle */
    wr16be(h + n, (unsigned int)j->out_rate); n += 2;
    h[n++] = 1;                                      /* ctOctave */
    h[n++] = 0;                                      /* sCompression: none */
    wr32be(h + n, 0x10000UL);              n += 4;   /* volume, 1.0 */

    if (fwrite(h, 1, (size_t)n, j->out) != (size_t)n)
        return 1;

    memcpy(h, "ANNO", 4);
    wr32be(h + 4, annolen);
    if (fwrite(h, 1, 8, j->out) != 8)
        return 1;
    if (fwrite(anno, 1, (size_t)annolen, j->out) != (size_t)annolen)
        return 1;
    if (annopad) {
        h[0] = 0;
        if (fwrite(h, 1, 1, j->out) != 1)
            return 1;
    }

    memcpy(h, "BODY", 4);
    wr32be(h + 4, j->out_total);
    return fwrite(h, 1, 8, j->out) == 8 ? 0 : 1;
}

/* ---- the job ------------------------------------------------------------ */

int gta_iff_open(gta_iff *j, const char *src, const char *dst, int out_rate)
{
    unsigned long frames, q, r;
    int bpf;

    memset(j, 0, sizeof *j);
    j->out_rate = out_rate > 0 ? out_rate : GTA_IFF_RATE;
    strncpy(j->dst, dst, sizeof j->dst - 1);

    if (wav_open(j, src)) {
        gta_iff_abort(j);
        return 1;
    }
    if ((j->channels != 1 && j->channels != 2) ||
        (j->bits != 8 && j->bits != 16) ||
        j->rate < 1000 || j->rate > 96000) {
        gta_iff_abort(j);
        return 1;
    }

    bpf = j->channels * (j->bits / 8);
    frames = j->data_total / (unsigned long)bpf;

    /* HOW MANY SAMPLES COME OUT, without a 64-bit multiply and without a
     * float. frames * out_rate / rate overflows 32 bits at about three
     * minutes of 22 kHz, so it is split: the quotient carries the whole
     * seconds and the remainder is always smaller than the rate. */
    q = frames / (unsigned long)j->rate;
    r = frames % (unsigned long)j->rate;
    j->out_total = q * (unsigned long)j->out_rate
                 + (r * (unsigned long)j->out_rate) / (unsigned long)j->rate;
    if (j->out_total == 0) {
        gta_iff_abort(j);
        return 1;
    }

    /* 16.16 source frames per output sample. 1<<16 exactly when the rates
     * match, which is the case for every track of the 2002 release. */
    j->step = ((unsigned long)j->rate << 16) / (unsigned long)j->out_rate;
    if (j->step == 0)
        j->step = 1;

    j->out = fopen(dst, "wb");
    if (!j->out) {
        gta_iff_abort(j);
        return 1;
    }
    if (write_header(j, src)) {
        gta_iff_abort(j);
        return 1;
    }
    return 0;
}

static int flush_out(gta_iff *j)
{
    if (j->n_obuf > 0) {
        if ((int)fwrite(j->obuf, 1, (size_t)j->n_obuf, j->out) != j->n_obuf)
            return 1;
        j->n_obuf = 0;
    }
    return 0;
}

/* One output sample. 16 bits down to 8 with rounding, then clamped: the
 * unrounded (v >> 8) biases every sample downwards by half a step, which on
 * quiet music is an audible hiss floor rather than a rounding detail. */
static void emit(gta_iff *j, int v)
{
    int s = (v + 128) >> 8;
    if (s > 127) s = 127;
    if (s < -128) s = -128;
    j->obuf[j->n_obuf++] = (signed char)s;
    j->out_done++;
    if (j->n_obuf >= GTA_IFF_OUTBUF && flush_out(j))
        j->err = 1;                     /* a full disk, and the step says so */
}

static void finish(gta_iff *j)
{
    /* The BODY's length is promised in the header, so it is written even if
     * the source ran a sample or two short of what the arithmetic said - a
     * short BODY is a corrupt file, a repeated last sample is inaudible. */
    while (j->out_done < j->out_total)
        emit(j, j->last);
    if (j->body_pad) {
        if (flush_out(j)) j->err = 1;
        if (fputc(0, j->out) == EOF) j->err = 1;
    }
    if (flush_out(j)) j->err = 1;
    /* fclose is where a buffered write actually fails on a full disk, so its
     * return is the one that must not be thrown away: a track that came out
     * short would otherwise be indistinguishable from a finished one on the
     * next run, and would play as music that stops in the middle. */
    if (j->out) {
        if (fclose(j->out) != 0) j->err = 1;
        j->out = NULL;
    }
    if (j->in)  { fclose(j->in);  j->in  = NULL; }
    j->done = 1;
}

int gta_iff_step(gta_iff *j)
{
    unsigned long want;
    size_t got;
    int bpf, i, n;

    if (j->err)
        return -1;
    if (j->done)
        return 0;

    bpf = j->channels * (j->bits / 8);
    want = (unsigned long)GTA_IFF_FRAMES * (unsigned long)bpf;
    if (want > j->data_left)
        want = j->data_left - (j->data_left % (unsigned long)bpf);

    if (want == 0) {
        finish(j);
        return 0;
    }
    got = fread(j->raw, 1, (size_t)want, j->in);
    if (got == 0) {
        finish(j);
        return 0;
    }
    j->data_left -= (unsigned long)got;
    n = (int)((unsigned long)got / (unsigned long)bpf);

    /* to mono, still 16 bits: the average of the two channels, not the left
     * one - a track mixed with the bass hard right would lose it. */
    if (j->bits == 16) {
        const unsigned char *p = j->raw;
        if (j->channels == 2) {
            for (i = 0; i < n; i++, p += 4) {
                int l = (int)(short)rd16le(p);
                int r = (int)(short)rd16le(p + 2);
                j->mono[i] = (short)((l + r) >> 1);
            }
        } else {
            for (i = 0; i < n; i++, p += 2)
                j->mono[i] = (short)(short)rd16le(p);
        }
    } else {
        const unsigned char *p = j->raw;
        if (j->channels == 2) {
            for (i = 0; i < n; i++, p += 2) {
                int l = ((int)p[0] - 128) << 8;
                int r = ((int)p[1] - 128) << 8;
                j->mono[i] = (short)((l + r) >> 1);
            }
        } else {
            for (i = 0; i < n; i++, p++)
                j->mono[i] = (short)(((int)p[0] - 128) << 8);
        }
    }
    j->n_mono = n;
    j->last = j->mono[n - 1];

    /* THE FAST PATH, and it is the one that runs: the rates match, so every
     * source frame is one output sample and the resampler's accumulator has
     * nothing to decide. Worth its ten lines - the nine tracks are twelve
     * million samples and this loop is what a 68020 spends the extraction in.
     */
    if (j->step == 0x10000UL && j->phase == 0) {
        for (i = 0; i < n && j->out_done < j->out_total; i++)
            emit(j, j->mono[i]);
    } else {
        unsigned long span = (unsigned long)n << 16;
        while (j->out_done < j->out_total) {
            unsigned long idx = j->phase >> 16;
            if (idx >= (unsigned long)n)
                break;
            emit(j, j->mono[(int)idx]);
            j->phase += j->step;
        }
        /* THE PHASE IS RELATIVE TO THIS WINDOW, never absolute, and that is
         * not tidiness: an absolute 16.16 position in source frames overflows
         * 32 bits after 65 536 frames - three seconds of music - so a track
         * would have come out right for three seconds and been noise after
         * that, on exactly the machine nobody can hear. */
        j->phase = j->phase >= span ? j->phase - span : 0;
    }

    if (j->out_done >= j->out_total || j->data_left == 0) {
        finish(j);
        return 0;
    }
    return 1;
}

void gta_iff_abort(gta_iff *j)
{
    if (j->in)  { fclose(j->in);  j->in  = NULL; }
    if (j->out) {
        fclose(j->out);
        j->out = NULL;
        /* A PARTIAL TRACK MUST NOT SURVIVE. The next run decides what to
         * extract by asking whether the output is already there; a file left
         * behind by an interrupted extraction would be taken for a finished
         * one and played as a track that stops in the middle. */
        if (!j->done && j->dst[0])
            remove(j->dst);
    }
    j->done = 1;
}

int gta_iff_permille(const gta_iff *j)
{
    unsigned long d;
    if (j->out_total == 0)
        return 0;
    if (j->done)
        return 1000;
    /* out_done * 1000 overflows on a long track, so the division goes the
     * other way round. */
    d = j->out_total / 1000UL;
    if (d == 0)
        return (int)((j->out_done * 1000UL) / j->out_total);
    return (int)(j->out_done / d) > 1000 ? 1000 : (int)(j->out_done / d);
}

int gta_iff_convert(const char *src, const char *dst, int out_rate)
{
    gta_iff *j;
    int rc;

    j = (gta_iff *)malloc(sizeof *j);
    if (!j)
        return 1;
    if (gta_iff_open(j, src, dst, out_rate)) {
        free(j);
        return 1;
    }
    while ((rc = gta_iff_step(j)) > 0)
        ;
    if (rc < 0) {
        gta_iff_abort(j);
        free(j);
        return 1;
    }
    free(j);
    return 0;
}

/* ---- what needs extracting ---------------------------------------------- */

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static unsigned long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f)
        return 0;
    fseek(f, 0L, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n > 0 ? (unsigned long)n : 0;
}

/* WHICH TRACK IS WHAT, AND IT IS MEASURED RATHER THAN GUESSED.
 *
 * The first version numbered the outputs in the order the files were found,
 * so Track1 became station 1 and so on. That put the WRONG THING ON THE CAR
 * RADIO and the developer heard it at once: *"jest audio policyjne zamiast
 * radia w aucie"* - police dispatch chatter playing in an ordinary saloon,
 * which the original never did.
 *
 * The nine files of the 2002 release are not nine radio stations. Walked in
 * 100 ms blocks (length, mean amplitude, and how many blocks never rise
 * above a whisper):
 *
 *     Track1    137.0 s  mean |amp| 2842   4.8% quiet
 *     Track2    445.0 s             6407   2.0%
 *     Track3    581.0 s             5936   0.8%
 *     Track4    577.0 s             5273   0.9%
 *     Track5    451.0 s             6500   1.1%
 *     Track6    659.0 s             6063   1.5%
 *     Track7    191.0 s             7078   1.6%
 *     Track9     61.0 s             2302  16.7%
 *     Track10    72.0 s              875   8.6%
 *
 * Tracks 2 to 6 are seven to eleven minutes of continuous loud audio with
 * almost no gaps: those are the radio stations, and their length is what a
 * station is. **Track9 is a minute long, quiet, and one block in six is
 * silence** - that is speech with pauses between transmissions, which is the
 * police band, and it is what was playing in the developer's car. Track10 is
 * quieter still at a mean of 875 and too short to be a station. Track1, two
 * minutes and continuous, is the title theme. Track8 is not in the release.
 *
 * So the mapping is by ROLE, written down here with the numbers that decided
 * it. A player whose files are numbered differently gets the wrong roles, and
 * that is a real limitation - but a rule with evidence behind it beats
 * "whatever order readdir returned", which is what produced the fault.
 */
static const struct { int track; const char *role; } iff_role[] = {
    {  1, "title"  },
    {  2, "radio1" },
    {  3, "radio2" },
    {  4, "radio3" },
    {  5, "radio4" },
    {  6, "radio5" },
    {  7, "radio6" },
    {  9, "police" }
};
#define IFF_ROLES ((int)(sizeof iff_role / sizeof iff_role[0]))

int gta_iff_scan(const char *dir, gta_iff_item *items, int max)
{
    int k, todo = 0;

    if (!dir || !items || max <= 0)
        return 0;
    for (k = 0; k < IFF_ROLES; k++) {
        char src[GTA_IFF_PATHLEN], dst[GTA_IFF_PATHLEN];

        snprintf(src, sizeof src, "%sMusic/Track%d.wav", dir,
                 iff_role[k].track);
        if (!file_exists(src))
            snprintf(src, sizeof src, "%sTrack%d.wav", dir,
                     iff_role[k].track);
        if (!file_exists(src))
            continue;

        snprintf(dst, sizeof dst, "%s%s.8svx", dir, iff_role[k].role);
        if (file_exists(dst))
            continue;
        if (todo >= max)
            break;
        strncpy(items[todo].src, src, GTA_IFF_PATHLEN - 1);
        items[todo].src[GTA_IFF_PATHLEN - 1] = 0;
        strncpy(items[todo].dst, dst, GTA_IFF_PATHLEN - 1);
        items[todo].dst[GTA_IFF_PATHLEN - 1] = 0;
        items[todo].bytes = file_size(src);
        todo++;
    }
    return todo;
}
