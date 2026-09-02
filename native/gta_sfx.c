/* The sound bank: read the baked one, and bake it from the original pair.
 *
 * See gta_sfx.h for the format and for why it is baked at all.
 *
 * No sprintf anywhere (CLAUDE.md defect 4), no floats, C89 aggregates only -
 * this file is built with -std=c89 -pedantic on the host so that bebbo's 68k
 * GCC 6.5 cannot find anything in it that the host compiler let through.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gta_sfx.h"

/* PAULA'S CLOCK, PAL. period = clock / rate, and the DMA floor is 124.
 *
 * PAL and not NTSC on purpose: the target machine is a European A1200 and the
 * difference is 0.9%, which is under a semitone's twentieth - inaudible on a
 * car horn and not worth a second build. amiga_audio.c uses the same floor. */
#define PAULA_PAL_CLOCK 3546895UL
#define PAULA_MIN_PERIOD 124

/* ---- byte order ---------------------------------------------------------
 *
 * Read and write explicitly, byte at a time, rather than casting a struct over
 * a buffer. The whole engine does this: the same source has to be correct on a
 * big-endian 68k, on a big-endian PowerPC and on a little-endian host, and a
 * struct cast is only correct on one of them. */

static unsigned long rd_le32(const unsigned char *p)
{
    return (unsigned long)p[0]        | ((unsigned long)p[1] << 8) |
          ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned long rd_be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

static unsigned int rd_be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}

static void wr_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xff);
    p[1] = (unsigned char)((v >> 16) & 0xff);
    p[2] = (unsigned char)((v >> 8)  & 0xff);
    p[3] = (unsigned char)( v        & 0xff);
}

static void wr_be16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)((v >> 8) & 0xff);
    p[1] = (unsigned char)( v       & 0xff);
}

/* Rounded rather than truncated: at 5500 Hz the truncation is 0.3 Hz and at
 * 22050 it is 60 Hz, which is a quarter of a semitone. Costs one add. */
static int period_for(unsigned long rate)
{
    unsigned long p;
    if (rate == 0) return PAULA_MIN_PERIOD;
    p = (PAULA_PAL_CLOCK + rate / 2) / rate;
    if (p < PAULA_MIN_PERIOD) p = PAULA_MIN_PERIOD;
    if (p > 65535UL) p = 65535UL;
    return (int)p;
}

/* ---- loading the baked bank --------------------------------------------- */

#define HDR_BYTES   16          /* magic, version, count, data bytes */
#define ENTRY_BYTES 12          /* offset, length, period, rate */

int gta_sfx_load(const char *path, gta_sfx *sfx)
{
    unsigned char hdr[HDR_BYTES];
    unsigned char *idx = NULL;
    FILE *f;
    int i, count;
    unsigned long bytes;

    if (sfx == NULL) return 1;
    memset(sfx, 0, sizeof *sfx);
    if (path == NULL) return 1;

    f = fopen(path, "rb");
    if (f == NULL) return 1;

    if (fread(hdr, 1, HDR_BYTES, f) != HDR_BYTES) { fclose(f); return 1; }
    if (rd_be32(hdr) != GTA_SFX_MAGIC)            { fclose(f); return 2; }
    if (rd_be32(hdr + 4) != (unsigned long)GTA_SFX_VERSION) {
        fclose(f); return 3;
    }
    count = (int)rd_be32(hdr + 8);
    bytes = rd_be32(hdr + 12);
    if (count <= 0 || count > GTA_SFX_MAX || bytes == 0) { fclose(f); return 4; }

    idx = (unsigned char *)malloc((size_t)count * ENTRY_BYTES);
    sfx->entry = (gta_sfx_entry *)malloc((size_t)count * sizeof(gta_sfx_entry));
    sfx->data  = (signed char *)malloc((size_t)bytes);
    if (idx == NULL || sfx->entry == NULL || sfx->data == NULL) {
        free(idx); fclose(f); gta_sfx_free(sfx); return 5;
    }

    if (fread(idx, 1, (size_t)count * ENTRY_BYTES, f)
            != (size_t)count * ENTRY_BYTES) {
        free(idx); fclose(f); gta_sfx_free(sfx); return 6;
    }
    if (fread(sfx->data, 1, (size_t)bytes, f) != (size_t)bytes) {
        free(idx); fclose(f); gta_sfx_free(sfx); return 7;
    }
    fclose(f);

    for (i = 0; i < count; i++) {
        const unsigned char *e = idx + (long)i * ENTRY_BYTES;
        sfx->entry[i].offset = rd_be32(e);
        sfx->entry[i].length = rd_be32(e + 4);
        sfx->entry[i].period = (unsigned short)rd_be16(e + 8);
        sfx->entry[i].rate   = (unsigned short)rd_be16(e + 10);
        /* A truncated or edited bank must not hand the audio hardware a
         * pointer past the end of the buffer. Clamp rather than reject: one
         * bad entry is a silent sound, not a dead game. */
        if (sfx->entry[i].offset > bytes ||
            sfx->entry[i].length > bytes - sfx->entry[i].offset) {
            sfx->entry[i].offset = 0;
            sfx->entry[i].length = 0;
        }
    }
    free(idx);

    sfx->count = count;
    sfx->bytes = bytes;
    return 0;
}

void gta_sfx_free(gta_sfx *sfx)
{
    if (sfx == NULL) return;
    free(sfx->entry);
    free(sfx->data);
    memset(sfx, 0, sizeof *sfx);
}

const signed char *gta_sfx_sample(const gta_sfx *sfx, int n, unsigned long *len,
                                  int *period)
{
    if (len)    *len = 0;
    if (period) *period = PAULA_MIN_PERIOD;
    if (sfx == NULL || sfx->data == NULL) return NULL;
    if (n < 0 || n >= sfx->count) return NULL;
    if (sfx->entry[n].length == 0) return NULL;
    if (len)    *len = sfx->entry[n].length;
    if (period) *period = (int)sfx->entry[n].period;
    return sfx->data + sfx->entry[n].offset;
}

void gta_sfx_describe(const gta_sfx *sfx, FILE *out)
{
    unsigned long shortest = 0, longest = 0, used = 0;
    int i, holes = 0;

    if (sfx == NULL || out == NULL) return;
    for (i = 0; i < sfx->count; i++) {
        unsigned long L = sfx->entry[i].length;
        if (L == 0) { holes++; continue; }
        used += L;
        if (shortest == 0 || L < shortest) shortest = L;
        if (L > longest) longest = L;
    }
    fprintf(out, "sfx: %d sounds (%d empty), %lu bytes, %lu..%lu each\n",
            sfx->count, holes, sfx->bytes, shortest, longest);
}

/* ---- baking, from the original .SDT + .RAW ------------------------------ */

int gta_sfx_bake(const char *sdt_path, const char *raw_path, const char *out_path,
                 FILE *log)
{
    unsigned char *sdt = NULL, *idx = NULL;
    unsigned char *raw = NULL;
    unsigned char hdr[HDR_BYTES];
    FILE *f = NULL;
    long sdt_bytes = 0, raw_bytes = 0;
    unsigned long out_bytes = 0, pos = 0;
    int count = 0, i, rc = 1;

    /* --- the index --- */
    f = fopen(sdt_path, "rb");
    if (f == NULL) {
        if (log) fprintf(log, "gtabake: cannot open %s\n", sdt_path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    sdt_bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* 12 bytes a record, and nothing else in the file - which is why a size
     * that is not a multiple of 12 means this is not an .sdt at all. */
    if (sdt_bytes <= 0 || (sdt_bytes % 12) != 0) {
        if (log) fprintf(log, "gtabake: %s is %ld bytes, not a multiple of 12 -"
                              " this is not an .sdt\n", sdt_path, sdt_bytes);
        fclose(f); return 2;
    }
    count = (int)(sdt_bytes / 12);
    if (count > GTA_SFX_MAX) {
        if (log) fprintf(log, "gtabake: %s has %d entries, over the %d ceiling\n",
                         sdt_path, count, GTA_SFX_MAX);
        fclose(f); return 3;
    }
    sdt = (unsigned char *)malloc((size_t)sdt_bytes);
    if (sdt == NULL || fread(sdt, 1, (size_t)sdt_bytes, f) != (size_t)sdt_bytes) {
        if (log) fprintf(log, "gtabake: cannot read %s\n", sdt_path);
        free(sdt); fclose(f); return 4;
    }
    fclose(f);

    /* --- the samples --- */
    f = fopen(raw_path, "rb");
    if (f == NULL) {
        if (log) fprintf(log, "gtabake: cannot open %s\n", raw_path);
        free(sdt); return 5;
    }
    fseek(f, 0, SEEK_END);
    raw_bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    raw = (unsigned char *)malloc((size_t)(raw_bytes > 0 ? raw_bytes : 1));
    if (raw_bytes <= 0 || raw == NULL ||
        fread(raw, 1, (size_t)raw_bytes, f) != (size_t)raw_bytes) {
        if (log) fprintf(log, "gtabake: cannot read %s\n", raw_path);
        free(sdt); free(raw); fclose(f); return 6;
    }
    fclose(f);

    /* --- lay the output out: every sound rounded up to an even length ---
     *
     * Repacked rather than copied wholesale, because rounding lengths up would
     * otherwise make each sound overlap the start of the next one by a byte.
     * The bank grows by at most `count` bytes. */
    idx = (unsigned char *)malloc((size_t)count * ENTRY_BYTES);
    if (idx == NULL) { free(sdt); free(raw); return 7; }
    memset(idx, 0, (size_t)count * ENTRY_BYTES);

    for (i = 0; i < count; i++) {
        unsigned long off  = rd_le32(sdt + (long)i * 12);
        unsigned long len  = rd_le32(sdt + (long)i * 12 + 4);
        unsigned long rate = rd_le32(sdt + (long)i * 12 + 8);
        unsigned long even;

        /* An entry that points outside the .raw is a corrupt or text-mode
         * transferred file. Turn it into a hole rather than reading wild. */
        if (len == 0 || off > (unsigned long)raw_bytes ||
            len > (unsigned long)raw_bytes - off) {
            if (log && len != 0)
                fprintf(log, "gtabake: sound %d is outside the .raw "
                             "(off %lu len %lu) - dropped\n", i, off, len);
            continue;
        }
        even = (len + 1UL) & ~1UL;
        wr_be32(idx + (long)i * ENTRY_BYTES,     out_bytes);
        wr_be32(idx + (long)i * ENTRY_BYTES + 4, even);
        wr_be16(idx + (long)i * ENTRY_BYTES + 8, (unsigned int)period_for(rate));
        wr_be16(idx + (long)i * ENTRY_BYTES + 10,
                (unsigned int)(rate > 65535UL ? 65535UL : rate));
        out_bytes += even;
    }

    /* --- write it --- */
    f = fopen(out_path, "wb");
    if (f == NULL) {
        if (log) fprintf(log, "gtabake: cannot write %s\n", out_path);
        free(sdt); free(raw); free(idx); return 8;
    }
    wr_be32(hdr,      GTA_SFX_MAGIC);
    wr_be32(hdr + 4,  (unsigned long)GTA_SFX_VERSION);
    wr_be32(hdr + 8,  (unsigned long)count);
    wr_be32(hdr + 12, out_bytes);
    if (fwrite(hdr, 1, HDR_BYTES, f) != HDR_BYTES) goto writefail;
    if (fwrite(idx, 1, (size_t)count * ENTRY_BYTES, f)
            != (size_t)count * ENTRY_BYTES) goto writefail;

    /* The samples, unsigned to signed as they go past. The pad byte of an
     * odd-length sound is SILENCE (0 signed), not a repeat of the last
     * sample - one extra sample at 5.5 kHz is 180 microseconds and a repeated
     * peak there is an audible click. */
    for (i = 0; i < count; i++) {
        unsigned long off = rd_le32(sdt + (long)i * 12);
        unsigned long len = rd_le32(sdt + (long)i * 12 + 4);
        unsigned long j, even;
        if (rd_be32(idx + (long)i * ENTRY_BYTES + 4) == 0) continue;
        even = (len + 1UL) & ~1UL;
        for (j = 0; j < len; j++) {
            int v = (int)raw[off + j] - 128;
            if (fputc(v & 0xff, f) == EOF) goto writefail;
        }
        for (j = len; j < even; j++)
            if (fputc(0, f) == EOF) goto writefail;
        pos += even;
    }
    if (pos != out_bytes) {
        if (log) fprintf(log, "gtabake: internal error, wrote %lu of %lu\n",
                         pos, out_bytes);
        goto writefail;
    }
    if (fclose(f) != 0) {
        if (log) fprintf(log, "gtabake: write failed on %s\n", out_path);
        free(sdt); free(raw); free(idx); return 9;
    }
    f = NULL;

    if (log) {
        int holes = 0;
        for (i = 0; i < count; i++)
            if (rd_be32(idx + (long)i * ENTRY_BYTES + 4) == 0) holes++;
        fprintf(log, "gtabake: %s + %s -> %s\n", sdt_path, raw_path, out_path);
        fprintf(log, "gtabake: %d sounds (%d empty), %lu bytes of samples\n",
                count, holes, out_bytes);
    }
    rc = 0;

writefail:
    if (f != NULL) {
        fclose(f);
        if (rc != 0 && log)
            fprintf(log, "gtabake: write failed on %s\n", out_path);
        if (rc != 0) rc = 9;
    }
    free(sdt); free(raw); free(idx);
    return rc;
}
