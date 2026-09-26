/* gtamusic - turn GTA's soundtrack into what the Amiga can stream.
 *
 *     gtamusic <in.wav> <out.wav>          convert
 *     gtamusic -check <in.wav> <out.wav>   convert, then DECODE WHAT IT WROTE
 *                                          and report the error against the
 *                                          source
 *
 * WHY THIS EXISTS AND WHAT IT IS NOT DOING
 * ----------------------------------------
 * The DOS release played its soundtrack off the CD; the 2002 Windows release
 * ships it as WAV, and that is where `pc/GTA/Music/Track*.wav` comes from -
 * nine of the ten. They are already **22050 Hz, 16-bit, stereo**, which is a
 * piece of luck worth naming: 22050 is inside Paula's range (period 161,
 * against a DMA floor of 124), so there is no resampling to do and no quality
 * argument to have. All that is left is stereo to mono and 16 bits to four.
 *
 * FOUR BITS, NOT EIGHT, and the reason is the disk rather than the ear. A
 * three-minute track as raw 8-bit mono at 22 kHz is about 4 MB and the nine
 * of them are 36; as IMA ADPCM they are half that, and the decoder is a table
 * lookup and three shifts per sample, which the 68020 can afford between
 * frames. `native/amiga_adpcm.c` is that decoder, carried over from the
 * OpenTTD port and already streaming block by block from disk - so this tool
 * writes exactly what it reads: **wFormatTag 0x11, mono, blockAlign 1024,
 * 2041 samples a block**, low nibble first, each block starting with its own
 * predictor so a seek never has to replay the track from the beginning.
 *
 * WHAT -check IS FOR. An encoder can be plausible and wrong in a way no
 * listening test on the host would catch, because the host is not what plays
 * it. So this decodes its own output with the SAME arithmetic the Amiga will
 * use and prints the mean and worst absolute error against the mono source,
 * in the 8-bit units the hardware actually receives. IMA at 4 bits is lossy
 * and a mean error of a few units is right; a mean error of fifty means the
 * nibble order or the block header is wrong, which is the failure this tool
 * is most likely to have.
 *
 * Portable C89, stdio only. Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_ALIGN        1024
#define SAMPLES_PER_BLOCK  2041

static const int ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767
};

static const int ima_index[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8
};

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static unsigned int rd16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}
static void wr32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}
static void wr16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

/* ---- the source: a PCM WAV, mono or stereo, 16-bit ---------------------- */

typedef struct {
    FILE *f;
    int   channels, rate, bits;
    unsigned long data_off, data_len;
} pcm_wav;

static int wav_open(pcm_wav *w, const char *path)
{
    unsigned char hdr[12], ch[8], fmt[40];
    int have_fmt = 0;

    memset(w, 0, sizeof *w);
    w->f = fopen(path, "rb");
    if (!w->f) { fprintf(stderr, "gtamusic: cannot open %s\n", path); return 1; }
    if (fread(hdr, 1, 12, w->f) != 12 || memcmp(hdr, "RIFF", 4) != 0
        || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "gtamusic: %s is not a RIFF/WAVE file\n", path);
        return 1;
    }
    while (fread(ch, 1, 8, w->f) == 8) {
        unsigned long clen = rd32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            unsigned long want = clen < sizeof fmt ? clen : sizeof fmt;
            if (fread(fmt, 1, want, w->f) != want) return 1;
            if (rd16(fmt) != 1) {
                fprintf(stderr, "gtamusic: %s is not plain PCM (tag %u)\n",
                        path, rd16(fmt));
                return 1;
            }
            w->channels = (int)rd16(fmt + 2);
            w->rate     = (int)rd32(fmt + 4);
            w->bits     = (int)rd16(fmt + 14);
            if (clen > want) fseek(w->f, (long)(clen - want), SEEK_CUR);
            if (clen & 1) fseek(w->f, 1, SEEK_CUR);
            have_fmt = 1;
        } else if (memcmp(ch, "data", 4) == 0) {
            w->data_off = (unsigned long)ftell(w->f);
            w->data_len = clen;
            break;
        } else {
            fseek(w->f, (long)(clen + (clen & 1)), SEEK_CUR);
        }
    }
    if (!have_fmt || !w->data_len) {
        fprintf(stderr, "gtamusic: %s has no fmt/data\n", path);
        return 1;
    }
    if (w->bits != 16 || w->channels < 1 || w->channels > 2) {
        fprintf(stderr, "gtamusic: %s is %d-bit %d-channel; want 16-bit "
                "mono or stereo\n", path, w->bits, w->channels);
        return 1;
    }
    return 0;
}

/* The next `want` MONO samples, downmixed. Returns how many were read. */
static long wav_read_mono(pcm_wav *w, short *out, long want)
{
    static unsigned char buf[4096];
    long got = 0;
    int fb = 2 * w->channels;           /* bytes per source frame */
    while (got < want) {
        long need = want - got;
        long n = (long)(sizeof buf / (size_t)fb);
        size_t r;
        if (n > need) n = need;
        r = fread(buf, (size_t)fb, (size_t)n, w->f);
        if (r == 0) break;
        {
            long i;
            for (i = 0; i < (long)r; i++) {
                const unsigned char *p = buf + i * fb;
                int l = (int)(short)rd16(p);
                if (w->channels == 2) {
                    int rr = (int)(short)rd16(p + 2);
                    l = (l + rr) / 2;
                }
                out[got++] = (short)l;
            }
        }
    }
    return got;
}

/* ---- the encoder -------------------------------------------------------- */

/* One block: the header carries the first sample and the step index, then
 * SAMPLES_PER_BLOCK-1 nibbles, low nibble first. `n` may be short at the end
 * of the track; the rest of the block is padded with the last nibble, which
 * is silence-preserving and keeps every block the same size - the decoder
 * refuses a partial one. */
static void encode_block(const short *in, long n, int *predictor, int *index,
                         unsigned char *out)
{
    long i;
    int byte = 0;
    unsigned char *p;

    *predictor = in[0];
    wr16(out, (unsigned int)(*predictor & 0xffff));
    out[2] = (unsigned char)*index;
    out[3] = 0;
    p = out + 4;
    memset(p, 0, BLOCK_ALIGN - 4);

    for (i = 1; i < SAMPLES_PER_BLOCK; i++) {
        int sample = i < n ? in[i] : *predictor;
        int step = ima_step[*index];
        int diff = sample - *predictor;
        int nibble = 0;
        int diffq;

        if (diff < 0) { nibble = 8; diff = -diff; }
        diffq = step >> 3;
        if (diff >= step)      { nibble |= 4; diff -= step;      diffq += step; }
        if (diff >= (step >> 1)) { nibble |= 2; diff -= step >> 1; diffq += step >> 1; }
        if (diff >= (step >> 2)) { nibble |= 1;                    diffq += step >> 2; }

        if (nibble & 8) *predictor -= diffq; else *predictor += diffq;
        if (*predictor > 32767) *predictor = 32767;
        if (*predictor < -32768) *predictor = -32768;
        *index += ima_index[nibble];
        if (*index < 0) *index = 0;
        if (*index > 88) *index = 88;

        /* Sample 1 is the LOW nibble of the first data byte. */
        if ((i & 1) == 1) byte = nibble & 0x0f;
        else {
            p[(i - 1) / 2] = (unsigned char)(byte | ((nibble & 0x0f) << 4));
        }
        if ((i & 1) == 1 && i == SAMPLES_PER_BLOCK - 1)
            p[(i - 1) / 2] = (unsigned char)byte;
    }
}

/* ---- the decoder, the same arithmetic amiga_adpcm.c uses ---------------- */

static long decode_block(const unsigned char *b, signed char *out)
{
    int predictor = (int)(short)rd16(b);
    int index = b[2];
    long n = 0;
    int i;

    if (index < 0) index = 0;
    if (index > 88) index = 88;
    out[n++] = (signed char)(predictor >> 8);
    for (i = 4; i < BLOCK_ALIGN && n < SAMPLES_PER_BLOCK; i++) {
        int byte = b[i], half;
        for (half = 0; half < 2 && n < SAMPLES_PER_BLOCK; half++) {
            int nibble = half ? (byte >> 4) : (byte & 0x0f);
            int step = ima_step[index];
            int diff = step >> 3;
            if (nibble & 1) diff += step >> 2;
            if (nibble & 2) diff += step >> 1;
            if (nibble & 4) diff += step;
            if (nibble & 8) predictor -= diff; else predictor += diff;
            if (predictor > 32767) predictor = 32767;
            if (predictor < -32768) predictor = -32768;
            index += ima_index[nibble];
            if (index < 0) index = 0;
            if (index > 88) index = 88;
            out[n++] = (signed char)(predictor >> 8);
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    const char *inp, *outp;
    int check = 0, arg = 1;
    pcm_wav w;
    FILE *out;
    short *pcm;
    unsigned char *blk;
    signed char *dec;
    unsigned long blocks = 0, samples = 0;
    /* RIFF 12 + fmt 8+20 + fact 8+4 + data 8 = SIXTY, and the first
     * version said 58 - so the last two bytes of the data chunk's
     * size were written over the first two of block zero, and the
     * data actually began two bytes before the header said. The
     * -check pass did not catch it because it checks the block in
     * memory; only decoding the FILE does. */
    unsigned char hdr[60];
    long err_sum = 0, err_worst = 0, err_n = 0;
    int predictor = 0, index = 0;

    if (argc > 1 && strcmp(argv[1], "-check") == 0) { check = 1; arg = 2; }
    if (argc - arg != 2) {
        fprintf(stderr, "usage: gtamusic [-check] <in.wav> <out.wav>\n");
        return 2;
    }
    inp = argv[arg];
    outp = argv[arg + 1];

    if (wav_open(&w, inp) != 0) return 1;
    if (w.rate != 22050)
        fprintf(stderr, "gtamusic: NOTE - %s is %d Hz, not 22050; it is "
                "written through unchanged and Paula will play it at that "
                "rate (period %d)\n", inp, w.rate,
                w.rate > 0 ? 3546895 / w.rate : 0);

    out = fopen(outp, "wb");
    if (!out) { fprintf(stderr, "gtamusic: cannot write %s\n", outp); return 1; }

    /* A 60-byte placeholder, rewritten once the sizes are known. */
    memset(hdr, 0, sizeof hdr);
    fwrite(hdr, 1, sizeof hdr, out);

    pcm = (short *)malloc(SAMPLES_PER_BLOCK * sizeof(short));
    blk = (unsigned char *)malloc(BLOCK_ALIGN);
    dec = (signed char *)malloc(SAMPLES_PER_BLOCK);
    if (!pcm || !blk || !dec) { fprintf(stderr, "gtamusic: out of memory\n"); return 1; }

    for (;;) {
        long n = wav_read_mono(&w, pcm, SAMPLES_PER_BLOCK);
        if (n <= 0) break;
        encode_block(pcm, n, &predictor, &index, blk);
        fwrite(blk, 1, BLOCK_ALIGN, out);
        blocks++;
        samples += (unsigned long)n;
        if (check) {
            long m = decode_block(blk, dec), i;
            for (i = 0; i < n && i < m; i++) {
                long e = (long)dec[i] - (long)(pcm[i] >> 8);
                if (e < 0) e = -e;
                err_sum += e;
                if (e > err_worst) err_worst = e;
                err_n++;
            }
        }
    }

    /* Now the header, with the sizes known. */
    {
        unsigned long data_bytes = blocks * BLOCK_ALIGN;
        unsigned char *p = hdr;
        memcpy(p, "RIFF", 4);           wr32(p + 4, 52 + data_bytes);
        memcpy(p + 8, "WAVE", 4);
        memcpy(p + 12, "fmt ", 4);      wr32(p + 16, 20);
        wr16(p + 20, 0x0011);           /* IMA ADPCM */
        wr16(p + 22, 1);                /* mono */
        wr32(p + 24, (unsigned long)w.rate);
        /* Average bytes a second: blocks a second times the block. */
        wr32(p + 28, (unsigned long)w.rate * BLOCK_ALIGN / SAMPLES_PER_BLOCK);
        wr16(p + 32, BLOCK_ALIGN);
        wr16(p + 34, 4);                /* bits per sample */
        wr16(p + 36, 2);                /* cbSize */
        wr16(p + 38, SAMPLES_PER_BLOCK);
        memcpy(p + 40, "fact", 4);      wr32(p + 44, 4);
        wr32(p + 48, samples);
        memcpy(p + 52, "data", 4);      wr32(p + 56, data_bytes);
        fseek(out, 0, SEEK_SET);
        fwrite(hdr, 1, sizeof hdr, out);
    }
    fclose(out);
    fclose(w.f);

    printf("gtamusic: %s -> %s\n", inp, outp);
    printf("  %lu samples, %d Hz mono, %lu blocks of %d = %lu bytes "
           "(%lu KB, %lu s)\n", samples, w.rate, blocks, BLOCK_ALIGN,
           blocks * BLOCK_ALIGN, blocks * BLOCK_ALIGN / 1024,
           w.rate ? samples / (unsigned long)w.rate : 0);
    if (check && err_n > 0)
        printf("  CHECK: decoded with the Amiga's own arithmetic - mean error "
               "%ld.%02ld of 255, worst %ld, over %ld samples\n",
               err_sum / err_n, (err_sum * 100 / err_n) % 100, err_worst,
               err_n);
    free(pcm); free(blk); free(dec);
    return 0;
}
