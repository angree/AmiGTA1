/* GTA 1 ".GRY" style reader.  See gta_style.h for why this is ours rather
 * than borrowed, and for the licence/attribution note. */

#include <stdlib.h>
#include <string.h>

#include "gta_style.h"

/* The files are little-endian; the 68k is not.  Read byte-wise so the same
 * source works on both without a byte-swap #ifdef. */
static int read_u32le(FILE *f, unsigned long *out)
{
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4)
        return -1;
    *out = (unsigned long)b[0]
         | ((unsigned long)b[1] << 8)
         | ((unsigned long)b[2] << 16)
         | ((unsigned long)b[3] << 24);
    return 0;
}

static int read_header(FILE *f, gta_style_header *h)
{
    /* Assigned rather than brace-initialised: C89 wants constant expressions
     * even for automatic aggregates, and the Amiga toolchain is stricter about
     * it than the host compiler. */
    unsigned long *fields[13];
    int i;

    fields[0]  = &h->version;
    fields[1]  = &h->side_size;
    fields[2]  = &h->lid_size;
    fields[3]  = &h->aux_size;
    fields[4]  = &h->anim_size;
    fields[5]  = &h->palette_size;
    fields[6]  = &h->unknown_a;
    fields[7]  = &h->unknown_b;
    fields[8]  = &h->object_info_size;
    fields[9]  = &h->car_size;
    fields[10] = &h->sprite_info_size;
    fields[11] = &h->sprite_graphics_size;
    fields[12] = &h->sprite_numbers_size;

    for (i = 0; i < 13; i++) {
        if (read_u32le(f, fields[i]) != 0)
            return -1;
    }
    return 0;
}

static int read_u16le(FILE *f, unsigned short *out)
{
    unsigned char b[2];
    if (fread(b, 1, 2, f) != 2)
        return -1;
    *out = (unsigned short)(b[0] | (b[1] << 8));
    return 0;
}

/* Signed little-endian reads, for the car table. Byte-wise like their
 * unsigned cousins - the 68k has no unaligned 32-bit load either. The sign is
 * put back by hand rather than by casting an unsigned to a short, because that
 * cast is implementation-defined when the value does not fit. */
static int read_s16le(FILE *f, short *out)
{
    unsigned short u;
    if (read_u16le(f, &u) != 0) return -1;
    *out = (short)(u < 0x8000U ? (int)u : (int)u - 0x10000);
    return 0;
}

static int read_s32le(FILE *f, long *out)
{
    unsigned long u;
    if (read_u32le(f, &u) != 0) return -1;
    *out = (u < 0x80000000UL) ? (long)u : -(long)(0xFFFFFFFFUL - u) - 1L;
    return 0;
}

static int read_u8(FILE *f, unsigned char *out)
{
    int c = fgetc(f);
    if (c == EOF) return -1;
    *out = (unsigned char)c;
    return 0;
}

static int read_s8(FILE *f, signed char *out)
{
    unsigned char b;
    if (read_u8(f, &b) != 0) return -1;
    *out = (signed char)(b < 0x80U ? (int)b : (int)b - 256);
    return 0;
}

gta_sprite_type gta_vehicle_sprite_type(int vtype)
{
    switch (vtype) {
    case GTA_VEH_BUS:        return GTA_SPR_BUS;
    case GTA_VEH_JUGG_FRONT:                        /* both halves of an */
    case GTA_VEH_JUGG_BACK:  return GTA_SPR_CAR;    /* artic lorry are cars */
    case GTA_VEH_CAR:        return GTA_SPR_CAR;
    case GTA_VEH_BIKE:       return GTA_SPR_BIKE;
    case GTA_VEH_TRAIN:      return GTA_SPR_TRAIN;
    case GTA_VEH_TRAM:       return GTA_SPR_TRAM;
    case GTA_VEH_BOAT:       return GTA_SPR_BOAT;
    case GTA_VEH_TANK:       return GTA_SPR_TANK;
    default:                 return GTA_SPR_TYPE_COUNT;
    }
}

const char *gta_vehicle_class_name(int vtype)
{
    switch (vtype) {
    case GTA_VEH_BUS:        return "bus";
    case GTA_VEH_JUGG_FRONT: return "jugg-front";
    case GTA_VEH_JUGG_BACK:  return "jugg-back";
    case GTA_VEH_BIKE:       return "bike";
    case GTA_VEH_CAR:        return "car";
    case GTA_VEH_TRAIN:      return "train";
    case GTA_VEH_TRAM:       return "tram";
    case GTA_VEH_BOAT:       return "boat";
    case GTA_VEH_TANK:       return "tank";
    default:                 return "?";
    }
}

/* One car record. Returns the number of BYTES consumed, or -1.
 *
 * `out` may be NULL, which is the counting pass: a record carries its doors
 * inline so the section can only be walked, not indexed - exactly like
 * sprite_info below. */
static long read_one_car(FILE *f, gta_car_info *out)
{
    gta_car_info c;
    long start = ftell(f);
    long end;
    int i;

    memset(&c, 0, sizeof c);

    /* width, length, vert - see the header for why the middle one is the
     * one that runs along the road and not the vertical. */
    if (read_s16le(f, &c.width)  != 0) return -1;
    if (read_s16le(f, &c.length) != 0) return -1;
    if (read_s16le(f, &c.vert)   != 0) return -1;

    if (read_s16le(f, &c.sprite_num) != 0) return -1;
    if (read_s16le(f, &c.weight)     != 0) return -1;
    if (read_s16le(f, &c.max_speed)  != 0) return -1;
    if (read_s16le(f, &c.min_speed)  != 0) return -1;
    if (read_s16le(f, &c.accel)      != 0) return -1;
    if (read_s16le(f, &c.braking)    != 0) return -1;
    if (read_s16le(f, &c.grip)       != 0) return -1;
    if (read_s16le(f, &c.handling)   != 0) return -1;

    /* The 24-bit HLS remaps, 12 triples of i16. Useless to an 8-bit port. */
    if (fseek(f, GTA_CAR_REMAPS * 3 * 2, SEEK_CUR) != 0) return -1;

    /* The 8-bit remaps - the twelve bytes Carnage3D skips and we keep. */
    if (fread(c.remap8, 1, GTA_CAR_REMAPS, f) != GTA_CAR_REMAPS) return -1;

    if (read_u8(f, &c.vtype)     != 0) return -1;
    if (read_u8(f, &c.model_id)  != 0) return -1;
    if (read_s8(f, &c.turning)   != 0) return -1;
    if (read_s8(f, &c.damagable) != 0) return -1;

    for (i = 0; i < 4; i++)
        if (read_s16le(f, &c.value[i]) != 0) return -1;

    if (read_s8(f, &c.cx) != 0) return -1;
    if (read_s8(f, &c.cy) != 0) return -1;
    if (read_s32le(f, &c.moment) != 0) return -1;

    /* Already 16.16 - see the header comment. No conversion, deliberately. */
    if (read_s32le(f, &c.mass)               != 0) return -1;
    if (read_s32le(f, &c.thrust)             != 0) return -1;
    if (read_s32le(f, &c.tyre_adhesion_x)    != 0) return -1;
    if (read_s32le(f, &c.tyre_adhesion_y)    != 0) return -1;
    if (read_s32le(f, &c.handbrake_friction) != 0) return -1;
    if (read_s32le(f, &c.footbrake_friction) != 0) return -1;
    if (read_s32le(f, &c.front_brake_bias)   != 0) return -1;

    if (read_s16le(f, &c.turn_ratio)            != 0) return -1;
    if (read_s16le(f, &c.drive_wheel_offset)    != 0) return -1;
    if (read_s16le(f, &c.steering_wheel_offset) != 0) return -1;

    if (read_s32le(f, &c.back_end_slide)  != 0) return -1;
    if (read_s32le(f, &c.handbrake_slide) != 0) return -1;

    if (read_u8(f, &c.convertible)    != 0) return -1;
    if (read_u8(f, &c.engine)         != 0) return -1;
    if (read_u8(f, &c.radio)          != 0) return -1;
    if (read_u8(f, &c.horn)           != 0) return -1;
    if (read_u8(f, &c.sound_function) != 0) return -1;
    if (read_u8(f, &c.fast_change)    != 0) return -1;

    if (read_s16le(f, &c.n_doors) != 0) return -1;
    if (c.n_doors < 0 || c.n_doors > GTA_CAR_DOORS) {
        fprintf(stderr, "gta_style: a car claims %d doors, outside 0..%d - "
                        "the record layout is wrong\n",
                (int)c.n_doors, GTA_CAR_DOORS);
        return -1;
    }
    for (i = 0; i < c.n_doors; i++) {
        if (read_s16le(f, &c.doors[i].rpy)    != 0) return -1;
        if (read_s16le(f, &c.doors[i].rpx)    != 0) return -1;
        if (read_s16le(f, &c.doors[i].object) != 0) return -1;
        if (read_s16le(f, &c.doors[i].delta)  != 0) return -1;
    }

    c.sprite_index = -1;            /* resolved once sprite_numbers is read */

    end = ftell(f);
    if (start < 0 || end < 0) return -1;
    if (out) *out = c;
    return end - start;
}

/* The car section. Two passes over the same bytes: count, then fill.
 *
 * The section MUST be consumed to the byte. That is the ONLY check there is
 * that the record layout is right - a layout wrong by one field would
 * otherwise yield a plausible table of nonsense numbers instead of an error,
 * and this project has already been bitten once by reusing a .G24 record
 * layout on a .GRY file (the sprite record, which desynchronised after 38
 * entries). */
static int read_cars(FILE *f, gta_style *st)
{
    long section_start = ftell(f);
    long remaining;
    int pass, n;

    if (section_start < 0) return -1;
    if (st->hdr.car_size == 0) return 0;

    for (pass = 0; pass < 2; pass++) {
        if (fseek(f, section_start, SEEK_SET) != 0) return -1;
        remaining = (long)st->hdr.car_size;
        n = 0;

        while (remaining > 0) {
            long used;
            /* The two passes read the same bytes and must therefore agree on
             * the count. Guarded anyway: a write past the array would be a
             * heap corruption chasing a parse bug, and this project already
             * lost an evening to one of those in the sibling port. */
            if (pass == 1 && n >= st->car_count) return -1;
            used = read_one_car(f, (pass == 1) ? &st->cars[n] : NULL);
            if (used <= 0) {
                fprintf(stderr, "gta_style: car table broke after %d records, "
                                "%ld bytes left of %lu\n",
                        n, remaining, st->hdr.car_size);
                return -1;
            }
            remaining -= used;
            n++;
        }

        if (remaining != 0) {
            fprintf(stderr, "gta_style: car table overran by %ld bytes after "
                            "%d records - the record layout is wrong\n",
                    -remaining, n);
            return -1;
        }

        if (pass == 0) {
            st->car_count = n;
            if (n == 0) return 0;
            st->cars = (gta_car_info *)malloc((size_t)n * sizeof *st->cars);
            if (!st->cars) {
                fprintf(stderr, "gta_style: out of memory for %d cars\n", n);
                return -1;
            }
        }
    }

    /* Leave the file exactly where the section ends, so the caller's sequence
     * of fseeks reads the same whether this parsed anything or not. */
    if (fseek(f, section_start + (long)st->hdr.car_size, SEEK_SET) != 0)
        return -1;
    return 0;
}

/* Sprite records are variable-length - 12 bytes plus 6 per delta - so the
 * section can only be walked, not indexed. Two passes: count, then fill. */
static int read_sprite_info(FILE *f, gta_style *st)
{
    long section_start = ftell(f);
    long remaining;
    int pass, n;

    if (section_start < 0) return -1;
    if (st->hdr.sprite_info_size == 0) return 0;

    for (pass = 0; pass < 2; pass++) {
        if (fseek(f, section_start, SEEK_SET) != 0) return -1;
        remaining = (long)st->hdr.sprite_info_size;
        n = 0;

        while (remaining >= 10) {
            unsigned char b[8];
            unsigned short size, page;
            int d;

            if (fread(b, 1, 4, f) != 4) return -1;      /* w, h, deltas, pad */
            if (read_u16le(f, &size) != 0) return -1;
            if (fread(b + 4, 1, 2, f) != 2) return -1;  /* page_x, page_y */
            if (read_u16le(f, &page) != 0) return -1;
            remaining -= 10;

            /* size == w*h is the invariant that identifies a good record.
             * Checking it turns a desynchronised walk into an error instead of
             * thousands of garbage sprites. */
            if (pass == 0 && (unsigned)size != (unsigned)b[0] * b[1]) {
                fprintf(stderr, "gta_style: sprite %d has size %u but is "
                                "%ux%u - the record layout is wrong\n",
                        n, (unsigned)size, (unsigned)b[0], (unsigned)b[1]);
                return -1;
            }

            if (pass == 1) {
                st->sprites[n].w = b[0];
                st->sprites[n].h = b[1];
                st->sprites[n].delta_count = b[2];
                st->sprites[n].size = size;
                st->sprites[n].page_x = b[4];
                st->sprites[n].page_y = b[5];
                st->sprites[n].page = page;
            }

            for (d = 0; d < (int)b[2]; d++) {
                if (remaining < 6) {
                    fprintf(stderr, "gta_style: sprite %d claims %d deltas but "
                                    "the section ends early\n", n, (int)b[2]);
                    return -1;
                }
                if (fseek(f, 6, SEEK_CUR) != 0) return -1;
                remaining -= 6;
            }
            n++;
        }

        if (pass == 0) {
            st->sprite_count = n;
            st->sprites = (struct gta_sprite *)
                malloc((size_t)n * sizeof(struct gta_sprite));
            if (!st->sprites) {
                fprintf(stderr, "gta_style: out of memory for %d sprites\n", n);
                return -1;
            }
        }
    }

    /* Land exactly on the end of the section, whatever the walk did. */
    if (fseek(f, section_start + (long)st->hdr.sprite_info_size, SEEK_SET) != 0)
        return -1;
    return 0;
}

const char *const gta_sprite_type_name[GTA_SPR_TYPE_COUNT] = {
    "arrow", "digit", "boat", "box", "bus", "car", "object", "ped",
    "speedo", "tank", "traffic_light", "train", "trdoor", "bike", "tram",
    "wrecked_car", "wbus", "ex", "tumcar", "tumtruck", "ferry"
};

/* The last section of the file: one u16 count per category. It is small and it
 * is the only index into the sprite array, so a short or oversized section is
 * a warning rather than a failure - the blocks still render without it. */
static int read_sprite_numbers(FILE *f, gta_style *st)
{
    int i;
    unsigned long want = (unsigned long)GTA_SPR_TYPE_COUNT * 2UL;

    if (st->hdr.sprite_numbers_size < want) {
        fprintf(stderr, "gta_style: sprite_numbers is %lu bytes, expected %lu "
                        "- sprite categories unavailable\n",
                st->hdr.sprite_numbers_size, want);
        return 0;
    }
    for (i = 0; i < GTA_SPR_TYPE_COUNT; i++) {
        unsigned short v;
        if (read_u16le(f, &v) != 0) {
            fprintf(stderr, "gta_style: short read on sprite_numbers\n");
            return 0;
        }
        st->sprite_numbers[i] = (int)v;
    }
    return 0;
}

int gta_style_sprite_base(const gta_style *st, gta_sprite_type type)
{
    int i, base = 0;
    if ((int)type < 0 || (int)type >= GTA_SPR_TYPE_COUNT) return 0;
    for (i = 0; i < (int)type; i++)
        base += st->sprite_numbers[i];
    return base;
}

int gta_style_sprite_count(const gta_style *st, gta_sprite_type type)
{
    if ((int)type < 0 || (int)type >= GTA_SPR_TYPE_COUNT) return 0;
    return st->sprite_numbers[type];
}

int gta_style_load(const char *path, gta_style *st)
{
    FILE *f;
    long file_size;
    unsigned long sum;
    int extra_blocks;

    memset(st, 0, sizeof *st);

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gta_style: cannot open %s\n", path);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) goto fail;
    file_size = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) goto fail;

    if (read_header(f, &st->hdr) != 0) {
        fprintf(stderr, "gta_style: %s is too short for a header\n", path);
        goto fail;
    }

    if (st->hdr.version != GTA_STYLE_VERSION_GRY) {
        fprintf(stderr, "gta_style: %s has version %lu, expected %d (.GRY).\n"
                        "           Version 336 is a .G24 (24-bit) file; this "
                        "reader handles the 8-bit styles only.\n",
                path, st->hdr.version, GTA_STYLE_VERSION_GRY);
        goto fail;
    }

    st->side_blocks = (int)(st->hdr.side_size / GTA_BLOCK_AREA);
    st->lid_blocks  = (int)(st->hdr.lid_size  / GTA_BLOCK_AREA);
    st->aux_blocks  = (int)(st->hdr.aux_size  / GTA_BLOCK_AREA);

    /* Blocks live in 256x256 pages of 4x4 blocks, and the file pads the tail
     * of aux so the block count is a multiple of 4 - i.e. so the last page row
     * is complete.  The padding is present in the data but not in aux_size. */
    st->total_blocks = st->side_blocks + st->lid_blocks + st->aux_blocks;
    extra_blocks = (st->total_blocks % 4) ? (4 - (st->total_blocks % 4)) : 0;

    /* The twelve section sizes, THE TILE PADDING, and this 52-byte header must
     * account for the whole file.  If they do not, the header layout above is
     * wrong, and every section offset derived from it would be wrong too - so
     * refuse loudly rather than render garbage.
     *
     * THE PADDING TERM WAS MISSING AND IT LOCKED THE PORT INTO LIBERTY CITY.
     * style001 happens to have a block count that is already a multiple of
     * four, so the sum matched and nobody noticed; style002 is two blocks
     * short of a page row and style003 three, which is 8192 and 12288 bytes
     * of padding that the header does not count. This check ran BEFORE
     * `extra_blocks` was worked out, so both files were rejected as
     * "header layout is wrong" - and every byte after the tile block,
     * including the whole car table, sits at an offset that only comes out
     * right once the padding is allowed for. */
    sum = st->hdr.side_size + st->hdr.lid_size + st->hdr.aux_size
        + (unsigned long)extra_blocks * GTA_BLOCK_AREA
        + st->hdr.anim_size + st->hdr.palette_size
        + st->hdr.unknown_a + st->hdr.unknown_b
        + st->hdr.object_info_size + st->hdr.car_size
        + st->hdr.sprite_info_size + st->hdr.sprite_graphics_size
        + st->hdr.sprite_numbers_size;
    if (sum + 52UL != (unsigned long)file_size) {
        fprintf(stderr, "gta_style: %s section sizes sum to %lu + 52 header "
                        "= %lu, but the file is %ld bytes. Header layout is "
                        "wrong.\n", path, sum, sum + 52UL, file_size);
        goto fail;
    }

    st->blocks_len = (unsigned long)(st->total_blocks + extra_blocks) * GTA_BLOCK_AREA;
    st->blocks = (unsigned char *)malloc(st->blocks_len);
    if (!st->blocks) {
        fprintf(stderr, "gta_style: out of memory for %lu bytes of blocks\n",
                st->blocks_len);
        goto fail;
    }
    if (fread(st->blocks, 1, st->blocks_len, f) != st->blocks_len) {
        fprintf(stderr, "gta_style: short read on block data\n");
        goto fail;
    }

    /* anim, then the palette. */
    if (fseek(f, (long)st->hdr.anim_size, SEEK_CUR) != 0) goto fail;

    if (st->hdr.palette_size != sizeof st->palette) {
        fprintf(stderr, "gta_style: palette section is %lu bytes, expected %u\n",
                st->hdr.palette_size, (unsigned)sizeof st->palette);
        goto fail;
    }
    if (fread(st->palette, 1, sizeof st->palette, f) != sizeof st->palette) {
        fprintf(stderr, "gta_style: short read on palette\n");
        goto fail;
    }

    /* DOS-era palettes are usually 6-bit VGA DAC values (0..63). Detect rather
     * than assume: a genuine 8-bit palette will almost certainly exceed 63
     * somewhere, and scaling one that does not would only brighten a very dark
     * image, which is visible and therefore self-correcting. */
    {
        int i, max = 0;
        for (i = 0; i < (int)sizeof st->palette; i++) {
            if (st->palette[i] > max) max = st->palette[i];
        }
        if (max <= 63) {
            st->palette_was_6bit = 1;
            for (i = 0; i < (int)sizeof st->palette; i++) {
                /* 6->8 bits by replicating the top two bits, not <<2, so 63
                 * maps to 255 rather than 252. */
                unsigned int v = st->palette[i];
                st->palette[i] = (unsigned char)((v << 2) | (v >> 4));
            }
        }
    }

    /* Sections run in header-field order. Skip the three we do not interpret
     * yet to reach the sprites. unknown_a/unknown_b sit where the 24-bit format
     * keeps its CLUT pages and palette index; whatever they hold, their sizes
     * are what put the sprite sections at the right offset. */
    /* unknown_a is the remap table block - 256 x 256 bytes. Read rather than
     * skipped since 2026-08-27; see gta_style.h. A file whose block is not the
     * expected size is not an error, it just gets no remaps. */
    if (st->hdr.unknown_a == (unsigned long)GTA_REMAP_COUNT * 256) {
        st->remaps = (unsigned char *)malloc((size_t)GTA_REMAP_COUNT * 256);
        if (!st->remaps) goto fail;
        if (fread(st->remaps, 1, (size_t)GTA_REMAP_COUNT * 256, f)
                != (size_t)GTA_REMAP_COUNT * 256) goto fail;
        st->remap_count = GTA_REMAP_COUNT;
    } else {
        if (fseek(f, (long)st->hdr.unknown_a, SEEK_CUR) != 0) goto fail;
    }
    if (fseek(f, (long)st->hdr.unknown_b, SEEK_CUR) != 0) goto fail;
    if (fseek(f, (long)st->hdr.object_info_size, SEEK_CUR) != 0) goto fail;

    if (read_cars(f, st) != 0) goto fail;

    if (read_sprite_info(f, st) != 0) goto fail;

    st->sprite_graphics_len = st->hdr.sprite_graphics_size;
    if (st->sprite_graphics_len) {
        st->sprite_graphics = (unsigned char *)malloc(st->sprite_graphics_len);
        if (!st->sprite_graphics) {
            fprintf(stderr, "gta_style: out of memory for sprite graphics\n");
            goto fail;
        }
        if (fread(st->sprite_graphics, 1, st->sprite_graphics_len, f)
                != st->sprite_graphics_len) {
            fprintf(stderr, "gta_style: short read on sprite graphics\n");
            goto fail;
        }
    }

    if (read_sprite_numbers(f, st) != 0) goto fail;

    /* The counts must account for every sprite the info section described. If
     * they do not, either the walk or the category order is wrong, and every
     * "sprite 4 of the ped set" lookup built on it would silently fetch a bus.
     * Warn rather than fail: the map still renders without categories. */
    {
        int i, sum = 0;
        for (i = 0; i < GTA_SPR_TYPE_COUNT; i++)
            sum += st->sprite_numbers[i];
        if (sum != st->sprite_count)
            fprintf(stderr, "gta_style: sprite_numbers sum to %d but the info "
                            "section holds %d sprites\n", sum,
                    st->sprite_count);
    }

    /* Resolve every car's sprite. This has to wait until here: sprite_num
     * counts within the vehicle class's own CATEGORY, and where a category
     * starts is only known once sprite_numbers has been read. A car whose
     * class is not one of the nine known vtypes keeps -1 rather than pointing
     * at sprite 0, so a bad class draws nothing instead of drawing an arrow. */
    {
        int i;
        for (i = 0; i < st->car_count; i++) {
            gta_sprite_type ty = gta_vehicle_sprite_type(st->cars[i].vtype);
            if (ty == GTA_SPR_TYPE_COUNT) continue;
            if (st->cars[i].sprite_num < 0 ||
                st->cars[i].sprite_num >= gta_style_sprite_count(st, ty))
                continue;
            st->cars[i].sprite_index = gta_style_sprite_base(st, ty)
                                     + st->cars[i].sprite_num;
        }
    }

    fclose(f);
    return 0;

fail:
    if (f) fclose(f);
    gta_style_free(st);
    return -1;
}

void gta_style_free(gta_style *st)
{
    free(st->remaps);
    st->remaps = NULL;
    st->remap_count = 0;
    if (!st) return;
    free(st->blocks);
    free(st->sprite_graphics);
    free(st->sprites);
    free(st->cars);
    st->blocks = NULL;
    st->sprite_graphics = NULL;
    st->sprites = NULL;
    st->cars = NULL;
    st->blocks_len = 0;
    st->sprite_graphics_len = 0;
    st->sprite_count = 0;
    st->car_count = 0;
}

int gta_style_get_sprite(const gta_style *st, int index,
                         unsigned char *dst, int dst_stride)
{
    const struct gta_sprite *sp;
    const unsigned char *src;
    unsigned long base;
    int x, y;

    if (!st->sprite_graphics || index < 0 || index >= st->sprite_count)
        return -1;
    sp = &st->sprites[index];
    if (sp->w == 0 || sp->h == 0)
        return -1;

    base = (unsigned long)sp->page * GTA_PAGE_SIZE
         + (unsigned long)sp->page_y * GTA_PAGE_DIM
         + sp->page_x;
    if (base + (unsigned long)(sp->h - 1) * GTA_PAGE_DIM + sp->w
            > st->sprite_graphics_len)
        return -1;

    src = st->sprite_graphics + base;
    for (y = 0; y < sp->h; y++) {
        for (x = 0; x < sp->w; x++) {
            unsigned char px = src[(long)y * GTA_PAGE_DIM + x];
            if (px)                                 /* 0 is transparent */
                dst[(long)y * dst_stride + x] = px;
        }
    }
    return 0;
}

int gta_style_block_count(const gta_style *st, gta_block_type type)
{
    switch (type) {
    case GTA_BLOCK_SIDE: return st->side_blocks;
    case GTA_BLOCK_LID:  return st->lid_blocks;
    case GTA_BLOCK_AUX:  return st->aux_blocks;
    default:             return 0;
    }
}

/* side, lid and aux are stored back to back, so a type+index pair becomes one
 * linear block number. */
static int linear_index(const gta_style *st, gta_block_type type, int index)
{
    if (index < 0 || index >= gta_style_block_count(st, type))
        return -1;
    switch (type) {
    case GTA_BLOCK_SIDE: return index;
    case GTA_BLOCK_LID:  return st->side_blocks + index;
    case GTA_BLOCK_AUX:  return st->side_blocks + st->lid_blocks + index;
    default:             return -1;
    }
}

int gta_style_get_block(const gta_style *st, gta_block_type type, int index,
                        unsigned char *dst, int dst_stride)
{
    int linear, page, in_page, block_row, block_col, y;
    const unsigned char *src;

    if (!st->blocks) return -1;
    linear = linear_index(st, type, index);
    if (linear < 0) return -1;

    page      = linear / GTA_BLOCKS_PER_PAGE;
    in_page   = linear % GTA_BLOCKS_PER_PAGE;
    block_row = in_page / 4;
    block_col = in_page % 4;

    /* Within a page, a scanline spans all four blocks of a row, so stepping
     * down one pixel row costs GTA_PAGE_DIM bytes, not GTA_BLOCK_DIM. */
    src = st->blocks
        + (unsigned long)page * GTA_PAGE_SIZE
        + (unsigned long)block_row * GTA_BLOCK_DIM * GTA_PAGE_DIM
        + (unsigned long)block_col * GTA_BLOCK_DIM;

    if ((unsigned long)(src - st->blocks) + (GTA_BLOCK_DIM - 1) * GTA_PAGE_DIM
            + GTA_BLOCK_DIM > st->blocks_len)
        return -1;

    for (y = 0; y < GTA_BLOCK_DIM; y++) {
        memcpy(dst + (long)y * dst_stride, src + (long)y * GTA_PAGE_DIM,
               GTA_BLOCK_DIM);
    }
    return 0;
}

void gta_style_describe(const gta_style *st, FILE *out)
{
    fprintf(out, "version            %lu\n", st->hdr.version);
    fprintf(out, "side  %8lu bytes = %d blocks\n", st->hdr.side_size, st->side_blocks);
    fprintf(out, "lid   %8lu bytes = %d blocks\n", st->hdr.lid_size, st->lid_blocks);
    fprintf(out, "aux   %8lu bytes = %d blocks\n", st->hdr.aux_size, st->aux_blocks);
    fprintf(out, "total blocks       %d (padded to %lu bytes)\n",
            st->total_blocks, st->blocks_len);
    fprintf(out, "anim               %lu\n", st->hdr.anim_size);
    fprintf(out, "palette            %lu (%s)\n", st->hdr.palette_size,
            st->palette_was_6bit ? "6-bit VGA, scaled to 8" : "already 8-bit");
    fprintf(out, "unknown_a          %lu\n", st->hdr.unknown_a);
    fprintf(out, "unknown_b          %lu\n", st->hdr.unknown_b);
    fprintf(out, "object_info        %lu\n", st->hdr.object_info_size);
    fprintf(out, "car                %lu\n", st->hdr.car_size);
    fprintf(out, "sprite_info        %lu\n", st->hdr.sprite_info_size);
    fprintf(out, "sprite_graphics    %lu\n", st->hdr.sprite_graphics_size);
    fprintf(out, "sprite_numbers     %lu\n", st->hdr.sprite_numbers_size);
    fprintf(out, "cars               %d records in %lu bytes\n",
            st->car_count, st->hdr.car_size);
}
