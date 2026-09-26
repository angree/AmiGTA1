/* gtaiff - the music extractor as a program.
 *
 *     gtaiff <in.wav> <out.8svx> [rate]   convert one track
 *     gtaiff -scan <dir>                  what a first run would extract
 *     gtaiff -info <file.8svx>            the header, and what is in the BODY
 *
 * IT IS THE SAME CODE THE GAME RUNS. `native/gta_iff.c` does the work; this
 * file is a main() around it, built for the host by tools/bin/build_host.sh
 * and for the Amiga by tools/bin/build.sh. That is deliberate and it is the
 * rule the tile baker already follows: a converter that only exists on a PC
 * is no use to somebody who has an Amiga and the CD.
 *
 * WHAT -info IS FOR. Nobody working on this port can hear the emulator, so
 * "it played" is always argued from numbers. -info decodes the BODY and
 * prints its minimum, maximum, mean and mean absolute amplitude: full range
 * with a mean near zero is signal; a mean near -128 or +127 is a sign
 * conversion that went the wrong way; a mean absolute amplitude of nothing
 * is silence. That is the same test PROGRESS.md (172) ran on the effects
 * bank, and it caught nothing there because nothing was wrong there.
 *
 * Portable C89, stdio only. Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/gta_iff.h"

static unsigned long rd32be(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
         | ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static int do_info(const char *path)
{
    FILE *f;
    unsigned char hdr[12], ch[8], vhdr[20];
    unsigned long body = 0, one = 0;
    int rate = 0, comp = -1;
    long vol = -1;

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gtaiff: cannot open %s\n", path);
        return 1;
    }
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "FORM", 4) != 0
        || memcmp(hdr + 8, "8SVX", 4) != 0) {
        fprintf(stderr, "gtaiff: %s is not an IFF 8SVX\n", path);
        fclose(f);
        return 1;
    }
    printf("%s: FORM %lu bytes, 8SVX\n", path, rd32be(hdr + 4));

    while (fread(ch, 1, 8, f) == 8) {
        unsigned long clen = rd32be(ch + 4);
        if (memcmp(ch, "VHDR", 4) == 0 && clen >= 20) {
            if (fread(vhdr, 1, 20, f) != 20) break;
            one  = rd32be(vhdr);
            rate = ((int)vhdr[12] << 8) | vhdr[13];
            comp = vhdr[15];
            vol  = (long)rd32be(vhdr + 16);
            printf("  VHDR  %lu samples, %d Hz, octaves %d, compression %d, "
                   "volume 0x%lx\n", one, rate, vhdr[14], comp, vol);
            if (clen > 20) fseek(f, (long)(clen - 20), SEEK_CUR);
            if (clen & 1UL) fseek(f, 1L, SEEK_CUR);
        } else if (memcmp(ch, "BODY", 4) == 0) {
            /* THE SAMPLES THEMSELVES. Streamed, because a track is twelve
             * megabytes and this tool must run on the Amiga too. */
            /* INTEGER ARITHMETIC ONLY, and not out of purity: this tool is
             * also built for the Amiga, where a float multiply reaches a
             * Kickstart 3.1 ROM whose single-precision multiply entry is
             * broken on an FPU-less machine (CLAUDE.md, defect 5). The means
             * are printed as hundredths of a unit, worked out with longs.
             *
             * AND THE SUMS ARE CAPPED AT TWO MILLION SAMPLES - ninety
             * seconds - because |v| summed over a twelve-minute track is two
             * thousand million and overflows a 32-bit long on the target.
             * Ninety seconds is a fair sample of a track and an honest one:
             * the line says so. */
            long mn = 127, mx = -128, n = 0, sum = 0, asum = 0;
            signed char buf[4096];
            size_t got;
            unsigned long left = clen;
            body = clen;
            while (left > 0 &&
                   (got = fread(buf, 1,
                                left < sizeof buf ? (size_t)left : sizeof buf,
                                f)) > 0) {
                size_t i;
                for (i = 0; i < got; i++) {
                    int v = buf[i];
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                    if (n < 2000000L) {
                        sum += v;
                        asum += v < 0 ? -v : v;
                        n++;
                    }
                }
                left -= (unsigned long)got;
            }
            {   /* the fraction as (remainder * 100) / n, because sum * 100
                 * is what would overflow. */
                long mf = n ? (sum % n) * 100L / n : 0L;
                long af = n ? (asum % n) * 100L / n : 0L;
                if (mf < 0) mf = -mf;
                printf("  BODY  %lu bytes | min %ld max %ld | mean %ld.%02ld"
                       " | mean |amp| %ld.%02ld%s\n", clen, mn, mx,
                       n ? sum / n : 0L, mf, n ? asum / n : 0L, af,
                       clen > 2000000UL ? "  (first 2 000 000 samples)" : "");
            }
            if (rate > 0)
                printf("  = %lu.%lu seconds at %d Hz, Paula period %d\n",
                       clen / (unsigned long)rate,
                       (clen % (unsigned long)rate) * 10UL
                           / (unsigned long)rate,
                       rate, 3546895 / rate);
            if (clen & 1UL) fseek(f, 1L, SEEK_CUR);
        } else {
            if (memcmp(ch, "ANNO", 4) == 0 && clen < 200) {
                char anno[200];
                if (fread(anno, 1, (size_t)clen, f) != (size_t)clen) break;
                anno[clen] = 0;
                printf("  ANNO  %s\n", anno);
                if (clen & 1UL) fseek(f, 1L, SEEK_CUR);
                continue;
            }
            fseek(f, (long)(clen + (clen & 1UL)), SEEK_CUR);
        }
    }
    fclose(f);
    if (body != one)
        printf("  ** BODY is %lu bytes but VHDR promises %lu samples **\n",
               body, one);
    if (comp != 0)
        printf("  ** sCompression %d - this port only writes 0 **\n", comp);
    return 0;
}

static int do_scan(const char *dir)
{
    gta_iff_item it[GTA_IFF_MAX_ITEMS];
    int n, i;

    n = gta_iff_scan(dir, it, GTA_IFF_MAX_ITEMS);
    printf("%s: %d track%s to extract\n", dir, n, n == 1 ? "" : "s");
    for (i = 0; i < n; i++)
        printf("  %s -> %s (%lu KB)\n", it[i].src, it[i].dst,
               it[i].bytes / 1024UL);
    return 0;
}

int main(int argc, char **argv)
{
    gta_iff *j;
    int rate = 0, rc, last = -1;

    if (argc >= 3 && strcmp(argv[1], "-info") == 0)
        return do_info(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "-scan") == 0)
        return do_scan(argv[2]);
    if (argc < 3) {
        fprintf(stderr,
                "usage: gtaiff <in.wav> <out.8svx> [rate]\n"
                "       gtaiff -scan <dir>\n"
                "       gtaiff -info <file.8svx>\n");
        return 2;
    }
    if (argc >= 4)
        rate = atoi(argv[3]);

    j = (gta_iff *)malloc(sizeof *j);
    if (!j) {
        fprintf(stderr, "gtaiff: out of memory\n");
        return 1;
    }
    if (gta_iff_open(j, argv[1], argv[2], rate)) {
        fprintf(stderr, "gtaiff: cannot convert %s\n", argv[1]);
        free(j);
        return 1;
    }
    printf("%s: %d Hz, %d bit, %d channel%s, %lu bytes\n", argv[1],
           j->rate, j->bits, j->channels, j->channels == 1 ? "" : "s",
           j->data_total);
    printf("%s: %lu samples at %d Hz\n", argv[2], j->out_total, j->out_rate);

    while ((rc = gta_iff_step(j)) > 0) {
        int pm = gta_iff_permille(j) / 100;
        if (pm != last) {
            last = pm;
            printf("\r  %3d%%", pm * 10);
            fflush(stdout);
        }
    }
    printf("\r  100%%\n");
    if (rc < 0) {
        fprintf(stderr, "gtaiff: write failed\n");
        gta_iff_abort(j);
        free(j);
        return 1;
    }
    free(j);
    return 0;
}
