/* Loader for the baked tile set. See gta_tiles.h for the format and the
 * reasoning; this file only reads it.
 *
 * One allocation, one fread. That is the whole point of baking: on the Amiga
 * this replaces parsing a 2.7 MB .GRY, walking its paged block storage and
 * downscaling 380 blocks.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gta_tiles.h"

/* The file is big-endian on purpose (gta_tiles.h says why), so this is a plain
 * byte assembly rather than a cast - `unsigned long` is 4 bytes on m68k and 8
 * on the host, and reading a struct straight off disk would differ between
 * them. That exact mistake cost an afternoon in gta_map.c. */
static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

static unsigned short be16(const unsigned char *p)
{
    return (unsigned short)(((unsigned)p[0] << 8) | p[1]);
}

/* The sprite section, positioned by the caller immediately after aux.
 *
 * Every offset is validated against sprite_bytes before it is stored. That is
 * not defensive politeness: a sprite index is the one thing in this file the
 * renderer turns straight into a pointer, and on the Amiga a bad one is a
 * silent corruption of whatever else is in fast RAM rather than a crash. */
static int read_sprites(const char *path, FILE *f, gta_tiles *t)
{
    unsigned char sub[GTA_TIL_SPRHDR];
    unsigned char *idx;
    int i;

    if (fread(sub, 1, GTA_TIL_SPRHDR, f) != (size_t)GTA_TIL_SPRHDR) {
        fprintf(stderr, "gta_tiles: %s ends before its sprite header\n", path);
        return 1;
    }
    t->sprite_bytes = be32(sub);
    for (i = 0; i < GTA_TIL_SPRITE_TYPES; i++)
        t->sprite_numbers[i] = (int)be32(sub + 4 + i * 4);

    idx = (unsigned char *)malloc((size_t)t->n_sprites * GTA_TIL_SPRENTRY);
    t->sprites = (gta_tile_sprite *)
        malloc((size_t)t->n_sprites * sizeof(gta_tile_sprite));
    t->sprite_pixels = (unsigned char *)malloc((size_t)t->sprite_bytes);
    if (!idx || !t->sprites || !t->sprite_pixels) {
        fprintf(stderr, "gta_tiles: out of memory for %d sprites (%lu bytes)\n",
                t->n_sprites, t->sprite_bytes);
        free(idx);
        return 1;
    }

    if (fread(idx, 1, (size_t)t->n_sprites * GTA_TIL_SPRENTRY, f)
            != (size_t)t->n_sprites * GTA_TIL_SPRENTRY) {
        fprintf(stderr, "gta_tiles: %s ends inside the sprite index\n", path);
        free(idx);
        return 1;
    }
    for (i = 0; i < t->n_sprites; i++) {
        const unsigned char *e = idx + (long)i * GTA_TIL_SPRENTRY;
        unsigned long span;
        t->sprites[i].w   = be16(e);
        t->sprites[i].h   = be16(e + 2);
        t->sprites[i].off = be32(e + 4);
        span = (unsigned long)t->sprites[i].w * t->sprites[i].h;
        if (t->sprites[i].off + span > t->sprite_bytes) {
            fprintf(stderr, "gta_tiles: sprite %d runs past the end of the "
                            "section (%lu + %lu > %lu)\n",
                    i, t->sprites[i].off, span, t->sprite_bytes);
            free(idx);
            return 1;
        }
    }
    free(idx);

    if (fread(t->sprite_pixels, 1, (size_t)t->sprite_bytes, f)
            != (size_t)t->sprite_bytes) {
        fprintf(stderr, "gta_tiles: %s is short - wanted %lu sprite bytes\n",
                path, t->sprite_bytes);
        return 1;
    }
    return 0;
}

/* The car table, last section in the file. Fixed-size records, so unlike the
 * sprites this is a count and then a straight read - no index to decode and no
 * walk. gta_car.c unpacks each record; see gta_tiles.h for why the doors are
 * padded out to four. */
/* The palette remap tables - see gta_tiles.h. Absent or zero-length is fine
 * and simply means nothing gets recoloured; a short read is not. */
static int read_remaps(const char *path, FILE *f, gta_tiles *t)
{
    unsigned char n[4];
    size_t want;

    if (fread(n, 1, 4, f) != 4)
        return 0;                       /* an older file: no section at all */
    t->n_remaps = (int)be32(n);
    if (t->n_remaps <= 0 || t->n_remaps > 4096) {
        t->n_remaps = 0;
        return 0;
    }
    want = (size_t)t->n_remaps * GTA_TIL_REMAP_STRIDE;
    t->remaps = (unsigned char *)malloc(want);
    if (!t->remaps) {
        fprintf(stderr, "gta_tiles: out of memory for %d remaps\n",
                t->n_remaps);
        return 1;
    }
    if (fread(t->remaps, 1, want, f) != want) {
        fprintf(stderr, "gta_tiles: %s ends inside its remap tables\n", path);
        return 1;
    }
    return 0;
}

/* THE SPRITE DELTAS - see gta_tiles.h for the layout.
 *
 * Everything here is tolerant of absence, because a .til with no deltas is a
 * perfectly good tile set: cars are simply always drawn shut. What is NOT
 * tolerated is a short read, which means the file was truncated and every
 * later offset is a lie. */
static int read_deltas(const char *path, FILE *f, gta_tiles *t)
{
    unsigned char n[8];
    long i;
    size_t want;

    if (fread(n, 1, 8, f) != 8)
        return 0;                       /* an older file: no section at all */
    t->n_deltas    = (int)be32(n);
    t->delta_bytes = be32(n + 4);
    if (t->n_deltas <= 0 || t->delta_bytes == 0) {
        t->n_deltas = 0;
        t->delta_bytes = 0;
        return 0;
    }
    if (t->n_deltas > 65535) {
        fprintf(stderr, "gta_tiles: %s claims %d deltas\n", path, t->n_deltas);
        return 1;
    }

    t->delta_first = (unsigned short *)
        malloc((size_t)t->n_sprites * sizeof(unsigned short));
    t->delta_num = (unsigned short *)
        malloc((size_t)t->n_sprites * sizeof(unsigned short));
    t->delta_off = (unsigned long *)
        malloc((size_t)t->n_deltas * sizeof(unsigned long));
    t->delta_size = (unsigned long *)
        malloc((size_t)t->n_deltas * sizeof(unsigned long));
    t->delta_data = (unsigned char *)malloc((size_t)t->delta_bytes);
    if (!t->delta_first || !t->delta_num || !t->delta_off ||
        !t->delta_size || !t->delta_data) {
        fprintf(stderr, "gta_tiles: out of memory for %d deltas\n",
                t->n_deltas);
        return 1;
    }

    for (i = 0; i < t->n_sprites; i++) {
        unsigned char e[4];
        if (fread(e, 1, 4, f) != 4) {
            fprintf(stderr, "gta_tiles: %s ends inside its delta index\n",
                    path);
            return 1;
        }
        t->delta_first[i] = (unsigned short)be16(e);
        t->delta_num[i]   = (unsigned short)be16(e + 2);
    }
    for (i = 0; i < t->n_deltas; i++) {
        unsigned char e[8];
        if (fread(e, 1, 8, f) != 8) {
            fprintf(stderr, "gta_tiles: %s ends inside its delta records\n",
                    path);
            return 1;
        }
        t->delta_off[i]  = be32(e);
        t->delta_size[i] = be32(e + 4);
    }
    want = (size_t)t->delta_bytes;
    if (fread(t->delta_data, 1, want, f) != want) {
        fprintf(stderr, "gta_tiles: %s ends inside its delta streams\n", path);
        return 1;
    }
    return 0;
}

/* THE OBJECT TABLE (version 7), last in the file. Tolerant of a file that
 * ends before it only in the sense that the count read fails cleanly: the
 * version check has already refused anything older. */
static int read_objects(const char *path, FILE *f, gta_tiles *t)
{
    unsigned char n[4];
    unsigned char rec[GTA_TIL_OBJREC];
    int i;

    if (fread(n, 1, 4, f) != 4) {
        fprintf(stderr, "gta_tiles: %s ends before its object count\n", path);
        return 1;
    }
    t->n_objects = (int)be32(n);
    if (t->n_objects < 0 || t->n_objects > 4096) {
        fprintf(stderr, "gta_tiles: %s claims %d objects\n", path,
                t->n_objects);
        return 1;
    }
    if (t->n_objects == 0)
        return 0;
    t->objects = (gta_tile_object *)
        malloc((size_t)t->n_objects * sizeof(gta_tile_object));
    if (!t->objects) {
        fprintf(stderr, "gta_tiles: out of memory for %d objects\n",
                t->n_objects);
        return 1;
    }
    for (i = 0; i < t->n_objects; i++) {
        gta_tile_object *o = &t->objects[i];
        unsigned s;
        if (fread(rec, 1, GTA_TIL_OBJREC, f) != (size_t)GTA_TIL_OBJREC) {
            fprintf(stderr, "gta_tiles: %s ends inside object %d of %d\n",
                    path, i, t->n_objects);
            return 1;
        }
        s = be16(rec);
        o->sprite   = (s == 0xffffU || (int)s >= t->n_sprites) ? -1 : (short)s;
        o->w        = (unsigned short)be16(rec + 2);
        o->h        = (unsigned short)be16(rec + 4);
        o->depth    = (unsigned short)be16(rec + 6);
        o->weight   = (unsigned short)be16(rec + 8);
        o->aux      = (unsigned short)be16(rec + 10);
        o->status   = rec[12];
        o->num_into = rec[13];
    }
    return 0;
}

int gta_tiles_delta_count(const gta_tiles *t, int sprite)
{
    if (!t || !t->delta_num) return 0;
    if (sprite < 0 || sprite >= t->n_sprites) return 0;
    return (int)t->delta_num[sprite];
}

/* One overlay onto a buffer that ALREADY holds the base sprite. Split out of
 * gta_tiles_delta_apply() so a mask can lay several over one copy. */
static int delta_over(const gta_tiles *t, int sprite, int d,
                      unsigned char *dst)
{
    const unsigned char *str;
    unsigned long off, size;
    unsigned long pos = 0, cursor = 0;
    int w, h, runs = 0, rec;

    if (!t->delta_data || !t->delta_num) return 0;
    if (d < 0 || d >= (int)t->delta_num[sprite]) return 0;
    w = t->sprites[sprite].w;
    h = t->sprites[sprite].h;
    rec = (int)t->delta_first[sprite] + d;
    if (rec < 0 || rec >= t->n_deltas) return 0;
    off  = t->delta_off[rec];
    size = t->delta_size[rec];
    if (size == 0) return 0;
    if (off > t->delta_bytes || size > t->delta_bytes - off) return 0;
    str = t->delta_data + off;

    while (pos + 3 <= size) {
        unsigned long dest = (unsigned long)str[pos] | ((unsigned long)str[pos + 1] << 8);
        unsigned int len = str[pos + 2];
        unsigned long x, y, k;
        pos += 3;
        if (pos + len > size) break;
        cursor += dest;
        y = cursor / GTA_TIL_DELTA_PAGE;
        x = cursor % GTA_TIL_DELTA_PAGE;
        for (k = 0; k < len; k++) {
            if (x + k < (unsigned long)w && y < (unsigned long)h)
                dst[y * (unsigned long)w + x + k] = str[pos + k];
        }
        cursor += len;
        pos += len;
        runs++;
    }
    return runs;
}

int gta_tiles_delta_apply_mask(const gta_tiles *t, int sprite,
                               unsigned long mask, unsigned char *dst)
{
    const unsigned char *base;
    int w, h, d, runs = 0;

    if (!t || !dst || sprite < 0 || sprite >= t->n_sprites) return 0;
    w = t->sprites[sprite].w;
    h = t->sprites[sprite].h;
    base = t->sprite_pixels + t->sprites[sprite].off;
    memcpy(dst, base, (size_t)w * h);
    if (!mask) return 0;
    for (d = 0; d < 32; d++)
        if (mask & (1UL << d))
            runs += delta_over(t, sprite, d, dst);
    return runs;
}

int gta_tiles_delta_apply(const gta_tiles *t, int sprite, int d,
                          unsigned char *dst)
{
    const unsigned char *base;
    const unsigned char *str;
    unsigned long off, size;
    unsigned long pos = 0, cursor = 0;
    int w, h, runs = 0, rec;

    if (!t || !dst || sprite < 0 || sprite >= t->n_sprites) return 0;
    w = t->sprites[sprite].w;
    h = t->sprites[sprite].h;
    base = t->sprite_pixels + t->sprites[sprite].off;
    memcpy(dst, base, (size_t)w * h);

    if (!t->delta_data || !t->delta_num) return 0;
    if (d < 0 || d >= (int)t->delta_num[sprite]) return 0;
    rec = (int)t->delta_first[sprite] + d;
    if (rec < 0 || rec >= t->n_deltas) return 0;
    off  = t->delta_off[rec];
    size = t->delta_size[rec];
    if (size == 0) return 0;            /* an empty slot: base, unchanged */
    if (off > t->delta_bytes || size > t->delta_bytes - off) return 0;
    str = t->delta_data + off;

    /* Runs of {u16 dest_offset, u8 len, len bytes}. THE CURSOR STEPS IN UNITS
     * OF 256 - the .GRY's sprite page width - not the sprite's own width, so
     * a run that drops a line adds 256. Decoding it against `w` is
     * syntactically fine and smears the overlay diagonally; that is the whole
     * trap in this format. */
    while (pos + 3 <= size) {
        unsigned int step = (unsigned int)str[pos]
                          | ((unsigned int)str[pos + 1] << 8);
        unsigned int len  = str[pos + 2];
        unsigned long x, y;
        unsigned int k;

        pos += 3;
        if (len == 0 || pos + len > size) break;
        cursor += step;
        y = cursor / GTA_TIL_DELTA_PAGE;
        x = cursor % GTA_TIL_DELTA_PAGE;
        for (k = 0; k < len; k++) {
            if (y < (unsigned long)h && x + k < (unsigned long)w)
                dst[y * w + x + k] = str[pos + k];
        }
        cursor += len;
        pos += len;
        runs++;
    }
    return runs;
}

static int read_cars(const char *path, FILE *f, gta_tiles *t)
{
    unsigned char n[4];
    unsigned char rec[GTA_TIL_CARREC];
    int i;

    if (fread(n, 1, 4, f) != 4) {
        fprintf(stderr, "gta_tiles: %s ends before its car count\n", path);
        return 1;
    }
    t->n_cars = (int)be32(n);
    if (t->n_cars < 0 || t->n_cars > 4096) {
        fprintf(stderr, "gta_tiles: %s claims %d cars, which is not a number "
                        "of cars\n", path, t->n_cars);
        return 1;
    }
    if (t->n_cars == 0)
        return 0;

    t->cars = (gta_car_info *)malloc((size_t)t->n_cars * sizeof(gta_car_info));
    if (!t->cars) {
        fprintf(stderr, "gta_tiles: out of memory for %d cars\n", t->n_cars);
        return 1;
    }
    for (i = 0; i < t->n_cars; i++) {
        if (fread(rec, 1, GTA_TIL_CARREC, f) != (size_t)GTA_TIL_CARREC) {
            fprintf(stderr, "gta_tiles: %s ends inside car %d of %d\n",
                    path, i, t->n_cars);
            return 1;
        }
        gta_car_unpack(&t->cars[i], rec);
        /* A sprite index out of range would be drawn, and would draw whatever
         * happened to sit at that offset. Better to lose the car. */
        if (t->cars[i].sprite_index >= t->n_sprites)
            t->cars[i].sprite_index = -1;
    }
    return 0;
}

int gta_tiles_load(const char *path, gta_tiles *t)
{
    unsigned char hdr[GTA_TIL_DATA_OFF];
    FILE *f;
    unsigned long magic, version;
    long need, got;

    memset(t, 0, sizeof(*t));

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gta_tiles: cannot open %s\n", path);
        return 1;
    }
    if (fread(hdr, 1, GTA_TIL_DATA_OFF, f) != (size_t)GTA_TIL_DATA_OFF) {
        fprintf(stderr, "gta_tiles: %s is too short for a header\n", path);
        fclose(f);
        return 1;
    }

    magic   = be32(hdr + 0);
    version = be32(hdr + 4);
    if (magic != GTA_TIL_MAGIC) {
        fprintf(stderr, "gta_tiles: %s is not a .til (magic %08lx)\n",
                path, magic);
        fclose(f);
        return 1;
    }
    if (version != GTA_TIL_VERSION) {
        fprintf(stderr, "gta_tiles: %s is version %lu, this build wants %lu\n",
                path, version, (unsigned long)GTA_TIL_VERSION);
        fclose(f);
        return 1;
    }

    t->dim    = (int)be32(hdr + 8);
    t->n_side = (int)be32(hdr + 12);
    t->n_lid  = (int)be32(hdr + 16);
    t->n_aux  = (int)be32(hdr + 20);
    t->n_sprites = (int)be32(hdr + 28);

    /* The renderer's blits are written around a 32x32 tile with a shift, not a
     * multiply. A file baked at another size would draw garbage rather than
     * fail, so it is rejected here instead. */
    if (t->dim != GTA_TILE_DIM) {
        fprintf(stderr, "gta_tiles: %s holds %dx%d tiles, this build wants %d\n",
                path, t->dim, t->dim, GTA_TILE_DIM);
        fclose(f);
        return 1;
    }
    if (t->n_side < 0 || t->n_lid < 0 || t->n_aux < 0) {
        fprintf(stderr, "gta_tiles: %s has a negative tile count\n", path);
        fclose(f);
        return 1;
    }

    memcpy(t->palette, hdr + GTA_TIL_HDR, GTA_TIL_PALETTE);

    /* side + lid x rotations + aux. No transposed copy since version 4 - see
     * GTA_TIL_VERSION. */
    need = ((long)t->n_side + (long)t->n_lid * GTA_LID_ROTATIONS +
            (long)t->n_aux) * GTA_TILE_AREA;

    t->data = (unsigned char *)malloc((size_t)need);
    if (!t->data) {
        fprintf(stderr, "gta_tiles: cannot allocate %ld bytes of tiles\n", need);
        fclose(f);
        return 1;
    }

    got = (long)fread(t->data, 1, (size_t)need, f);
    if (got != need) {
        fprintf(stderr, "gta_tiles: %s is short - wanted %ld tile bytes, got %ld\n",
                path, need, got);
        fclose(f);
        free(t->data);
        t->data = NULL;
        return 1;
    }

    t->side   = t->data;
    t->lid    = t->side   + (long)t->n_side * GTA_TILE_AREA;
    t->aux    = t->lid    + (long)t->n_lid * GTA_LID_ROTATIONS * GTA_TILE_AREA;

    /* Which side tiles have no transparent pixel - see gta_tiles.side_opaque.
     * A failure to allocate is not fatal: the macro reads it as "not opaque",
     * which is the old behaviour and merely slower. */
    t->side_opaque = (unsigned char *)malloc((size_t)(t->n_side > 0 ? t->n_side : 1));
    if (t->side_opaque) {
        int ti;
        for (ti = 0; ti < t->n_side; ti++) {
            const unsigned char *px = t->side + (long)ti * GTA_TILE_AREA;
            long k;
            unsigned char op = 1;
            for (k = 0; k < GTA_TILE_AREA; k++)
                if (px[k] == 0) { op = 0; break; }
            t->side_opaque[ti] = op;
        }
    }

    /* The sprite section follows aux and is read separately: it is two
     * variable-length arrays rather than a fixed grid, and the index has to be
     * decoded byte-wise for the same reason the header is (gta_tiles.h). */
    if (t->n_sprites > 0 && read_sprites(path, f, t) != 0) {
        fclose(f);
        gta_tiles_free(t);
        return 1;
    }

    if (read_cars(path, f, t) != 0) {
        fclose(f);
        gta_tiles_free(t);
        return 1;
    }

    if (read_remaps(path, f, t) != 0) {
        fclose(f);
        gta_tiles_free(t);
        return 1;
    }

    if (read_deltas(path, f, t) != 0) {
        fclose(f);
        gta_tiles_free(t);
        return 1;
    }

    if (read_objects(path, f, t) != 0) {
        fclose(f);
        gta_tiles_free(t);
        return 1;
    }

    fclose(f);
    return 0;
}

int gta_tiles_sprite_base(const gta_tiles *t, int type)
{
    int i, base = 0;
    if (type < 0 || type >= GTA_TIL_SPRITE_TYPES) return 0;
    for (i = 0; i < type; i++)
        base += t->sprite_numbers[i];
    return base;
}

int gta_tiles_sprite_count(const gta_tiles *t, int type)
{
    if (type < 0 || type >= GTA_TIL_SPRITE_TYPES) return 0;
    return t->sprite_numbers[type];
}

void gta_tiles_free(gta_tiles *t)
{
    if (t->data)
        free(t->data);
    if (t->side_opaque)
        free(t->side_opaque);
    if (t->sprites)
        free(t->sprites);
    if (t->sprite_pixels)
        free(t->sprite_pixels);
    if (t->cars)
        free(t->cars);
    if (t->remaps)
        free(t->remaps);
    if (t->delta_first)
        free(t->delta_first);
    if (t->delta_num)
        free(t->delta_num);
    if (t->delta_off)
        free(t->delta_off);
    if (t->delta_size)
        free(t->delta_size);
    if (t->delta_data)
        free(t->delta_data);
    if (t->objects)
        free(t->objects);
    memset(t, 0, sizeof(*t));
}

void gta_tiles_describe(const gta_tiles *t, FILE *out)
{
    long bytes = ((long)t->n_side + (long)t->n_lid * GTA_LID_ROTATIONS +
                  (long)t->n_aux) * GTA_TILE_AREA;
    fprintf(out, "tiles: %dx%d, %d side, %d lid (x%d rotations), "
                 "%d aux, %ld bytes\n",
            t->dim, t->dim, t->n_side, t->n_lid, GTA_LID_ROTATIONS,
            t->n_aux, bytes);
    fprintf(out, "sprites: %d, %lu bytes (source scale, %d ped from %d)\n",
            t->n_sprites, t->sprite_bytes,
            gta_tiles_sprite_count(t, 7), gta_tiles_sprite_base(t, 7));
    fprintf(out, "cars: %d definitions (%d bytes each)\n",
            t->n_cars, GTA_TIL_CARREC);
    /* Printed on the Amiga too, so gta.log proves the delta section was read
     * rather than merely present in the file. A version-5 .til reports 0
     * here, which is a working tile set with every car door shut. */
    fprintf(out, "deltas: %d records, %lu stream bytes\n",
            t->n_deltas, t->delta_bytes);
}
