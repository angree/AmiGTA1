/* gtabake - turn a .GRY style file into the baked 32x32 tile set the port
 * loads at runtime.
 *
 *   build/host/gtabake <style.gry> <out.til>
 *   build/host/gtabake -sfx <audio/level001> <out.snd>
 *
 * The second form bakes the SOUND bank instead: it reads <prefix>.sdt and
 * <prefix>.raw - GTA's own pair - and writes the Paula-ready bank described in
 * native/gta_sfx.h. Same tool because it is the same job (convert the player's
 * own files, once, on whichever machine they have) and because shipping one
 * converter is one thing for a player to find instead of two.
 *
 * HOST TOOL. It runs on the PC at build time so that the Amiga never has to
 * parse a .GRY, never downscales a block, and never rotates one. See
 * native/gta_tiles.h for the file layout and the size arithmetic, and the
 * Phase 4 design note in PLAN.md for why the renderer wants exactly these
 * variants.
 *
 * The downscale filter is NEAREST, and that was decided by looking rather than
 * by argument: averaging four source pixels and snapping back to the palette
 * dulls GTA's road markings, which are thin bright lines on dark tarmac. The
 * comparison is in PROGRESS.md. gtadump still has both filters if the question
 * is ever reopened.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/gta_style.h"
#include "../native/gta_tiles.h"
#include "../native/gta_sfx.h"
#include "../native/gta_front.h"

#define SRC_DIM  GTA_BLOCK_DIM      /* 64 */
#define DST_DIM  GTA_TILE_DIM       /* 32 */

/* 2:1 nearest downscale of one 64x64 block into 32x32. */
static void shrink(const unsigned char *src, unsigned char *dst)
{
    int x, y;
    for (y = 0; y < DST_DIM; y++) {
        const unsigned char *s = src + (long)(y * 2) * SRC_DIM;
        unsigned char *d = dst + (long)y * DST_DIM;
        for (x = 0; x < DST_DIM; x++)
            d[x] = s[x * 2];
    }
}

/* Rotate a 32x32 tile clockwise by rot*90 degrees.
 *
 * CLOCKWISE is a guess that the first render settles: the map's lid_rotation
 * field says how much to turn the lid, not which way the artist's zero points,
 * and the only cheap way to know is to look at Liberty City's road markings.
 * If they run across junctions instead of through them, swap this for the
 * anticlockwise form (dst(x,y) = src(DIM-1-y, x) at rot 1) and rebake. */
static void rotate(const unsigned char *src, unsigned char *dst, int rot)
{
    int x, y;
    for (y = 0; y < DST_DIM; y++) {
        for (x = 0; x < DST_DIM; x++) {
            int sx, sy;
            switch (rot & 3) {
            case 1:  sx = y;               sy = DST_DIM - 1 - x; break;
            case 2:  sx = DST_DIM - 1 - x; sy = DST_DIM - 1 - y; break;
            case 3:  sx = DST_DIM - 1 - y; sy = x;               break;
            default: sx = x;               sy = y;               break;
            }
            dst[(long)y * DST_DIM + x] = src[(long)sy * DST_DIM + sx];
        }
    }
}


static void put_be32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void put_be16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

/* Fetch block `index` of `type`, shrink it, and hand back a pointer to a
 * static 32x32 buffer. Returns NULL if the style file has no such block. */
static const unsigned char *fetch(const gta_style *st, gta_block_type type,
                                  int index)
{
    static unsigned char big[GTA_BLOCK_AREA];
    static unsigned char small_[GTA_TILE_AREA];
    if (gta_style_get_block(st, type, index, big, SRC_DIM) != 0)
        return NULL;
    shrink(big, small_);
    return small_;
}


/* ---- THE FRONT END'S ART ------------------------------------------------
 *
 * See gta_front.h for the format of what this writes and for how the .rat /
 * .raw pair was decoded. Everything here is nearest-pixel on purpose: the
 * source is PALETTE INDICES, and the average of two indices is a colour
 * neither of them was.
 */
#define FRONT_SRC_W   640
#define FRONT_SRC_H   480
#define FRONT_UP_H    168               /* the logo strip, source rows */
#define FRONT_LOW_H   312               /* the background, source rows */

static int front_read(const char *dir, const char *name, unsigned char *dst,
                      long want)
{
    char path[512];
    FILE *f;
    long got;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gtabake: cannot open %s\n", path);
        return 1;
    }
    got = (long)fread(dst, 1, (size_t)want, f);
    fclose(f);
    if (got != want) {
        fprintf(stderr, "gtabake: %s is %ld bytes, expected %ld\n",
                path, got, want);
        return 1;
    }
    return 0;
}


/* ---- THE FRONT END'S OWN PALETTE ----------------------------------------
 *
 * See gta_front.h. The picture is quantised rather than pushed through
 * f_pal.raw, and the first GTA_FRONT_RESERVED entries are left to Intuition:
 *
 *    0  black          the screen's background and the bar's trim
 *    1  white          the bar's text, and the pointer's own white
 *    2  dark grey      the bar's fill
 *    3  mid grey
 *   17  white          the mouse pointer, which Intuition draws out of
 *   18  black          17..19 on an 8-bit screen whatever else is loaded
 *   19  light grey
 *
 * The rest of 0..19 are a small grey ramp, so anything else the system draws
 * lands on something sensible rather than on a piece of the artwork.
 */
#define FRONT_HIST_BITS 5
#define FRONT_HIST      (1 << (FRONT_HIST_BITS * 3))   /* 32768 */
#define FRONT_KEY(r, g, b) \
    ((((r) >> 3) << 10) | (((g) >> 3) << 5) | ((b) >> 3))

typedef struct {
    long n;                             /* pixels in the cell */
    long r, g, b;                       /* their sum */
} front_cell;

typedef struct {
    int lo, hi;                         /* range of `order` this box owns */
    long n;                             /* pixels in it */
    int wr, wg, wb;                     /* its extent, for the split */
} front_box;

static front_cell *front_hist;
static int *front_order;                /* cell indices, sorted per box */
static int front_sort_axis;

static int front_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a, ib = *(const int *)b;
    int va, vb;
    switch (front_sort_axis) {
    case 0:  va = (ia >> 10) & 31; vb = (ib >> 10) & 31; break;
    case 1:  va = (ia >> 5)  & 31; vb = (ib >> 5)  & 31; break;
    default: va = ia & 31;         vb = ib & 31;         break;
    }
    return va - vb;
}

/* The extent of a box and which axis is longest. */
static void front_extent(front_box *bx)
{
    int i, r0 = 31, r1 = 0, g0 = 31, g1 = 0, b0 = 31, b1 = 0;
    bx->n = 0;
    for (i = bx->lo; i <= bx->hi; i++) {
        int c = front_order[i];
        int r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
        if (r < r0) r0 = r;  if (r > r1) r1 = r;
        if (g < g0) g0 = g;  if (g > g1) g1 = g;
        if (b < b0) b0 = b;  if (b > b1) b1 = b;
        bx->n += front_hist[c].n;
    }
    /* Weighted the way the eye is - green carries most of the luminance, so
     * a box that is long in green is worth splitting before one long in
     * blue. Without it the sky and the rust share entries and the wall goes
     * flat. */
    bx->wr = (r1 - r0) * 3;
    bx->wg = (g1 - g0) * 6;
    bx->wb = (b1 - b0) * 1;
}

/* MEDIAN CUT down to `want` colours, written into pal[first..]. Returns how
 * many it actually made. */
static int front_quantise(unsigned char *pal, int first, int want)
{
    front_box *box;
    int n_box = 0, n_used = 0, i, k;

    box = (front_box *)calloc((size_t)want, sizeof *box);
    front_order = (int *)calloc(FRONT_HIST, sizeof *front_order);
    if (!box || !front_order) { free(box); free(front_order); return 0; }

    for (i = 0; i < FRONT_HIST; i++)
        if (front_hist[i].n)
            front_order[n_used++] = i;
    if (n_used <= 0) { free(box); free(front_order); return 0; }

    box[0].lo = 0;
    box[0].hi = n_used - 1;
    front_extent(&box[0]);
    n_box = 1;

    while (n_box < want) {
        /* The box worth splitting: the biggest extent, and among equals the
         * one with the most pixels in it. */
        int best = -1;
        long best_score = -1;
        for (i = 0; i < n_box; i++) {
            int w = box[i].wr > box[i].wg ? box[i].wr : box[i].wg;
            long score;
            if (box[i].wb > w) w = box[i].wb;
            if (box[i].hi <= box[i].lo || w <= 0)
                continue;
            score = (long)w * 1024 + (box[i].n > 1024 ? 1024 : box[i].n);
            if (score > best_score) { best_score = score; best = i; }
        }
        if (best < 0)
            break;
        /* Split it at the MEDIAN of its longest axis - the pixel count's
         * median, not the range's, which is what makes this median cut and
         * not a uniform subdivision. */
        {
            front_box *bx = &box[best];
            long half = bx->n / 2, run = 0;
            int cut;
            front_sort_axis = bx->wr >= bx->wg && bx->wr >= bx->wb ? 0
                            : (bx->wg >= bx->wb ? 1 : 2);
            qsort(front_order + bx->lo, (size_t)(bx->hi - bx->lo + 1),
                  sizeof *front_order, front_cmp);
            cut = bx->lo;
            for (i = bx->lo; i < bx->hi; i++) {
                run += front_hist[front_order[i]].n;
                cut = i;
                if (run >= half)
                    break;
            }
            box[n_box].lo = cut + 1;
            box[n_box].hi = bx->hi;
            bx->hi = cut;
            front_extent(bx);
            front_extent(&box[n_box]);
            n_box++;
        }
    }

    for (k = 0; k < n_box; k++) {
        long r = 0, g = 0, b = 0, n = 0;
        for (i = box[k].lo; i <= box[k].hi; i++) {
            int c = front_order[i];
            r += front_hist[c].r; g += front_hist[c].g; b += front_hist[c].b;
            n += front_hist[c].n;
        }
        if (n < 1) n = 1;
        pal[(first + k) * 3 + 0] = (unsigned char)(r / n);
        pal[(first + k) * 3 + 1] = (unsigned char)(g / n);
        pal[(first + k) * 3 + 2] = (unsigned char)(b / n);
    }
    free(box);
    free(front_order);
    front_order = 0;
    return n_box;
}

/* THE NEAREST ENTRY OF THE PALETTE to an 8-bit RGB triple. Linear search
 * over 256, which is nothing at build time and keeps the palette the
 * original's rather than inventing one. */
static int front_nearest(const unsigned char *pal, int r, int g, int b)
{
    int i, best = GTA_FRONT_RESERVED;
    long bd = -1;
    /* THE RESERVED ENTRIES ARE NOT CANDIDATES. They are the title bar's and
     * the pointer's, and a picture that maps onto them changes colour the
     * moment Intuition redraws either. */
    for (i = GTA_FRONT_RESERVED; i < 256; i++) {
        long dr = r - pal[i * 3], dg = g - pal[i * 3 + 1],
             db = b - pal[i * 3 + 2];
        long d = dr * dr + dg * dg + db * db;
        if (bd < 0 || d < bd) { bd = d; best = i; }
        if (d == 0) break;
    }
    return best;
}

/* One 24-bit source strip, downscaled, as 8-bit RGB in `rgb`.
 *
 * `dst_row0` is where this strip starts on the 200-row screen and `src_row0`
 * where it starts in the 480-row original; the source rows for one output
 * row are everything between this row's place and the next one's, which is
 * two or three of them, and they are AVERAGED - the art is dithered and
 * picking one pixel out of a dither is noise. */
static void front_scale(const unsigned char *src, int src_h, int src_row0,
                        int dst_row0, unsigned char *rgb, int rows)
{
    int y, x;
    for (y = 0; y < rows; y++) {
        int y0 = ((dst_row0 + y) * FRONT_SRC_H) / GTA_FRONT_H - src_row0;
        int y1 = ((dst_row0 + y + 1) * FRONT_SRC_H) / GTA_FRONT_H - src_row0;
        unsigned char *d = rgb + (long)y * GTA_FRONT_W * 3;
        if (y0 < 0) y0 = 0;
        if (y1 > src_h) y1 = src_h;
        if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < GTA_FRONT_W; x++) {
            long r = 0, g = 0, b = 0, n = 0;
            int sy, sx;
            for (sy = y0; sy < y1; sy++) {
                const unsigned char *s = src + ((long)sy * FRONT_SRC_W
                                                + x * 2) * 3;
                for (sx = 0; sx < 2; sx++) {
                    /* EIGHT BITS A COMPONENT, NOT SIX. Measured: the largest
                     * byte in f_logo0.raw and f_lower0.raw is 255. The first
                     * version scaled them as if they were VGA's 0..63 and
                     * everything brighter than 63 wrapped round in the byte -
                     * which is why the wall came out right and the lettering
                     * and the glow came out as confetti. */
                    r += s[sx * 3];
                    g += s[sx * 3 + 1];
                    b += s[sx * 3 + 2];
                    n++;
                }
            }
            d[x * 3 + 0] = (unsigned char)(r / n);
            d[x * 3 + 1] = (unsigned char)(g / n);
            d[x * 3 + 2] = (unsigned char)(b / n);
        }
    }
}

/* Count one scaled strip into the 5:5:5 histogram. */
static void front_count(const unsigned char *rgb, int rows)
{
    long i, n = (long)rows * GTA_FRONT_W;
    for (i = 0; i < n; i++) {
        int r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        front_cell *c = &front_hist[FRONT_KEY(r, g, b)];
        c->n++; c->r += r; c->g += g; c->b += b;
    }
}

/* ...and map it into the quantised palette, with an ordered dither. The
 * lookup is cached per 5:5:5 cell, so the 236-entry search runs at most
 * 32768 times for the whole title screen instead of once per pixel. */
static void front_map(const unsigned char *rgb, unsigned char *dst, int rows,
                      const unsigned char *pal, short *cache)
{
    static const signed char bayer[16] = {
        -10,   4,  -8,   6,
          8,  -6,  10,  -3,
         -7,   7,  -9,   3,
         11,  -2,   9,  -4 };
    int y, x;
    for (y = 0; y < rows; y++)
        for (x = 0; x < GTA_FRONT_W; x++) {
            long i = (long)y * GTA_FRONT_W + x;
            int t = bayer[(y & 3) * 4 + (x & 3)];
            int r = rgb[i * 3] + t, g = rgb[i * 3 + 1] + t,
                b = rgb[i * 3 + 2] + t;
            int key;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            key = FRONT_KEY(r, g, b);
            if (cache[key] < 0)
                cache[key] = (short)front_nearest(pal, r, g, b);
            dst[i] = (unsigned char)cache[key];
        }
}

/* THE RESERVED ENTRIES: what Intuition draws its title bar and its mouse
 * pointer with. A small grey ramp, so anything else the system paints lands
 * on something sensible rather than on a piece of the artwork. */
static void front_reserved(unsigned char *pal)
{
    static const unsigned char fixed[GTA_FRONT_RESERVED][3] = {
        {   0,   0,   0 },   /*  0 black - the screen's ground, the bar's trim */
        { 255, 255, 255 },   /*  1 white - the bar's text */
        {  72,  76,  88 },   /*  2 the bar's fill, a dark neutral */
        { 170, 170, 170 },   /*  3 */
        {  32,  32,  32 }, {  64,  64,  64 }, {  96,  96,  96 },
        { 128, 128, 128 }, { 160, 160, 160 }, { 192, 192, 192 },
        { 224, 224, 224 }, {  48,  48,  48 }, {  80,  80,  80 },
        { 112, 112, 112 }, { 144, 144, 144 }, { 176, 176, 176 },
        { 208, 208, 208 },
        { 255, 255, 255 },   /* 17 the mouse pointer, white */
        {   0,   0,   0 },   /* 18 the mouse pointer, black */
        { 187, 187, 187 }    /* 19 the mouse pointer, grey */
    };
    int i;
    for (i = 0; i < GTA_FRONT_RESERVED; i++) {
        pal[i * 3 + 0] = fixed[i][0];
        pal[i * 3 + 1] = fixed[i][1];
        pal[i * 3 + 2] = fixed[i][2];
    }
}

static int bake_front(const char *dir, const char *outpath)
{
    static unsigned char strip[FRONT_SRC_W * FRONT_LOW_H * 3];
    static unsigned char rgb_up[GTA_FRONT_W * GTA_FRONT_UP * 3];
    static unsigned char rgb_low[GTA_FRONT_W * GTA_FRONT_LOW * 3];
    static unsigned char out_up[GTA_FRONT_W * GTA_FRONT_UP];
    static unsigned char out_low[GTA_FRONT_W * GTA_FRONT_LOW];
    /* Every logo frame is scaled once, counted, and kept - the palette has to
     * be chosen for all eight together or the animation would shimmer as the
     * colours moved under it. */
    static unsigned char rgb_frames[GTA_FRONT_FRAMES]
                                   [GTA_FRONT_W * GTA_FRONT_UP * 3];
    unsigned char pal[768];
    unsigned char hdr[GTA_FRONT_HDR];
    short *cache;
    FILE *f;
    int i, ncol;

    front_hist = (front_cell *)calloc(FRONT_HIST, sizeof *front_hist);
    cache = (short *)malloc(FRONT_HIST * sizeof *cache);
    if (!front_hist || !cache) {
        fprintf(stderr, "gtabake: out of memory for the front end\n");
        free(front_hist); free(cache);
        return 1;
    }
    for (i = 0; i < FRONT_HIST; i++) cache[i] = -1;

    /* ---- pass one: scale everything and count its colours --------------- */
    for (i = 0; i < GTA_FRONT_FRAMES; i++) {
        char name[32];
        snprintf(name, sizeof name, "f_logo%d.raw", i);
        if (front_read(dir, name, strip,
                       (long)FRONT_SRC_W * FRONT_UP_H * 3)) {
            free(front_hist); free(cache);
            return 1;
        }
        front_scale(strip, FRONT_UP_H, 0, 0, rgb_frames[i], GTA_FRONT_UP);
        front_count(rgb_frames[i], GTA_FRONT_UP);
    }
    if (front_read(dir, "f_lower0.raw", strip,
                   (long)FRONT_SRC_W * FRONT_LOW_H * 3)) {
        free(front_hist); free(cache);
        return 1;
    }
    front_scale(strip, FRONT_LOW_H, FRONT_UP_H, GTA_FRONT_UP,
                rgb_low, GTA_FRONT_LOW);
    front_count(rgb_low, GTA_FRONT_LOW);

    /* ---- the palette ---------------------------------------------------- */
    memset(pal, 0, sizeof pal);
    front_reserved(pal);
    ncol = front_quantise(pal, GTA_FRONT_RESERVED, 256 - GTA_FRONT_RESERVED);
    if (ncol <= 0) {
        fprintf(stderr, "gtabake: the front end's art has no colours\n");
        free(front_hist); free(cache);
        return 1;
    }
    /* Any entry the cut did not reach repeats the last one it did, so a
     * nearest-colour search can never land on an uninitialised black. */
    for (i = GTA_FRONT_RESERVED + ncol; i < 256; i++) {
        pal[i * 3 + 0] = pal[(GTA_FRONT_RESERVED + ncol - 1) * 3 + 0];
        pal[i * 3 + 1] = pal[(GTA_FRONT_RESERVED + ncol - 1) * 3 + 1];
        pal[i * 3 + 2] = pal[(GTA_FRONT_RESERVED + ncol - 1) * 3 + 2];
    }

    /* ---- pass two: write ------------------------------------------------ */
    f = fopen(outpath, "wb");
    if (!f) {
        fprintf(stderr, "gtabake: cannot write %s\n", outpath);
        free(front_hist); free(cache);
        return 1;
    }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'G'; hdr[1] = 'T'; hdr[2] = 'A'; hdr[3] = 'F';
    hdr[4] = 1;
    hdr[5] = GTA_FRONT_FRAMES;
    hdr[6] = (unsigned char)(GTA_FRONT_W >> 8);
    hdr[7] = (unsigned char)(GTA_FRONT_W & 255);
    hdr[8] = (unsigned char)GTA_FRONT_UP;
    hdr[9] = (unsigned char)GTA_FRONT_LOW;
    fwrite(hdr, 1, sizeof hdr, f);
    fwrite(pal, 1, 768, f);

    for (i = 0; i < GTA_FRONT_FRAMES; i++) {
        front_map(rgb_frames[i], out_up, GTA_FRONT_UP, pal, cache);
        fwrite(out_up, 1, sizeof out_up, f);
    }
    front_map(rgb_low, out_low, GTA_FRONT_LOW, pal, cache);
    fwrite(out_low, 1, sizeof out_low, f);
    fclose(f);

    printf("front: %d logo frames %dx%d, background %dx%d, %d colours "
           "(%d..%d), %ld bytes\n",
           GTA_FRONT_FRAMES, GTA_FRONT_W, GTA_FRONT_UP,
           GTA_FRONT_W, GTA_FRONT_LOW, ncol,
           GTA_FRONT_RESERVED, GTA_FRONT_RESERVED + ncol - 1,
           (long)(GTA_FRONT_HDR + 768 +
                  (long)GTA_FRONT_FRAMES * GTA_FRONT_W * GTA_FRONT_UP +
                  (long)GTA_FRONT_W * GTA_FRONT_LOW));
    free(front_hist);
    free(cache);
    front_hist = 0;
    (void)rgb_up;
    return 0;
}

int main(int argc, char **argv)
{
    gta_style st;
    FILE *out;
    unsigned char hdr[GTA_TIL_DATA_OFF];
    unsigned char tile[GTA_TILE_AREA];
    unsigned char work[GTA_TILE_AREA];
    int n_side, n_lid, n_aux, i, r;
    long written = 0;
    int n_remaps = 0;
    long n_deltas = 0, n_delta_bytes = 0;
    unsigned long n_sprite_bytes = 0;

    if (argc == 4 && strcmp(argv[1], "-sfx") == 0) {
        /* THE SOUND BANK. Two input files from one prefix, because that is how
         * the original names them - level001.sdt beside level001.raw - and
         * asking the player to type both would be asking them to get one
         * wrong. */
        char sdt[512], raw[512];
        size_t n = strlen(argv[2]);
        if (n + 5 >= sizeof sdt) {
            fprintf(stderr, "gtabake: path too long\n");
            return 2;
        }
        memcpy(sdt, argv[2], n); memcpy(sdt + n, ".sdt", 5);
        memcpy(raw, argv[2], n); memcpy(raw + n, ".raw", 5);
        return gta_sfx_bake(sdt, raw, argv[3], stdout) == 0 ? 0 : 1;
    }

    if (argc == 4 && strcmp(argv[1], "-front") == 0)
        return bake_front(argv[2], argv[3]);

    if (argc != 3) {
        fprintf(stderr, "usage: %s <style.gry> <out.til>\n", argv[0]);
        fprintf(stderr, "       %s -sfx <audio/level001> <out.snd>\n", argv[0]);
        fprintf(stderr, "       %s -front <gtadata> <out.fnt>\n", argv[0]);
        return 2;
    }

    if (gta_style_load(argv[1], &st) != 0)
        return 1;

    n_side = gta_style_block_count(&st, GTA_BLOCK_SIDE);
    n_lid  = gta_style_block_count(&st, GTA_BLOCK_LID);
    n_aux  = gta_style_block_count(&st, GTA_BLOCK_AUX);

    out = fopen(argv[2], "wb");
    if (!out) {
        fprintf(stderr, "gtabake: cannot write %s\n", argv[2]);
        gta_style_free(&st);
        return 1;
    }

    memset(hdr, 0, sizeof(hdr));
    put_be32(hdr +  0, GTA_TIL_MAGIC);
    put_be32(hdr +  4, GTA_TIL_VERSION);
    put_be32(hdr +  8, (unsigned long)GTA_TILE_DIM);
    put_be32(hdr + 12, (unsigned long)n_side);
    put_be32(hdr + 16, (unsigned long)n_lid);
    put_be32(hdr + 20, (unsigned long)n_aux);
    put_be32(hdr + 24, (unsigned long)GTA_LID_ROTATIONS);
    put_be32(hdr + 28, (unsigned long)st.sprite_count);
    /* gta_style_load has already scaled the file's 6-bit VGA palette to 8 bits,
     * so what goes in here is what amigagfx_set_palette() wants. */
    memcpy(hdr + GTA_TIL_HDR, st.palette, GTA_TIL_PALETTE);
    fwrite(hdr, 1, sizeof(hdr), out);

    /* Order must match gta_tiles_load(): side, lid x4, aux.
     *
     * The transposed side section was dropped in .til version 4: the wall
     * blitter scans by column now and reads the normal tile contiguously, so
     * nothing ever looked at it. See GTA_TIL_VERSION. */
    for (i = 0; i < n_side; i++) {
        const unsigned char *t = fetch(&st, GTA_BLOCK_SIDE, i);
        if (!t) { memset(tile, 0, sizeof(tile)); t = tile; }
        fwrite(t, 1, GTA_TILE_AREA, out);
        written += GTA_TILE_AREA;
    }
    for (i = 0; i < n_lid; i++) {
        const unsigned char *t = fetch(&st, GTA_BLOCK_LID, i);
        if (!t) { memset(tile, 0, sizeof(tile)); t = tile; }
        for (r = 0; r < GTA_LID_ROTATIONS; r++) {
            rotate(t, work, r);
            fwrite(work, 1, GTA_TILE_AREA, out);
            written += GTA_TILE_AREA;
        }
    }
    for (i = 0; i < n_aux; i++) {
        const unsigned char *t = fetch(&st, GTA_BLOCK_AUX, i);
        if (!t) { memset(tile, 0, sizeof(tile)); t = tile; }
        fwrite(t, 1, GTA_TILE_AREA, out);
        written += GTA_TILE_AREA;
    }

    /* Sprites, at SOURCE scale - not halved like the blocks. gta_tiles.h says
     * why. The section is: a size, the 21 category counts, an index, then the
     * pixels; see the same header for the field layout. */
    {
        unsigned char sub[GTA_TIL_SPRHDR];
        unsigned char *entries;
        unsigned char *pixels;
        unsigned long total = 0, off = 0;

        for (i = 0; i < st.sprite_count; i++)
            total += (unsigned long)st.sprites[i].w * st.sprites[i].h;

        entries = (unsigned char *)malloc((size_t)st.sprite_count * GTA_TIL_SPRENTRY);
        pixels  = (unsigned char *)malloc((size_t)total);
        if (!entries || !pixels) {
            fprintf(stderr, "gtabake: out of memory for %lu sprite bytes\n", total);
            free(entries); free(pixels);
            fclose(out);
            gta_style_free(&st);
            return 1;
        }

        /* A sprite's transparent pixels are index 0, and gta_style_get_sprite
         * SKIPS those rather than writing them - it composites. So the
         * destination has to be cleared first or the previous sprite's pixels
         * show through the holes in this one. That is exactly the bug the
         * contact sheet in gtadump was already working around. */
        memset(pixels, 0, (size_t)total);

        for (i = 0; i < st.sprite_count; i++) {
            int w = st.sprites[i].w, h = st.sprites[i].h;
            unsigned char *e = entries + (long)i * GTA_TIL_SPRENTRY;
            e[0] = (unsigned char)(w >> 8); e[1] = (unsigned char)w;
            e[2] = (unsigned char)(h >> 8); e[3] = (unsigned char)h;
            put_be32(e + 4, off);
            if (w > 0 && h > 0)
                gta_style_get_sprite(&st, i, pixels + off, w);
            off += (unsigned long)w * h;
        }

        memset(sub, 0, sizeof(sub));
        put_be32(sub, total);
        for (i = 0; i < GTA_TIL_SPRITE_TYPES; i++)
            put_be32(sub + 4 + i * 4, (unsigned long)st.sprite_numbers[i]);

        fwrite(sub, 1, sizeof(sub), out);
        fwrite(entries, 1, (size_t)st.sprite_count * GTA_TIL_SPRENTRY, out);
        fwrite(pixels, 1, (size_t)total, out);
        written += (long)sizeof(sub) + (long)st.sprite_count * GTA_TIL_SPRENTRY
                 + (long)total;
        n_sprite_bytes = total;
        free(entries);
        free(pixels);
    }

    /* Cars, last. Fixed-size records so the Amiga can index the table instead
     * of walking it; gta_tiles.h has the layout and gta_car.c does the
     * packing, which is where the writer and the reader sit next to each other
     * so their field orders cannot drift apart. */
    {
        unsigned char n[4];
        unsigned char rec[GTA_TIL_CARREC];

        put_be32(n, (unsigned long)st.car_count);
        fwrite(n, 1, 4, out);
        for (i = 0; i < st.car_count; i++) {
            gta_car_pack(&st.cars[i], rec);
            fwrite(rec, 1, GTA_TIL_CARREC, out);
        }
        written += 4 + (long)st.car_count * GTA_TIL_CARREC;
    }

    /* The palette remap tables, after the cars. Without them the city is
     * monochrome: one pedestrian sheet for everybody and one colour per car
     * model. See gta_style.h for which ranges are cars and which are people. */
    {
        unsigned char n[4];
        long bytes = (long)st.remap_count * GTA_TIL_REMAP_STRIDE;

        put_be32(n, (unsigned long)st.remap_count);
        fwrite(n, 1, 4, out);
        if (st.remap_count > 0 && st.remaps)
            fwrite(st.remaps, 1, (size_t)bytes, out);
        written += 4 + bytes;
        n_remaps = st.remap_count;
    }

    /* THE SPRITE DELTAS, after the remaps. Open doors, damage panels, brake
     * lights - see gta_tiles.h for the layout and PROGRESS.md 111 for why the
     * record format is a fact rather than a reading.
     *
     * The streams are COPIED OUT of sprite_graphics rather than referenced,
     * so the baked file stays self-contained: the Amiga never opens a .GRY.
     * Offsets are rewritten to be relative to the copied blob. */
    {
        unsigned char n[8], rec[8], idx[4];
        long i, blob = 0;

        for (i = 0; i < st.delta_count; i++)
            blob += (long)st.deltas[i].size;

        put_be32(n,     (unsigned long)st.delta_count);
        put_be32(n + 4, (unsigned long)blob);
        fwrite(n, 1, 8, out);

        for (i = 0; i < st.sprite_count; i++) {
            int f = st.sprites[i].delta_first;
            put_be16(idx,     (unsigned)(f < 0 ? 0 : f));
            put_be16(idx + 2, (unsigned)st.sprites[i].delta_count);
            fwrite(idx, 1, 4, out);
        }

        {
            unsigned long at = 0;
            for (i = 0; i < st.delta_count; i++) {
                put_be32(rec,     at);
                put_be32(rec + 4, (unsigned long)st.deltas[i].size);
                fwrite(rec, 1, 8, out);
                at += st.deltas[i].size;
            }
        }

        for (i = 0; i < st.delta_count; i++) {
            unsigned long off = st.deltas[i].offset;
            unsigned long sz  = st.deltas[i].size;
            /* A record pointing outside sprite_graphics is a corrupt style
             * file. Write zeros rather than reading wild, and say so - a
             * silently shortened blob would desynchronise every later
             * offset. */
            if (sz == 0) continue;
            if (off > st.sprite_graphics_len ||
                sz > st.sprite_graphics_len - off) {
                long k;
                fprintf(stderr, "gtabake: delta %ld is outside the sprite "
                                "graphics (off %lu size %lu) - zeroed\n",
                        i, off, sz);
                for (k = 0; k < (long)sz; k++) fputc(0, out);
                continue;
            }
            fwrite(st.sprite_graphics + off, 1, (size_t)sz, out);
        }

        written += 8 + (long)st.sprite_count * 4
                     + (long)st.delta_count * 8 + blob;
        n_deltas = st.delta_count;
        n_delta_bytes = blob;
    }

    /* THE OBJECT TABLE, last (version 7). Sprite indices are RESOLVED here
     * - absolute, the way the car records carry theirs - so the Amiga never
     * needs the category sums. See gta_tiles.h for the record. */
    {
        unsigned char n[4], rec[GTA_TIL_OBJREC];
        int i;
        put_be32(n, (unsigned long)st.object_count);
        fwrite(n, 1, 4, out);
        for (i = 0; i < st.object_count; i++) {
            const struct gta_object_info *o = &st.objects[i];
            long w = o->w, h = o->h, d = o->depth;
            memset(rec, 0, sizeof rec);
            put_be16(rec, o->sprite_index < 0 ? 0xffffU
                                              : (unsigned)o->sprite_index);
            put_be16(rec + 2,  (unsigned)(w < 0 ? 0 : w > 65535 ? 65535 : w));
            put_be16(rec + 4,  (unsigned)(h < 0 ? 0 : h > 65535 ? 65535 : h));
            put_be16(rec + 6,  (unsigned)(d < 0 ? 0 : d > 65535 ? 65535 : d));
            put_be16(rec + 8,  (unsigned)(o->weight & 0xffff));
            put_be16(rec + 10, (unsigned)(o->aux & 0xffff));
            rec[12] = (unsigned char)o->status;
            rec[13] = (unsigned char)o->num_into;
            fwrite(rec, 1, GTA_TIL_OBJREC, out);
        }
        written += 4 + (long)st.object_count * GTA_TIL_OBJREC;
    }

    if (fclose(out) != 0) {
        fprintf(stderr, "gtabake: write failed on %s\n", argv[2]);
        gta_style_free(&st);
        return 1;
    }

    printf("gtabake: %s -> %s\n", argv[1], argv[2]);
    printf("  %d side, %d lid x%d rotations, %d aux\n",
           n_side, n_lid, GTA_LID_ROTATIONS, n_aux);
    printf("  %d palette remap tables\n", n_remaps);
    printf("  %ld sprite deltas, %ld stream bytes\n", n_deltas, n_delta_bytes);
    printf("  %d object types (bullet 0x4a -> sprite %d, splat 0xd -> %d)\n",
           st.object_count,
           st.object_count > 0x4a ? st.objects[0x4a].sprite_index : -1,
           st.object_count > 0x0d ? st.objects[0x0d].sprite_index : -1);
    printf("  %d sprites at source scale, %lu pixel bytes (%d ped from %d)\n",
           st.sprite_count, n_sprite_bytes,
           gta_style_sprite_count(&st, GTA_SPR_PED),
           gta_style_sprite_base(&st, GTA_SPR_PED));
    printf("  %ld tile bytes + %d header = %ld total\n",
           written, GTA_TIL_DATA_OFF, written + GTA_TIL_DATA_OFF);

    gta_style_free(&st);
    return 0;
}
