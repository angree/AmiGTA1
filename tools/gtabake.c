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

    if (argc != 3) {
        fprintf(stderr, "usage: %s <style.gry> <out.til>\n", argv[0]);
        fprintf(stderr, "       %s -sfx <audio/level001> <out.snd>\n", argv[0]);
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
