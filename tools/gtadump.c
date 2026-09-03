/* Host-side viewer for GTA data, built on the same readers the Amiga port uses.
 *
 * Writes 8-bit BMPs.  BMP rather than PNG on purpose: no library dependency,
 * trivial to write, and the palette survives - which is the whole point, since
 * the port renders into an 8-bit chunky buffer and a truecolour dump would hide
 * palette mistakes instead of showing them.
 *
 *   gtadump style <style.gry> <out-prefix>
 *
 * Produces <prefix>_side.bmp, <prefix>_lid.bmp, <prefix>_aux.bmp - contact
 * sheets of every block of each type - and prints the header.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native/gta_style.h"
#include "../native/gta_map.h"
#include "../native/gta_tiles.h"
#include "../native/gta_render.h"
#include "../native/gta_player.h"
#include "../native/gta_traffic.h"
#include "../native/gta_trig.h"
#include "../native/gta_nav.h"
#include "../native/gta_vehphys.h"
#include "../native/gta_peds.h"

static void put_u32le(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

static void put_u16le(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

/* 8-bit BMP: 14-byte file header, 40-byte info header, 256 BGRA palette
 * entries, then bottom-up rows padded to a multiple of 4 bytes. */
static int write_bmp8(const char *path, const unsigned char *pixels,
                      int w, int h, const unsigned char *palette_rgb)
{
    unsigned char fh[14], ih[40], pal[256 * 4];
    unsigned long row_stride = ((unsigned long)w + 3UL) & ~3UL;
    unsigned long pixel_bytes = row_stride * (unsigned long)h;
    unsigned long offbits = 14 + 40 + sizeof pal;
    unsigned char *padrow;
    FILE *f;
    int i, y;

    f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        return -1;
    }

    memset(fh, 0, sizeof fh);
    fh[0] = 'B'; fh[1] = 'M';
    put_u32le(fh + 2, offbits + pixel_bytes);
    put_u32le(fh + 10, offbits);

    memset(ih, 0, sizeof ih);
    put_u32le(ih + 0, 40);
    put_u32le(ih + 4, (unsigned long)w);
    put_u32le(ih + 8, (unsigned long)h);
    put_u16le(ih + 12, 1);          /* planes */
    put_u16le(ih + 14, 8);          /* bits per pixel */
    put_u32le(ih + 20, pixel_bytes);
    put_u32le(ih + 32, 256);        /* colours used */

    for (i = 0; i < 256; i++) {
        pal[i * 4 + 0] = palette_rgb[i * 3 + 2];   /* B */
        pal[i * 4 + 1] = palette_rgb[i * 3 + 1];   /* G */
        pal[i * 4 + 2] = palette_rgb[i * 3 + 0];   /* R */
        pal[i * 4 + 3] = 0;
    }

    fwrite(fh, 1, sizeof fh, f);
    fwrite(ih, 1, sizeof ih, f);
    fwrite(pal, 1, sizeof pal, f);

    padrow = (unsigned char *)calloc(1, row_stride);
    if (!padrow) { fclose(f); return -1; }
    for (y = h - 1; y >= 0; y--) {          /* BMP rows run bottom-up */
        memcpy(padrow, pixels + (long)y * w, (size_t)w);
        fwrite(padrow, 1, row_stride, f);
    }
    free(padrow);
    fclose(f);
    return 0;
}

/* ---- 2:1 downscaling of palettised tiles -------------------------------
 *
 * The port renders 32x32 tiles (see PROGRESS.md: 64x64 gives 5x3 tiles on a
 * 320x200 screen, which is unplayable). Downscaling happens once, on the host,
 * so cost does not matter here - correctness does.
 *
 * You cannot average palette *indices*; index 37 is not "between" 36 and 38.
 * So there are two honest options, and which one is right is a question about
 * the artwork rather than about image processing:
 *
 *   SCALE_NEAREST  keep one of the four source pixels. Colours stay exactly as
 *                  the artist chose them, but a one-pixel feature survives only
 *                  if it happens to land on the sampled corner. GTA's road
 *                  markings are thin bright lines on dark tarmac, so this is
 *                  the filter that might lose them.
 *
 *   SCALE_AVERAGE  average the four pixels in RGB, then snap to the nearest
 *                  palette entry. Thin features survive as a dimmer version of
 *                  themselves, but colour can drift wherever the palette has no
 *                  good match for the average.
 *
 * Both are implemented so the choice can be made by looking at the output.
 */
enum { SCALE_NEAREST = 0, SCALE_AVERAGE = 1 };

/* Nearest palette entry to an RGB triple, by squared distance. Linear over 256
 * entries: this is a build step, not an inner loop. */
static unsigned char nearest_index(const unsigned char *pal, int r, int g, int b)
{
    int best = 0;
    long best_d = -1;
    int i;
    for (i = 0; i < 256; i++) {
        int dr = r - pal[i * 3 + 0];
        int dg = g - pal[i * 3 + 1];
        int db = b - pal[i * 3 + 2];
        long d = (long)dr * dr + (long)dg * dg + (long)db * db;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = i;
            if (d == 0) break;
        }
    }
    return (unsigned char)best;
}

/* src is GTA_BLOCK_DIM square with stride src_stride; dst is half that. */
static void downscale_block(const unsigned char *src, int src_stride,
                            unsigned char *dst, int dst_stride,
                            const unsigned char *pal, int mode)
{
    int y, x;
    for (y = 0; y < GTA_BLOCK_DIM / 2; y++) {
        for (x = 0; x < GTA_BLOCK_DIM / 2; x++) {
            const unsigned char *s = src + (long)(y * 2) * src_stride + x * 2;
            if (mode == SCALE_NEAREST) {
                dst[(long)y * dst_stride + x] = s[0];
            } else {
                int i, r = 0, g = 0, b = 0;
                unsigned char p[4];
                p[0] = s[0];
                p[1] = s[1];
                p[2] = s[src_stride];
                p[3] = s[src_stride + 1];
                for (i = 0; i < 4; i++) {
                    r += pal[p[i] * 3 + 0];
                    g += pal[p[i] * 3 + 1];
                    b += pal[p[i] * 3 + 2];
                }
                dst[(long)y * dst_stride + x] =
                    nearest_index(pal, r / 4, g / 4, b / 4);
            }
        }
    }
}

/* Lay every block of one type out as a contact sheet, 16 blocks per row. */
static int dump_block_sheet(const gta_style *st, gta_block_type type,
                            const char *name, const char *prefix)
{
    int count = gta_style_block_count(st, type);
    int cols = 16;
    int rows, w, h, i;
    unsigned char *sheet;
    char path[512];

    if (count <= 0) {
        printf("  %-5s no blocks\n", name);
        return 0;
    }
    rows = (count + cols - 1) / cols;
    w = cols * GTA_BLOCK_DIM;
    h = rows * GTA_BLOCK_DIM;

    sheet = (unsigned char *)calloc(1, (size_t)w * (size_t)h);
    if (!sheet) {
        fprintf(stderr, "out of memory for a %dx%d sheet\n", w, h);
        return -1;
    }

    for (i = 0; i < count; i++) {
        int bx = (i % cols) * GTA_BLOCK_DIM;
        int by = (i / cols) * GTA_BLOCK_DIM;
        if (gta_style_get_block(st, type, i, sheet + (long)by * w + bx, w) != 0) {
            fprintf(stderr, "failed to read %s block %d\n", name, i);
            free(sheet);
            return -1;
        }
    }

    sprintf(path, "%s_%s.bmp", prefix, name);
    if (write_bmp8(path, sheet, w, h, st->palette) != 0) {
        free(sheet);
        return -1;
    }
    printf("  %-5s %4d blocks -> %s (%dx%d)\n", name, count, path, w, h);
    free(sheet);
    return 0;
}

static int cmd_style(const char *stylePath, const char *prefix)
{
    gta_style st;
    int rc = 0;

    if (gta_style_load(stylePath, &st) != 0)
        return 1;

    printf("=== %s ===\n", stylePath);
    gta_style_describe(&st, stdout);
    printf("--- writing sheets ---\n");

    if (dump_block_sheet(&st, GTA_BLOCK_SIDE, "side", prefix) != 0) rc = 1;
    if (dump_block_sheet(&st, GTA_BLOCK_LID,  "lid",  prefix) != 0) rc = 1;
    if (dump_block_sheet(&st, GTA_BLOCK_AUX,  "aux",  prefix) != 0) rc = 1;

    gta_style_free(&st);
    return rc;
}

/* Render a rectangle of the city as it looks from directly above: for every
 * column, paint each layer's lid in turn from the ground up, so upper floors
 * and roofs overwrite the road beneath them.
 *
 * Lid rotation and the two face-flip flags are deliberately ignored here. They
 * matter for a faithful renderer and are decoded by the gta_block_* macros, but
 * this tool exists to prove the readers, and a wrong tile is far easier to spot
 * than a wrongly-rotated one. */
static int cmd_map(const char *mapPath, const char *stylePath,
                   int x0, int y0, int tiles_w, int tiles_h, const char *out)
{
    gta_map mp;
    gta_style st;
    unsigned char *canvas;
    int w, h, tx, ty, z;
    int drawn = 0, missing = 0;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_style_load(stylePath, &st) != 0) {
        gta_map_free(&mp);
        return 1;
    }

    printf("=== %s ===\n", mapPath);
    gta_map_describe(&mp, stdout);

    w = tiles_w * GTA_BLOCK_DIM;
    h = tiles_h * GTA_BLOCK_DIM;
    canvas = (unsigned char *)calloc(1, (size_t)w * (size_t)h);
    if (!canvas) {
        fprintf(stderr, "out of memory for a %dx%d canvas\n", w, h);
        gta_map_free(&mp);
        gta_style_free(&st);
        return 1;
    }

    for (ty = 0; ty < tiles_h; ty++) {
        for (tx = 0; tx < tiles_w; tx++) {
            int mx = x0 + tx, my = y0 + ty;
            int height = gta_map_column_height(&mp, mx, my);
            unsigned char *dst = canvas
                + (long)(ty * GTA_BLOCK_DIM) * w + tx * GTA_BLOCK_DIM;
            for (z = 0; z < height; z++) {
                gta_block b;
                if (!gta_map_block(&mp, mx, my, z, &b))
                    continue;
                if (b.faces[GTA_FACE_LID] == 0)
                    continue;
                if (gta_style_get_block(&st, GTA_BLOCK_LID,
                                        b.faces[GTA_FACE_LID], dst, w) == 0)
                    drawn++;
                else
                    missing++;
            }
        }
    }

    printf("--- rendered %d lids (%d out of range) from (%d,%d) %dx%d tiles ---\n",
           drawn, missing, x0, y0, tiles_w, tiles_h);

    if (write_bmp8(out, canvas, w, h, st.palette) == 0)
        printf("  wrote %s (%dx%d)\n", out, w, h);

    free(canvas);
    gta_map_free(&mp);
    gta_style_free(&st);
    return 0;
}

/* What the sprite_numbers section says: which of the 1009 sprites is a
 * pedestrian, and how big it is. There is nothing else in the file that says
 * so, and everything drawn on top of the city depends on it. */
/* The car table, printed so it can be READ rather than trusted. The point of
 * this command is that the numbers have to look like a car: a saloon about 50
 * pixels long, a bus twice that, a mass of a few tonnes, four doors on a taxi.
 * A record layout that is wrong by one field parses without complaint and
 * produces a table that is obviously nonsense the moment a human sees it -
 * which is exactly why it gets printed. */
/* THE OBJECT TABLE - which sprite is the bullet, the splat, the crate. The
 * proof behind gta_tiles.h version 7 and the weapons' sprite numbers. */
static int cmd_objinfo(const char *stylePath)
{
    gta_style st;
    int i;

    if (gta_style_load(stylePath, &st) != 0)
        return 1;

    printf("=== %s ===\n", stylePath);
    printf("object_info: %lu bytes, %d entries; object sprites %d from %d\n",
           st.hdr.object_info_size, st.object_count,
           gta_style_sprite_count(&st, GTA_SPR_OBJECT),
           gta_style_sprite_base(&st, GTA_SPR_OBJECT));
    printf("%4s %6s %6s %6s %6s %6s %5s %6s %4s  %s\n",
           "type", "w", "h", "depth", "sprnum", "sprite", "wxh", "weight",
           "aux", "status/into");
    for (i = 0; i < st.object_count; i++) {
        const struct gta_object_info *o = &st.objects[i];
        int sw = 0, sh = 0;
        if (o->sprite_index >= 0 && o->sprite_index < st.sprite_count) {
            sw = st.sprites[o->sprite_index].w;
            sh = st.sprites[o->sprite_index].h;
        }
        printf("0x%02x %6ld %6ld %6ld %6d %6d %2dx%-2d %6d %4d  %d/%d\n",
               i, o->w, o->h, o->depth, o->sprite_num, o->sprite_index,
               sw, sh, o->weight, o->aux, o->status, o->num_into);
    }
    gta_style_free(&st);
    return 0;
}

static int cmd_carinfo(const char *stylePath, int verbose)
{
    gta_style st;
    int i;
    int by_class[16];

    if (gta_style_load(stylePath, &st) != 0)
        return 1;

    for (i = 0; i < 16; i++) by_class[i] = 0;

    printf("=== %s ===\n", stylePath);
    printf("car section: %lu bytes, %d records\n",
           st.hdr.car_size, st.car_count);
    printf("%3s %-11s %-6s %7s %6s %6s %5s %5s %4s %5s %s\n",
           "#", "class", "wxl", "sprite", "mass", "thrust", "maxsp",
           "accel", "door", "model", "remap0..3");

    for (i = 0; i < st.car_count; i++) {
        const gta_car_info *c = &st.cars[i];
        int sw = 0, sh = 0;

        if (c->vtype < 16) by_class[c->vtype]++;
        if (c->sprite_index >= 0 && c->sprite_index < st.sprite_count) {
            sw = st.sprites[c->sprite_index].w;
            sh = st.sprites[c->sprite_index].h;
        }

        /* Masses are 16.16; printed as a whole number and two decimals with
         * integer arithmetic, because this file must not grow a float. */
        printf("%3d %-11s %3dx%-3d %3d%c%2dx%-2d %3ld.%02ld %3ld.%02ld "
               "%5d %5d %4d %5d  %02x %02x %02x %02x\n",
               i, gta_vehicle_class_name(c->vtype),
               c->width, c->length,
               c->sprite_num, c->sprite_index >= 0 ? ':' : '?', sw, sh,
               c->mass >> 16, ((c->mass & 0xFFFFL) * 100L) >> 16,
               c->thrust >> 16, ((c->thrust & 0xFFFFL) * 100L) >> 16,
               c->max_speed, c->accel, c->n_doors, c->model_id,
               c->remap8[0], c->remap8[1], c->remap8[2], c->remap8[3]);

        if (verbose) {
            int d;
            printf("      vert=%d weight=%d minsp=%d brake=%d grip=%d hand=%d "
                   "turn=%d dmg=%d cx,cy=%d,%d moment=%ld\n",
                   c->vert, c->weight, c->min_speed, c->braking, c->grip,
                   c->handling, c->turning, c->damagable, c->cx, c->cy,
                   c->moment);
            printf("      adhesion %ld.%02ld/%ld.%02ld  brakes %ld.%02ld/"
                   "%ld.%02ld bias %ld.%02ld  slide %ld.%02ld/%ld.%02ld\n",
                   c->tyre_adhesion_x >> 16,
                   ((c->tyre_adhesion_x & 0xFFFFL) * 100L) >> 16,
                   c->tyre_adhesion_y >> 16,
                   ((c->tyre_adhesion_y & 0xFFFFL) * 100L) >> 16,
                   c->handbrake_friction >> 16,
                   ((c->handbrake_friction & 0xFFFFL) * 100L) >> 16,
                   c->footbrake_friction >> 16,
                   ((c->footbrake_friction & 0xFFFFL) * 100L) >> 16,
                   c->front_brake_bias >> 16,
                   ((c->front_brake_bias & 0xFFFFL) * 100L) >> 16,
                   c->back_end_slide >> 16,
                   ((c->back_end_slide & 0xFFFFL) * 100L) >> 16,
                   c->handbrake_slide >> 16,
                   ((c->handbrake_slide & 0xFFFFL) * 100L) >> 16);
            printf("      turn_ratio=%d drive_off=%d steer_off=%d conv=%d "
                   "engine=%d radio=%d horn=%d sfx=%d fast=%d\n",
                   c->turn_ratio, c->drive_wheel_offset,
                   c->steering_wheel_offset, c->convertible, c->engine,
                   c->radio, c->horn, c->sound_function, c->fast_change);
            for (d = 0; d < c->n_doors; d++)
                printf("      door %d: rp=(%d,%d) object=%d delta=%d\n", d,
                       c->doors[d].rpx, c->doors[d].rpy,
                       c->doors[d].object, c->doors[d].delta);
        }
    }

    printf("\nby class:");
    for (i = 0; i < 16; i++)
        if (by_class[i])
            printf("  %s %d", gta_vehicle_class_name(i), by_class[i]);
    printf("\n");

    gta_style_free(&st);
    return 0;
}

/* THE ROAD DIRECTIONS AS A MAP, over a rectangle of city.
 *
 * One character per block, so the shape of the road network is visible at a
 * glance rather than one `gtadump column` at a time. It exists to answer a
 * question that decides how traffic is laid out: **is a block a LANE?** If
 * neighbouring blocks carry opposite directions, then each block is one lane
 * and a car belongs in the middle of its own block; if a whole road's worth of
 * blocks carries the same direction, lanes are something else.
 *
 *   |  north only      -  east only       # a building or no road
 *   v  south only      <  west only       + two or more directions
 *   .  road, no direction bits at all
 */
/* WHERE IN THE CITY IS THIS SIDE TILE USED?
 *
 * Written for the mirrored-sign question: the contact sheet gives the index of
 * a tile with lettering on it, and to see whether the renderer flips it the
 * wrong way round you first have to find a wall that actually carries it.
 * Prints the first few blocks and which face they use it on. */
static int cmd_findtile(const char *mapPath, int want, int limit)
{
    gta_map mp;
    int x, y, z, found = 0;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;

    printf("blocks using side tile %d:\n", want);
    for (z = 0; z < GTA_MAP_LAYERS && found < limit; z++)
    for (y = 0; y < GTA_MAP_DIM && found < limit; y++)
    for (x = 0; x < GTA_MAP_DIM && found < limit; x++) {
        gta_block b;
        if (!gta_map_block(&mp, x, y, z, &b)) continue;
        if (b.faces[GTA_FACE_W] == want) { printf("  (%3d,%3d) L%d  WEST  face, flip_lr %d, flat %d\n",
                                      x, y, z, gta_block_flip_lr(&b) ? 1 : 0,
                                      gta_block_is_flat(&b) ? 1 : 0); found++; }
        if (b.faces[GTA_FACE_E] == want) { printf("  (%3d,%3d) L%d  EAST  face, flip_lr %d, flat %d\n",
                                      x, y, z, gta_block_flip_lr(&b) ? 1 : 0,
                                      gta_block_is_flat(&b) ? 1 : 0); found++; }
        if (b.faces[GTA_FACE_N] == want) { printf("  (%3d,%3d) L%d  NORTH face, flip_tb %d, flat %d\n",
                                      x, y, z, gta_block_flip_tb(&b) ? 1 : 0,
                                      gta_block_is_flat(&b) ? 1 : 0); found++; }
        if (b.faces[GTA_FACE_S] == want) { printf("  (%3d,%3d) L%d  SOUTH face, flip_tb %d, flat %d\n",
                                      x, y, z, gta_block_flip_tb(&b) ? 1 : 0,
                                      gta_block_is_flat(&b) ? 1 : 0); found++; }
    }
    if (!found) printf("  none\n");
    gta_map_free(&mp);
    return 0;
}

static int cmd_dirmap(const char *mapPath, int bx, int by, int w, int h, int z)
{
    gta_map mp;
    int x, y;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;

    printf("directions at layer %d, (%d,%d) to (%d,%d)\n",
           z, bx, by, bx + w - 1, by + h - 1);
    printf("     ");
    for (x = 0; x < w; x++) printf("%d", (bx + x) % 10);
    printf("\n");

    for (y = 0; y < h; y++) {
        printf("%4d ", by + y);
        for (x = 0; x < w; x++) {
            gta_block b;
            int d, g;
            char c;
            if (!gta_map_block(&mp, bx + x, by + y, z, &b)) { printf("#"); continue; }
            g = gta_block_ground_type(&b);
            d = gta_block_dirs(&b);
            if (g != 2)            c = (g == 3) ? ',' : '#';
            else if (d == 0)       c = '.';
            else if (d == 0x01)    c = '|';
            else if (d == 0x02)    c = 'v';
            else if (d == 0x08)    c = '-';
            else if (d == 0x04)    c = '<';
            else                   c = '+';
            printf("%c", c);
        }
        printf("\n");
    }
    printf("  | N only   v S only   - E only   < W only   + several   "
           ". road no dirs   , pavement   # other\n");
    gta_map_free(&mp);
    return 0;
}

/* THE BOX AS THE RESERVATION REALLY SEES IT. dirmap prints the MAP's
 * direction bits, but the traffic runs on the NAV grid (is_junction reads
 * gta_nav_dirs once gta_traffic_set_nav has run - both the game and the
 * drive test set it), and dirmap's `+` also hides the difference between a
 * both-axes seed and a two-way single-axis block. Diagnosing the box from
 * dirmap is therefore diagnosing the wrong data - this prints the verdicts
 * of the real function, on the real grid. 'S' = both-axes seed, 'B' = in
 * the box by extension, '.' = road outside every box. */
static int cmd_boxmap(const char *mapPath, int bx, int by, int w, int h, int z)
{
    gta_map mp;
    static gta_nav nav;
    static gta_traffic tr;
    int x, y;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_nav_build(&nav, &mp) != 0) {
        printf("nav build failed\n");
        return 1;
    }
    gta_traffic_set_nav(&tr, &nav);

    printf("box map at layer %d, (%d,%d) to (%d,%d)\n",
           z, bx, by, bx + w - 1, by + h - 1);
    printf("     ");
    for (x = 0; x < w; x++) printf("%d", (bx + x) % 10);
    printf("\n");
    for (y = 0; y < h; y++) {
        printf("%4d ", by + y);
        for (x = 0; x < w; x++) {
            unsigned char v = gta_nav_at(&nav, bx + x, by + y, z);
            int d = gta_nav_dirs(v);
            int g = gta_nav_ground(v);
            char c;
            if (g != 2)
                c = (g == 3) ? ',' : '#';
            else if (((d & 0x03) != 0) && ((d & 0x0C) != 0))
                c = 'S';
            else if (gta_traffic_is_junction(&mp, bx + x, by + y, z))
                c = 'B';
            else
                c = '.';
            printf("%c", c);
        }
        printf("\n");
    }
    printf("  S seed (both axes in NAV)   B in box   . road out of box   "
           ", pavement   # other\n");
    gta_map_free(&mp);
    return 0;
}

/* WHERE THE TRAFFIC LIGHTS ARE, read off the map rather than assumed.
 *
 * `type_map_ext` bits 0..2 are the traffic hint and 1 means a traffic light
 * (Carnage3D's eTrafficHint, MIT - it reads the field and does nothing with
 * it). Nothing in this port obeyed them, and before anything can, the layout
 * has to be looked at: whether the hint sits on the junction box itself or on
 * the approach block decides where a car has to stop.
 *
 * Prints a grid one character per block plus a whole-map census. */
static int cmd_lights(const char *mapPath, int bx, int by, int w, int h, int z)
{
    gta_map mp;
    int x, y, i;
    long census[8];

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;

    for (i = 0; i < 8; i++) census[i] = 0;
    for (y = 0; y < GTA_MAP_DIM; y++)
        for (x = 0; x < GTA_MAP_DIM; x++) {
            int zz;
            for (zz = 0; zz < GTA_MAP_LAYERS; zz++) {
                gta_block b;
                int hint;
                if (!gta_map_block(&mp, x, y, zz, &b)) continue;
                hint = gta_block_traffic_hint(&b) & 7;
                census[hint]++;
                /* The first few light blocks with their surroundings, because
                 * a census does not say whether the hint sits on the junction
                 * box, on the approach, or on the kerb beside it - and that is
                 * the only thing a car needs to know. */
                if (hint == 1 && census[1] <= 12)
                    printf("  light at (%d,%d) layer %d: ground %d dirs $%02x\n",
                           x, y, zz, gta_block_ground_type(&b),
                           gta_block_dirs(&b));
            }
        }

    printf("traffic hints over the whole map, all layers:\n");
    for (i = 0; i < 8; i++)
        if (census[i])
            printf("  hint %d: %ld blocks%s\n", i, census[i],
                   (i == 1) ? "   <- traffic light" : "");

    printf("hints at layer %d, (%d,%d) to (%d,%d)\n",
           z, bx, by, bx + w - 1, by + h - 1);
    printf("     ");
    for (x = 0; x < w; x++) printf("%d", (bx + x) % 10);
    printf("\n");

    for (y = 0; y < h; y++) {
        printf("%4d ", by + y);
        for (x = 0; x < w; x++) {
            gta_block b;
            int hint, g, d;
            char c;
            if (!gta_map_block(&mp, bx + x, by + y, z, &b)) { printf("#"); continue; }
            hint = gta_block_traffic_hint(&b);
            g    = gta_block_ground_type(&b);
            d    = gta_block_dirs(&b);
            if (hint)            c = (char)('0' + hint);
            else if (g != 2)     c = (g == 3) ? ',' : '#';
            else if (d == 0)     c = '.';
            else if ((d & 0x03) && (d & 0x0C)) c = '+';   /* junction box */
            else                 c = '-';
            printf("%c", c);
        }
        printf("\n");
    }
    printf("  digits are the hint (1 = traffic light)   + junction   "
           "- one-axis road   . road no dirs   , pavement   # other\n");
    gta_map_free(&mp);
    return 0;
}

/* RAMPS, and which way is UP.
 *
 * `type_map` bits 8..13 are the slope: 0 flat, 1..8 a 26-degree ramp in four
 * directions, 9..40 a 7-degree one in eight steps, 41..44 a 45-degree one.
 * Carnage3D labels the groups N/S/W/E but that label is about which CORNERS of
 * its mesh are raised, and this port needs something else entirely: which way
 * a man walking up it ends up a layer higher.
 *
 * So it is read off the map instead of inferred. For every ramp this prints
 * the layer of drivable ground on each of the four sides, and the direction
 * whose neighbour sits a layer HIGHER is the way up. If that agrees with the
 * letter Carnage3D uses, good; if it does not, the map wins.
 */
static int cmd_slopes(const char *mapPath, int want)
{
    gta_map mp;
    int bx, by, z, found = 0;
    static const char *const grp[5] = { "flat", "26deg", "7deg", "45deg", "?" };

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;

    printf("%-11s %-5s %-6s %-7s  %s\n",
           "block", "layer", "slope", "kind", "walkable ground layer N/S/W/E");

    for (by = 0; by < GTA_MAP_DIM && found < want; by++)
    for (bx = 0; bx < GTA_MAP_DIM && found < want; bx++)
    for (z = 0; z < GTA_MAP_LAYERS && found < want; z++) {
        gta_block b;
        int sl, kind, i;
        int nb[4];
        static const int dx[4] = { 0, 0, -1, 1 };
        static const int dy[4] = { -1, 1, 0, 0 };

        int g0;
        if (!gta_map_block(&mp, bx, by, z, &b)) continue;
        sl = gta_block_slope(&b);
        if (sl == 0) continue;
        /* Only ramps you can be ON. The first run of this listed 45-degree
         * slopes at layers 3, 4 and 5 with flat ground two layers below on
         * every side - those are the sloping WALLS and roofs of buildings, not
         * anything a person or a car uses, and they drowned the handful of
         * real ramps. A ramp that matters has a walkable ground type of its
         * own. */
        g0 = gta_block_ground_type(&b);
        if (g0 < 2 || g0 > 4) continue;

        kind = (sl <= 8) ? 1 : (sl <= 40) ? 2 : (sl <= 44) ? 3 : 4;

        /* For each side, the lowest layer at or above z-1 whose ground type is
         * something a person can stand on. That is the height of the ground
         * next door, which is what says where the ramp leads. */
        for (i = 0; i < 4; i++) {
            int qx = bx + dx[i], qy = by + dy[i], qz;
            nb[i] = -1;
            for (qz = 0; qz < GTA_MAP_LAYERS; qz++) {
                gta_block q;
                int g;
                if (qx < 0 || qx >= GTA_MAP_DIM || qy < 0 || qy >= GTA_MAP_DIM)
                    break;
                if (!gta_map_block(&mp, qx, qy, qz, &q)) continue;
                g = gta_block_ground_type(&q);
                if (g >= 2 && g <= 4) { nb[i] = qz; break; }
            }
        }

        printf("(%3d,%3d)   %d     %2d     %-7s  %2d %2d %2d %2d\n",
               bx, by, z, sl, grp[kind], nb[0], nb[1], nb[2], nb[3]);
        found++;
    }
    printf("%d shown. A neighbour one layer HIGHER than this block is where "
           "the ramp leads.\n", found);
    gta_map_free(&mp);
    return 0;
}

/* Columns where something is drawn OVER a place you can stand.
 *
 * The renderer draws a block's lid at grid z+1 - the top of its own cube - so
 * a lid belonging to layer z is above the head of anything standing on layer
 * z. Those are the columns where the draw order between the city and a sprite
 * actually matters, and they are what a canopy, a walkway or a pipe over a
 * road looks like in the data.
 *
 * This exists because "he is walking on top of that pipe" is a report about a
 * picture, and fixing it needs a list of places to point a camera at. */
static int cmd_overhead(const char *mapPath, int want)
{
    gta_map mp;
    int bx, by, found = 0;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;

    printf("columns where a walkable layer also carries a lid\n");
    printf("%-11s %-5s %-9s %s\n", "block", "layer", "ground", "lid at that layer");

    for (by = 0; by < GTA_MAP_DIM && found < want; by++) {
        for (bx = 0; bx < GTA_MAP_DIM && found < want; bx++) {
            int z;
            for (z = 0; z < GTA_MAP_LAYERS; z++) {
                gta_block b;
                int g;
                if (!gta_map_block(&mp, bx, by, z, &b))
                    continue;
                g = gta_block_ground_type(&b);
                if (g < 2 || g > 4)             /* road, pavement, field */
                    continue;
                if (!b.faces[GTA_FACE_LID])
                    continue;
                printf("(%3d,%3d)   %d     %-9s lid %d%s\n", bx, by, z,
                       g == 2 ? "road" : (g == 3 ? "pavement" : "field"),
                       b.faces[GTA_FACE_LID],
                       gta_block_is_flat(&b) ? "  (flat)" : "");
                found++;
                break;
            }
        }
    }
    printf("%d shown\n", found);
    gta_map_free(&mp);
    return 0;
}

/* Every placed car re-checked against the map: each block its own rectangle
 * covers must be road, or pavement across its width (a kerb overhang is what
 * parking at a kerb looks like). Returns how many are wrong and prints them.
 *
 * Shared by the placement test and the driving test, because "is it still on
 * the road" is the same question after a thousand ticks as it is after zero -
 * and the driving test is the one that can answer it about a car that has
 * turned four corners. */
/* ONE CAR, EVERY TICK, IN FULL. `GTA_TRACE_CAR=<serial> gtadump drive ...`
 *
 * The histograms and the entry dump between them said "a car driving straight
 * on a street ends up on the footway and stays there", and the route audit
 * said the route is not the problem. What is left is a sequence of decisions,
 * and a sequence needs a trace: the tick a car goes wrong is almost never the
 * tick the symptom appears.
 *
 * Follows a SERIAL, so it survives the fleet being compacted. Prints the
 * ground under the car's block so the moment it leaves the road is visible in
 * the same column as the decision that took it there. */
static void trace_car(const gta_map *mp, const gta_traffic *tr,
                      int t, unsigned long want)
{
    int i;
    if (!want) return;
    for (i = 0; i < tr->n; i++) {
        const gta_car *c = &tr->cars[i];
        int bx, by, g = 0;
        gta_block b;
        int nx = -1, ny = -1, n2x = -1, n2y = -1;

        if (c->serial != want) continue;
        bx = (int)(c->x >> (16 + 5));
        by = (int)(c->y >> (16 + 5));
        if (bx >= 0 && bx < GTA_MAP_DIM && by >= 0 && by < GTA_MAP_DIM &&
            gta_map_block(mp, bx, by, c->layer, &b))
            g = gta_block_ground_type(&b);
        if (c->path_i < c->path_n) { nx = c->path[c->path_i].x;
                                     ny = c->path[c->path_i].y; }
        if (c->path_i + 1 < c->path_n) { n2x = c->path[c->path_i + 1].x;
                                         n2y = c->path[c->path_i + 1].y; }
        printf("  T%-5d blk(%3d,%3d) g%d in(%2ld,%2ld) ang%3d face%3d turn%2d "
               "lock%2d spd%2ld.%02ld lane%2d hold%d wait%3d | node %d/%d "
               "(%d,%d)->(%d,%d) | cell(%d,%d)\n",
               t, bx, by, g, (c->x >> 16) & 31, (c->y >> 16) & 31,
               c->angle, c->face, c->turn, c->turn_lock,
               c->speed >> 16, ((c->speed & 0xFFFF) * 100) >> 16,
               c->lane_target, c->hold, c->wait,
               c->path_i, c->path_n, nx, ny, n2x, n2y,
               c->cell_x, c->cell_y);
        return;
    }
}

/* DOES THE ROUTE ITSELF GO OVER THE PAVEMENT?
 *
 * The obvious question, and one nothing here has ever asked. The route search
 * expands on the map's DIRECTION BITS - "may traffic leave this block that
 * way" - and a block can carry direction bits without being road, or be
 * reachable from a block that does. If a path node is a pavement block then a
 * car driving perfectly along its route is on the pavement, which would make
 * every lane-keeping instrument in this project measure the wrong thing.
 *
 * Also checks that consecutive nodes are 4-CONNECTED. The forensic dump shows
 * cars whose current node is diagonally adjacent to where they are, and a grid
 * route has no business producing a diagonal step. Whether that is a route
 * fault or a car that has wandered off its route is exactly what this
 * separates. */
static void audit_paths(const gta_map *mp, const gta_traffic *tr,
                        long *nodes, long *bad_ground, long *bad_step,
                        int *ex_bx, int *ex_by, int *ex_g, int show)
{
    int i, k;

    for (i = 0; i < tr->n; i++) {
        const gta_car *c = &tr->cars[i];
        for (k = 0; k < c->path_n; k++) {
            int qx = c->path[k].x, qy = c->path[k].y, qz = c->layer;
            int g = 0, lz;
            gta_block b;

            (*nodes)++;
            for (lz = qz - 1; lz <= qz + 1; lz++) {
                if (lz < 0 || lz >= GTA_MAP_LAYERS) continue;
                if (qx < 0 || qx >= GTA_MAP_DIM || qy < 0 || qy >= GTA_MAP_DIM)
                    continue;
                if (gta_map_block(mp, qx, qy, lz, &b) &&
                    gta_block_ground_type(&b) == 2) { g = 2; break; }
            }
            if (g != 2) {
                if (qx >= 0 && qx < GTA_MAP_DIM && qy >= 0 && qy < GTA_MAP_DIM &&
                    gta_map_block(mp, qx, qy, qz, &b))
                    g = gta_block_ground_type(&b);
                (*bad_ground)++;
                if (*ex_bx < 0) { *ex_bx = qx; *ex_by = qy; *ex_g = g; }
                if (show) {
                    printf("  ROUTE OVER NON-ROAD: car#%lu node %d/%d = "
                           "(%d,%d,%d) ground %d\n",
                           c->serial, k, c->path_n, qx, qy, qz, g);
                    show = 0;
                }
            }
            if (k > 0) {
                int dx = qx - c->path[k - 1].x, dy = qy - c->path[k - 1].y;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx + dy != 1)
                    (*bad_step)++;
            }
        }
    }
}

/* THE FIRST FEW TIMES A CAR PUTS ITS MIDDLE ON A NON-ROAD BLOCK, IN FULL.
 *
 * The engine counts these (gta_traffic.stat_offroad) and buckets them by
 * cause, and the buckets said "straight on a street, and it never comes back"
 * - which is a shape, not a diagnosis. This prints the whole state of the car
 * on the tick it happens, because the next question is always "which block,
 * what ground, and what was it aiming at", and a histogram cannot answer it.
 *
 * Tracked BY SERIAL, not by slot: tr->cars[] is compacted every tick, so slot
 * 3 is a different vehicle from one tick to the next and an edge detector
 * indexed by slot invents transitions that never happened. That mistake has
 * already cost this project one wrong measurement (gta_traffic.stat_slide).
 *
 * Prints at most `limit` of them and then goes quiet, because a car that is on
 * the pavement STAYS on the pavement and the useful line is the first one. */
static int pavement_watch(const gta_map *mp, const gta_tiles *ti,
                          const gta_traffic *tr, int t, int shown, int limit)
{
    static unsigned long was_off[GTA_MAX_CARS * 4];
    static int n_was_off;
    int i, k;

    for (i = 0; i < tr->n; i++) {
        const gta_car *c = &tr->cars[i];
        int bx = (int)(c->x >> (16 + 5));
        int by = (int)(c->y >> (16 + 5));
        int g = 0, prev = 0, lz;
        gta_block b;

        for (lz = c->layer - 1; lz <= c->layer + 1; lz++) {
            if (lz < 0 || lz >= GTA_MAP_LAYERS) continue;
            if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) continue;
            if (gta_map_block(mp, bx, by, lz, &b) &&
                gta_block_ground_type(&b) == 2) { g = 2; break; }
        }
        if (!g && bx >= 0 && bx < GTA_MAP_DIM && by >= 0 && by < GTA_MAP_DIM &&
            gta_map_block(mp, bx, by, c->layer, &b))
            g = gta_block_ground_type(&b);

        for (k = 0; k < n_was_off; k++)
            if (was_off[k] == c->serial) { prev = 1; break; }

        if (g == 2) {                       /* on the road: clear the flag */
            if (prev) {
                was_off[k] = was_off[--n_was_off];
            }
            continue;
        }
        if (prev)
            continue;                       /* already reported this excursion */
        if (n_was_off < (int)(sizeof was_off / sizeof was_off[0]))
            was_off[n_was_off++] = c->serial;

        if (shown < limit) {
            const gta_car_info *ci = &ti->cars[c->model];
            int nx = -1, ny = -1;
            if (c->path_i < c->path_n) {
                nx = c->path[c->path_i].x;
                ny = c->path[c->path_i].y;
            }
            printf("  PAVEMENT t=%d car#%lu (%s %dx%d) block (%d,%d) ground %d "
                   "layer %d | angle %d face %d turn %d speed %ld.%02ld "
                   "lane %d | node %d/%d -> (%d,%d) | cell (%d,%d) "
                   "aim (%ld,%ld) | in-block %ld,%ld\n",
                   t, c->serial, gta_vehicle_class_name(ci->vtype),
                   ci->width, ci->length, bx, by, g, c->layer,
                   c->angle, c->face, c->turn,
                   c->speed >> 16, ((c->speed & 0xFFFF) * 100) >> 16,
                   c->lane_target, c->path_i, c->path_n, nx, ny,
                   c->cell_x, c->cell_y, c->tx >> 16, c->ty >> 16,
                   (c->x >> 16) & 31, (c->y >> 16) & 31);
            shown++;
        }
    }
    return shown;
}

static int count_offroad(const gta_map *mp, const gta_tiles *ti,
                         const gta_traffic *tr, int verbose)
{
    int i, off = 0;

    for (i = 0; i < tr->n; i++) {
        const gta_car_info *ci = &ti->cars[tr->cars[i].model];
        int cbx = (int)(tr->cars[i].x >> (16 + 5));
        int cby = (int)(tr->cars[i].y >> (16 + 5));
        /* THE SAMPLED BODY IS FOUR PIXELS SMALLER THAN THE ARTWORK, all round.
         *
         * A bus is 34 to 46 pixels wide and a lane is 32, so a wide vehicle
         * sitting perfectly in its lane ALWAYS has paint over the kerb - and
         * where the block beside it is a building, over the building. Testing
         * the outermost pixel therefore reports a fault that no driving could
         * avoid; what is worth catching is a body actually inside something,
         * not a wing mirror against a wall. Four pixels is the margin, and the
         * centre line is still tested at full length. */
        long half_l = (long)gta_car_world_len(ci) / 2 - 2;
        long half_w = (long)gta_car_world_wid(ci) / 2 - 2;
        /* THE BODY IS SAMPLED ALONG THE CAR'S REAL HEADING, not along the
         * compass direction it is nearest to. Cars are steered now, so a car
         * half way round a corner lies diagonally - and an axis-aligned box
         * for a 119-pixel bus mid-turn reaches two blocks into a building the
         * bus is nowhere near. That is how this test invented off-road faults
         * the moment the steering went in. */
        long fx = gta_sin(tr->cars[i].face);
        long fy = -gta_cos(tr->cars[i].face);
        long rx = -fy, ry = fx;
        int k, bad = 0;

        /* Three lines along the vehicle: its centre and both flanks. The
         * centre line was NOT sampled before - only the flanks were - so the
         * strict rule below is not a relaxation of what was tested, it is a
         * stricter test where it matters and a fair one where it does not. */
        for (k = -1; k <= 1 && !bad; k++) {
            long a;
            for (a = -half_l; a <= half_l && !bad; a += 16) {
                gta_block b;
                long ox = (a * fx + k * half_w * rx) >> 14;
                long oy = (a * fy + k * half_w * ry) >> 14;
                long wx = tr->cars[i].x + (ox << 16);
                long wy = tr->cars[i].y + (oy << 16);
                int qx = (int)(wx >> (16 + 5));
                int qy = (int)(wy >> (16 + 5));
                int g = 0;
                if (qx >= 0 && qx < GTA_MAP_DIM && qy >= 0 && qy < GTA_MAP_DIM &&
                    gta_map_block(mp, qx, qy, tr->cars[i].layer, &b))
                    g = gta_block_ground_type(&b);
                /* A CAR ON A RAMP IS ON TWO LAYERS AT ONCE, and this test used
                 * to call that off-road. Where the block at the car's own
                 * layer is EMPTY - not a building, nothing at all - and the
                 * one directly below or above it is road, the vehicle is
                 * straddling a slope, which is the only way a flyover can be
                 * left. The relaxation is deliberately narrow: a car whose
                 * body is inside a BUILDING is still reported, because that
                 * block is not empty. */
                if (g == 0 && qx >= 0 && qx < GTA_MAP_DIM &&
                    qy >= 0 && qy < GTA_MAP_DIM) {
                    int lz;
                    for (lz = tr->cars[i].layer - 1;
                         lz <= tr->cars[i].layer + 1; lz += 2) {
                        if (lz < 0 || lz >= GTA_MAP_LAYERS) continue;
                        if (gta_map_block(mp, qx, qy, lz, &b)) {
                            int g2 = gta_block_ground_type(&b);
                            if (g2 == 2 || g2 == 3) { g = g2; break; }
                        }
                    }
                }
                /* Road and pavement are both fine, and so is a verge UNDER
                 * THE VEHICLE'S SIDE: a bus is 40 pixels wide in a 32-pixel
                 * lane, so its flanks hang over whatever is beside the road by
                 * a few pixels wherever it goes. That is the artwork, not a
                 * placement fault - the original's buses do exactly the same.
                 *
                 * What is never acceptable is a vehicle inside a BUILDING or
                 * in the WATER, and those are what this still catches. The
                 * sample along the car's centre line is unchanged: a car whose
                 * MIDDLE is off the road is off the road. */
                if (g == 2 || g == 3) continue;
                if (k != 0 && g != 1 && g != 5) continue;
                if (verbose)
                    printf("  OFF-ROAD car %2d (%s, %dx%d) at (%d,%d) "
                           "angle %d face %d turn %d: block (%d,%d) ground %d\n",
                           i, gta_vehicle_class_name(ci->vtype),
                           ci->width, ci->length, cbx, cby,
                           tr->cars[i].angle, tr->cars[i].face,
                           tr->cars[i].turn, qx, qy, g);
                bad = 1;
            }
        }
        if (bad) off++;
    }
    return off;
}

/* --- DOES ANYTHING TURN ROUND? -------------------------------------------
 *
 * "In GTA NOTHING ever turned round at a junction - a turn was ninety degrees
 * at most, and here it often reverses." That is the developer's report and it
 * is the one traffic complaint no existing test could answer: the flow figure
 * is perfectly happy with a car that drives back the way it came, and the
 * overlap and off-road tests are happy with it too.
 *
 * A U-turn is measured as NET ROTATION rather than as a heading comparison,
 * because the two ways a car can reverse look nothing alike in a single frame:
 *
 *   - two ninety-degree turns the same way, one per junction block, a second
 *     apart. Each one is legal; the pair is a U-turn. This is the common one
 *     and no per-tick test can see it.
 *   - the instant flip, where the driver assigns the opposite heading in one
 *     tick (the `d == 128` arm of the turn code).
 *
 * So the signed heading change is accumulated per car and the total is judged.
 * It is reset once the car has driven straight for half a second, which is
 * what separates "one corner" from "a corner and then another corner".
 *
 * Cars are followed by SERIAL, not by index: the fleet array is compacted
 * whenever a car is retired, so slot 3 is a different vehicle from one tick to
 * the next, and a tracker keyed on the index invents rotations nobody made. */
#define UT_SLOTS (GTA_MAX_CARS * 4)
#define UT_STRAIGHT 25          /* ticks without a turn that end a manoeuvre */
#define UT_UTURN   112          /* net 256ths of a circle that count as round */
#define UT_FLIP     16          /* a one-tick change bigger than any steering */
#define UT_TRAIL     8          /* blocks of history kept per car */

typedef struct {
    unsigned long serial;
    int face, spin, straight, flagged, live;
    /* Where the manoeuvre STARTED - the block and the heading the car had
     * when its spin last stood at zero. Reporting only where it ended says
     * nothing about how it got into that position, and for the reversals that
     * survived the route fix that is the whole question. */
    int from_face, from_x, from_y, from_dirs;
    /* The last few blocks this car occupied, newest last, so the print can
     * show how it got into the position it is turning round out of. A car
     * facing a way its block forbids did not start there; it drove there. */
    int trail_x[UT_TRAIL], trail_y[UT_TRAIL], trail_f[UT_TRAIL], trail_n;
} ut_track;

typedef struct {
    ut_track s[UT_SLOTS];
    int uturns;             /* gradual: two turns the same way */
    int flips;              /* instant: the heading assigned outright */
    int at_junction;        /* of the above, how many were on a junction block */
    int worst_x, worst_y;   /* where the last one was */
    int verbose;            /* print every event with the car's route state */
    int tick;
} ut_state;

static void ut_init(ut_state *u)
{
    memset(u, 0, sizeof *u);
}

/* The signed shortest way round from `a` to `b` on a 256-unit circle. */
static int ut_delta(int a, int b)
{
    int d = (b - a) & 255;
    if (d > 128) d -= 256;
    return d;
}

static void ut_tick(ut_state *u, const gta_traffic *tr, const gta_map *mp)
{
    int i, k;

    for (i = 0; i < UT_SLOTS; i++) u->s[i].live = 0;

    for (i = 0; i < tr->n; i++) {
        const gta_car *c = &tr->cars[i];
        ut_track *t = 0;
        gta_block jb;
        int d, bx, by, junc, on_j;

        for (k = 0; k < UT_SLOTS; k++)
            if (u->s[k].serial == c->serial) { t = &u->s[k]; break; }
        if (!t) {
            for (k = 0; k < UT_SLOTS; k++)
                if (u->s[k].serial == 0) { t = &u->s[k]; break; }
            if (!t) continue;               /* cannot happen; not trusted */
            memset(t, 0, sizeof *t);
            t->serial = c->serial;
            t->face = c->face;
        }
        t->live = 1;

        d  = ut_delta(t->face, c->face);
        bx = (int)(c->x >> 21);
        by = (int)(c->y >> 21);
        junc = gta_map_block(mp, bx, by, c->layer, &jb) ? gta_block_dirs(&jb) : 0;
        on_j = ((junc & 0x03) != 0) && ((junc & 0x0C) != 0);
        t->face = c->face;

        if (d > UT_FLIP || d < -UT_FLIP) {
            /* One tick, more than any steering rate: an assignment. */
            u->flips++;
            if (on_j) u->at_junction++;
            u->worst_x = bx; u->worst_y = by;
            if (u->verbose)
                printf("  t=%5d FLIP   car#%lu at (%3d,%3d)%s "
                       "face %d  route %d/%d\n",
                       u->tick, c->serial, bx, by, on_j ? " JUNCTION" : "",
                       c->face, c->path_i, c->path_n);
            t->spin = 0; t->flagged = 0; t->straight = 0;
            continue;
        }

        /* One entry per BLOCK entered, not per tick. */
        if (t->trail_n == 0 ||
            t->trail_x[t->trail_n - 1] != bx || t->trail_y[t->trail_n - 1] != by) {
            if (t->trail_n == UT_TRAIL) {
                int q;
                for (q = 1; q < UT_TRAIL; q++) {
                    t->trail_x[q - 1] = t->trail_x[q];
                    t->trail_y[q - 1] = t->trail_y[q];
                    t->trail_f[q - 1] = t->trail_f[q];
                }
                t->trail_n--;
            }
            t->trail_x[t->trail_n] = bx;
            t->trail_y[t->trail_n] = by;
            t->trail_f[t->trail_n] = c->face;
            t->trail_n++;
        }

        if (t->spin == 0) {
            t->from_face = c->face; t->from_x = bx; t->from_y = by;
            t->from_dirs = junc;
        }
        t->spin += d;
        if (c->turn == 0) t->straight++; else t->straight = 0;
        if (t->straight >= UT_STRAIGHT) { t->spin = 0; t->flagged = 0; }

        if (!t->flagged && (t->spin >= UT_UTURN || t->spin <= -UT_UTURN)) {
            u->uturns++;
            if (on_j) u->at_junction++;
            u->worst_x = bx; u->worst_y = by;
            if (u->verbose) {
                int q;
                static const char *dn[4] = { "N", "E", "S", "W" };
                printf("  t=%5d U-TURN car#%lu  from (%3d,%3d) facing %s "
                       "[arrows %c%c%c%c]  to (%3d,%3d)%s  spun %d  "
                       "route %d/%d\n",
                       u->tick, c->serial,
                       t->from_x, t->from_y, dn[((t->from_face + 32) & 255) >> 6],
                       (t->from_dirs & 1) ? 'N' : '.',
                       (t->from_dirs & 8) ? 'E' : '.',
                       (t->from_dirs & 2) ? 'S' : '.',
                       (t->from_dirs & 4) ? 'W' : '.',
                       bx, by, on_j ? " JUNCTION" : "",
                       t->spin, c->path_i, c->path_n);
                {
                    int q;
                    printf("        came:");
                    for (q = 0; q < t->trail_n; q++) {
                        gta_block tb;
                        int td = gta_map_block(mp, t->trail_x[q], t->trail_y[q],
                                               c->layer, &tb)
                                 ? gta_block_dirs(&tb) : 0;
                        printf(" (%d,%d)%s[%c%c%c%c]",
                               t->trail_x[q], t->trail_y[q],
                               dn[((t->trail_f[q] + 32) & 255) >> 6],
                               (td & 1) ? 'N' : '.', (td & 8) ? 'E' : '.',
                               (td & 2) ? 'S' : '.', (td & 4) ? 'W' : '.');
                    }
                    printf("\n");
                }
                /* AND THE ROUTE ITSELF, which is the only thing that can say
                 * whether the driver invented the reversal or was told to make
                 * it. Six nodes back and eight forward is the whole of a
                 * doubling-back manoeuvre on a dual carriageway. */
                printf("        route:");
                for (q = c->path_i - 6; q < c->path_i + 8; q++) {
                    if (q < 0 || q >= c->path_n) continue;
                    printf(" %s(%d,%d)", q == c->path_i ? ">" : "",
                           c->path[q].x, c->path[q].y);
                }
                printf("  dest (%d,%d)\n", c->dest_x, c->dest_y);
            }
            t->flagged = 1;
        }
    }

    for (i = 0; i < UT_SLOTS; i++)
        if (!u->s[i].live) u->s[i].serial = 0;
}

/* Cars that are inside each other.
 *
 * "They drive into the back of the one in front" is a claim about DISTANCE,
 * and the off-road test cannot see it at all - two cars occupying the same
 * stretch of tarmac are both perfectly on the road. So it gets its own check:
 * any two vehicles in the same lane whose bumpers have passed through each
 * other are counted, with a couple of pixels of tolerance because a queue that
 * touches is a queue, not a crash.
 *
 * Same-lane is a lateral test, not a block test - see gap_ahead() for why. */
static int count_overlaps(const gta_tiles *ti, const gta_traffic *tr,
                          int verbose, int tick)
{
    int i, j, bad = 0;

    for (i = 0; i < tr->n; i++)
    for (j = i + 1; j < tr->n; j++) {
        const gta_car *a = &tr->cars[i], *b = &tr->cars[j];
        long dx, dy, along, side, need;
        int ns;

        if (a->layer != b->layer) continue;

        /* CARS ON DIFFERENT AXES ARE A CRASH, NOT A QUEUE.
         *
         * This used to skip them entirely, on the grounds that a car crossing
         * a junction at right angles is not "in the back of" another. True,
         * and it meant the test was blind to the thing that actually goes
         * wrong at a junction: two cars in the same box at once, driving
         * through each other. Reported from the screen, invisible here.
         *
         * They cannot be compared bumper to bumper - the lengths lie along
         * different axes - so it is a plain proximity test: centres closer
         * than half a block in BOTH axes means they are sharing the same piece
         * of road. */
        if (((a->angle & 127) == 64) != ((b->angle & 127) == 64)) {
            long ax = b->x - a->x, ay = b->y - a->y;
            if (ax < 0) ax = -ax;
            if (ay < 0) ay = -ay;
            if (ax < (16L << 16) && ay < (16L << 16)) {
                if (verbose)
                    printf("  CROSSING t=%d cars %d and %d: %ld,%ld px apart"
                           "  angles %d/%d\n",
                           tick, i, j, ax >> 16, ay >> 16, a->angle, b->angle);
                bad++;
            }
            continue;
        }

        /* PROJECTED ONTO THE FIRST CAR'S REAL HEADING. Same reason as the
         * off-road test: with steering, "along" and "across" are the car's
         * own axes and not the compass's, and half way round a corner the two
         * differ by 45 degrees - which is enough to call a perfectly spaced
         * pair a collision. */
        (void)ns;
        dx = b->x - a->x; dy = b->y - a->y;
        {
            long fx = gta_sin(a->face), fy = -gta_cos(a->face);
            along = ((dx >> 8) * fx + (dy >> 8) * fy) >> 6;
            side  = ((dx >> 8) * -fy + (dy >> 8) * fx) >> 6;
        }
        if (along < 0) along = -along;
        if (side  < 0) side  = -side;
        if (side > (12L << 16)) continue;          /* different lane */

        need = ((long)gta_car_world_len(&ti->cars[a->model]) / 2 +
                (long)gta_car_world_len(&ti->cars[b->model]) / 2) << 16;
        if (along + (3L << 16) < need) {
            if (verbose)
                printf("  OVERLAP t=%d cars %d and %d: %ld px apart, need %ld"
                       "  speeds %ld/%ld  angles %d/%d"
                       "  serials %lu/%lu at blk(%ld,%ld)/(%ld,%ld) "
                       "cross %d/%d turn %d/%d\n",
                       tick, i, j, along >> 16, need >> 16,
                       a->speed, b->speed, a->angle, b->angle,
                       a->serial, b->serial,
                       a->x >> (16 + 5), a->y >> (16 + 5),
                       b->x >> (16 + 5), b->y >> (16 + 5),
                       a->crossing, b->crossing, a->turn, b->turn);
            bad++;
        }
    }
    return bad;
}

/* THE DRIVING TEST.
 *
 * Runs the fleet for as many simulation ticks as asked and checks, EVERY TICK,
 * that no car has left the road. That is the invariant traffic has to hold and
 * it is the one a picture cannot check: a car that clips a building for three
 * ticks at a corner looks fine in any frame that does not happen to catch it.
 *
 * It also reports how far the fleet actually travelled, because a traffic
 * system that deadlocks - every car waiting for the one in front, all the way
 * round - passes an on-the-road test perfectly while doing nothing at all.
 *
 * Frames are written every `every` ticks so the run can also be looked at. */
/* DO CARS USE BOTH LANES OF A TWO-LANE CARRIAGEWAY, OR ONLY THE KERB ONE?
 *
 * "auta jezdza tylko calkiem prawym pasem - przestaly jezdzic lewym z 2 pasow.
 * dlatego nie ma skretow w lewo bo nikt nie jest na tej lane gdzie jest
 * mozliwy taki skret"
 *
 * That is a testable claim and it had no instrument, so here is one. For every
 * car standing on an ordinary road block, look at the block to its LEFT and
 * the block to its RIGHT in the navigation grid. A neighbour that is road AND
 * carries the same direction bit is another lane of the same carriageway.
 * Count how wide the carriageway is under each car and where in it the car is
 * sitting.
 *
 * The grid is the engine's own (`gta_nav_at`), so this cannot disagree with
 * the driving code about what a lane is - which is the mistake three earlier
 * instruments in this project made. */
static void lanewatch(const gta_nav *nav, const gta_traffic *tr,
                      long *w1, long *w2_left, long *w2_right,
                      long *w3_left, long *w3_mid, long *w3_right)
{
    static const int dxs[4] = { 0,  1,  0, -1 };     /* N E S W */
    static const int dys[4] = {-1,  0,  1,  0 };
    static const unsigned char bit[4] = { GTA_NAV_N, GTA_NAV_E,
                                          GTA_NAV_S, GTA_NAV_W };
    int i;

    for (i = 0; i < tr->n; i++) {
        const gta_car *c = &tr->cars[i];
        int bx = (int)(c->x >> (16 + 5));
        int by = (int)(c->y >> (16 + 5));
        int q  = ((c->angle & 255) + 32) >> 6;       /* 0 N, 1 E, 2 S, 3 W */
        int lq, rq, lx, ly, rx, ry, lanes, pos;
        unsigned char me, l, r;

        if (c->done) continue;
        q &= 3;
        me = gta_nav_at(nav, bx, by, c->layer);
        if (gta_nav_ground(me) != 2 || !(gta_nav_dirs(me) & bit[q]))
            continue;                                /* not driving road here */

        lq = (q + 3) & 3;                            /* a quarter turn left */
        rq = (q + 1) & 3;
        lx = bx + dxs[lq]; ly = by + dys[lq];
        rx = bx + dxs[rq]; ry = by + dys[rq];
        l = gta_nav_at(nav, lx, ly, c->layer);
        r = gta_nav_at(nav, rx, ry, c->layer);

        lanes = 1;
        pos = 0;                                     /* 0 = leftmost */
        if (gta_nav_ground(l) == 2 && (gta_nav_dirs(l) & bit[q])) {
            lanes++;
            pos++;                                   /* somebody is left of us */
        }
        if (gta_nav_ground(r) == 2 && (gta_nav_dirs(r) & bit[q]))
            lanes++;

        if (lanes == 1) { (*w1)++; continue; }
        if (lanes == 2) {
            if (pos == 0) (*w2_left)++; else (*w2_right)++;
            continue;
        }
        if (pos == 0)      (*w3_left)++;
        else               (*w3_mid)++;
        (void)w3_right;
    }
}

/* DID ANY CAR TURN OUT OF A BLOCK THAT DOES NOT ALLOW IT?
 *
 * "z prawej alejki w lewo skreca"
 *
 * The engine now refuses those, and a rule that measures itself proves
 * nothing. This watches the OUTCOME instead and shares no code with the rule:
 * remember, per car, the block it was standing on when a turn began; when the
 * turn ends, look up that block in the navigation grid and ask whether it
 * carries the direction the car has ended up travelling in. If it does not,
 * the car turned out of a lane whose painted arrows forbid it.
 *
 * The grid is the same one the map's arrows are drawn from, so this is exactly
 * the question a player asks looking at the road. */
static void arrowwatch(const gta_nav *nav, const gta_traffic *tr,
                       int *was_turning, int *from_x, int *from_y,
                       unsigned long *who,
                       long *legal, long *illegal, int *shown, int cap)
{
    int i;

    for (i = 0; i < tr->n && i < GTA_MAX_CARS; i++) {
        const gta_car *c = &tr->cars[i];
        int turning = (c->turn != 0);

        /* THE SLOT IS NOT THE CAR. The fleet array is compacted when vehicles
         * are retired, so slot i holds a different car from one tick to the
         * next, and an instrument that remembers "slot i started a turn at
         * block P" compares one car's start with another car's finish. That
         * is what produced 186 "illegal turns" that no rule could ever refuse,
         * through three attempts at a rule that was working the whole time.
         * The same mistake is recorded in PROGRESS.md for the distance
         * counter and for the vanish detector; it is the third time. */
        if (who[i] != c->serial) {
            who[i] = c->serial;
            was_turning[i] = turning;
            from_x[i] = (int)(c->x >> (16 + 5));
            from_y[i] = (int)(c->y >> (16 + 5));
            continue;
        }

        if (turning && !was_turning[i]) {
            /* THE LANE THE CAR CAME IN ON, not the block it happens to be
             * standing on when the arc begins - by then it is usually over the
             * junction, which carries every direction and has no opinion. */
            from_x[i] = c->lane_bx;
            from_y[i] = c->lane_by;
        } else if (!turning && was_turning[i]) {
            unsigned char v = gta_nav_at(nav, from_x[i], from_y[i], c->layer);
            int a = c->angle & 255;
            unsigned char want = (a == 0)   ? GTA_NAV_N
                               : (a == 64)  ? GTA_NAV_E
                               : (a == 128) ? GTA_NAV_S : GTA_NAV_W;
            if (gta_nav_ground(v) != 2) {
                /* the turn started off the road: a different fault, not this */
            } else if (gta_nav_dirs(v) & want) {
                (*legal)++;
            } else {
                (*illegal)++;
                if (*shown < cap) {
                    printf("  ARROW car#%lu turned out of block (%d,%d) "
                           "heading %d - that block allows %s%s%s%s only\n",
                           c->serial, from_x[i], from_y[i], a,
                           (gta_nav_dirs(v) & GTA_NAV_N) ? "N" : "",
                           (gta_nav_dirs(v) & GTA_NAV_S) ? "S" : "",
                           (gta_nav_dirs(v) & GTA_NAV_W) ? "W" : "",
                           (gta_nav_dirs(v) & GTA_NAV_E) ? "E" : "");
                    (*shown)++;
                }
            }
        }
        was_turning[i] = turning;
    }
}

/* HOW OFTEN DOES A MOVING CAR PASS THROUGH A STOPPED ONE?
 *
 * "zobacz na te cysterne - wszyscy przez nia przejezdzaja"
 *
 * The overlap counter reports the WORST number of overlapping pairs at any one
 * instant, which is dominated by knots at busy crossings and says nothing
 * about this. This counts car-ticks in which a MOVING vehicle's body is inside
 * a STOPPED one's - the tanker case exactly - using the same oriented boxes
 * the engine uses. */
static long drivethrough(const gta_tiles *ti, const gta_traffic *tr)
{
    long n = 0;
    int i, j;

    for (i = 0; i < tr->n; i++) {
        const gta_car *a = &tr->cars[i];
        if (a->done || a->speed <= 0) continue;
        for (j = 0; j < tr->n; j++) {
            const gta_car *b = &tr->cars[j];
            long dx, dy;
            if (i == j || b->done || b->speed > 0 || b->layer != a->layer)
                continue;
            dx = (b->x - a->x) >> 16;
            dy = (b->y - a->y) >> 16;
            if (dx > 80 || dx < -80 || dy > 80 || dy < -80) continue;
            /* Oriented boxes, the same separating-axis test the engine uses,
             * written out here so the instrument shares no code with the rule
             * it is measuring. */
            {
                const gta_car_info *ai = &ti->cars[a->model];
                const gta_car_info *bi = &ti->cars[b->model];
                int ahl = gta_car_world_len(ai) / 2;
                int ahw = gta_car_world_wid(ai) / 2;
                int bhl = gta_car_world_len(bi) / 2;
                int bhw = gta_car_world_wid(bi) / 2;
                long ax[4][2];
                int k, sep = 0;
                ax[0][0] =  gta_sin(a->face); ax[0][1] = -gta_cos(a->face);
                ax[1][0] =  gta_cos(a->face); ax[1][1] =  gta_sin(a->face);
                ax[2][0] =  gta_sin(b->face); ax[2][1] = -gta_cos(b->face);
                ax[3][0] =  gta_cos(b->face); ax[3][1] =  gta_sin(b->face);
                for (k = 0; k < 4 && !sep; k++) {
                    long nx = ax[k][0], ny = ax[k][1];
                    long r = dx * nx + dy * ny;
                    long pa, pb, t;
                    if (r < 0) r = -r;
                    t = (ax[0][0]*nx + ax[0][1]*ny) >> 14;
                    pa = (t < 0 ? -t : t) * ahl;
                    t = (ax[1][0]*nx + ax[1][1]*ny) >> 14;
                    pa += (t < 0 ? -t : t) * ahw;
                    t = (ax[2][0]*nx + ax[2][1]*ny) >> 14;
                    pb = (t < 0 ? -t : t) * bhl;
                    t = (ax[3][0]*nx + ax[3][1]*ny) >> 14;
                    pb += (t < 0 ? -t : t) * bhw;
                    if (r > pa + pb) sep = 1;
                }
                if (!sep) n++;
            }
        }
    }
    return n;
}

/* The reason a car is stopped, as a word. */
static const char *hold_name(int h)
{
    switch (h) {
    case GTA_HOLD_NONE:    return "none";
    case GTA_HOLD_QUEUE:   return "queue";
    case GTA_HOLD_LIGHT:   return "light";
    case GTA_HOLD_BOX:     return "junction-box";
    case GTA_HOLD_MERGE:   return "merge";
    case GTA_HOLD_DEADEND: return "dead-end";
    case GTA_HOLD_ROAD:    return "road";
    case GTA_HOLD_GAP:     return "gap";
    default:               return "?";
    }
}

/* IS THE JUNCTION RESERVATION LEAKING, AND HOW?
 *
 * The overlaps that are left are inside crossings, and six geometric answers
 * to them were built and rejected on 2026-08-24 (PROGRESS.md 31) because they
 * all gridlock the city. What a crossing needs is arbitration, and this port
 * has one - junction_claim() - so the question is whether it is being honoured
 * and simply too small, or not being honoured at all.
 *
 * Every tick: bucket the cars by the junction ROOT their centre is standing
 * in. A root holding two or more cars at once is what should never happen.
 * For each of those, the claim table is read directly - it is public struct -
 * so the report can say whether one of them owns the box, whether NOBODY owns
 * it (the claim aged out under a car that stopped, which the engine does on
 * purpose), or whether the owner is not even in there.
 *
 * The three ways in, for the reader of the output:
 *   - the entry gate,     drive_one section 4, which calls junction_claim()
 *   - the arc,            committed on the approach block, outside the box
 *   - recover_offroad(),  which teleports and asks nobody
 */
static void boxwatch(const gta_map *mp, gta_traffic *tr, int t,
                     long *ct_multi, long *ct_noowner, long *ct_owner_out,
                     long *ct_turning, long *ct_stopped, int *shown, int cap)
{
    int rx[GTA_MAX_CARS], ry[GTA_MAX_CARS], root_of[GTA_MAX_CARS];
    int i, j;

    for (i = 0; i < tr->n; i++) {
        int bx = (int)(tr->cars[i].x >> (16 + 5));
        int by = (int)(tr->cars[i].y >> (16 + 5));
        root_of[i] = -1;
        if (tr->cars[i].done) continue;
        if (!gta_traffic_is_junction(mp, bx, by, tr->cars[i].layer)) continue;
        gta_traffic_junction_root(mp, bx, by, tr->cars[i].layer, &rx[i], &ry[i]);
        root_of[i] = 1;
    }

    for (i = 0; i < tr->n; i++) {
        int n = 1, owner_here = 0, owner_known = 0, turning = 0, stopped = 0;
        unsigned long owner = 0;
        int k;

        if (root_of[i] < 0) continue;
        /* only report each root once: the lowest index in it speaks for it */
        for (j = 0; j < i; j++)
            if (root_of[j] >= 0 && rx[j] == rx[i] && ry[j] == ry[i] &&
                tr->cars[j].layer == tr->cars[i].layer)
                break;
        if (j < i) continue;

        for (j = i + 1; j < tr->n; j++)
            if (root_of[j] >= 0 && rx[j] == rx[i] && ry[j] == ry[i] &&
                tr->cars[j].layer == tr->cars[i].layer)
                n++;
        if (n < 2) continue;

        for (k = 0; k < GTA_CLAIM_MAX; k++)
            if (tr->claim_ttl[k] > 0 &&
                tr->claim_x[k] == (unsigned char)rx[i] &&
                tr->claim_y[k] == (unsigned char)ry[i] &&
                tr->claim_z[k] == (signed char)tr->cars[i].layer) {
                owner_known = 1;
                owner = tr->claim_car[k];
                break;
            }

        for (j = 0; j < tr->n; j++) {
            if (root_of[j] < 0 || rx[j] != rx[i] || ry[j] != ry[i] ||
                tr->cars[j].layer != tr->cars[i].layer)
                continue;
            if (owner_known && tr->cars[j].serial == owner) owner_here = 1;
            if (tr->cars[j].turn != 0) turning++;
            if (tr->cars[j].speed == 0) stopped++;
        }

        *ct_multi += n;
        if (!owner_known)             *ct_noowner += n;
        else if (!owner_here)         *ct_owner_out += n;
        *ct_turning += turning;
        *ct_stopped += stopped;

        if (*shown < cap) {
            printf("  BOX t=%d root(%d,%d) layer %d - %d cars, claim %s\n",
                   t, rx[i], ry[i], tr->cars[i].layer, n,
                   !owner_known ? "NOBODY OWNS IT"
                                : (owner_here ? "held by one of them"
                                              : "held by a car that has left"));
            for (j = 0; j < tr->n; j++) {
                if (root_of[j] < 0 || rx[j] != rx[i] || ry[j] != ry[i] ||
                    tr->cars[j].layer != tr->cars[i].layer)
                    continue;
                printf("      car#%lu at (%ld,%ld) angle %d face %d "
                       "speed %ld turn %d wait %d hold %s%s\n",
                       tr->cars[j].serial,
                       tr->cars[j].x >> (16 + 5), tr->cars[j].y >> (16 + 5),
                       tr->cars[j].angle, tr->cars[j].face,
                       tr->cars[j].speed >> 10, tr->cars[j].turn,
                       tr->cars[j].wait, hold_name(tr->cars[j].hold),
                       (owner_known && tr->cars[j].serial == owner)
                           ? "   <- OWNS THE BOX" : "");
            }
            (*shown)++;
        }
    }
}

static int cmd_drive(const char *mapPath, const char *tilesPath,
                     int bx, int by, const char *prefix,
                     int ticks, int every, unsigned long seed, int zoom)
{
    gta_map mp;
    gta_tiles ti;
    gta_view view;
    gta_traffic tr;
    gta_nav nav;
    unsigned char *canvas;
    const int W = 320, H = 200;
    long moved_total = 0;
    int worst = 0, worst_ov = 0, t, i, shots = 0, n0, vanished = 0, lit = 0;
    int pave_shown = 0;
    int  aw_turning[GTA_MAX_CARS], aw_fx[GTA_MAX_CARS], aw_fy[GTA_MAX_CARS];
    unsigned long aw_who[GTA_MAX_CARS];
    long aw_legal = 0, aw_illegal = 0;
    long dt_total = 0;
    long sib_total = 0;
    long sib_committed = 0, sib_hold[GTA_HOLD_COUNT];
    int sib_shown = 0;
    int  aw_shown = 0;
    long lane_w1 = 0, lane_w2l = 0, lane_w2r = 0;
    long lane_w3l = 0, lane_w3m = 0, lane_w3r = 0;
    int  have_nav_grid = 0;
    long box_multi = 0, box_noowner = 0, box_owner_out = 0;
    long box_turning = 0, box_stopped = 0;
    int  box_shown = 0;
    unsigned long trace_serial = 0;
    long path_nodes = 0, path_bad_ground = 0, path_bad_step = 0;
    int pathx = -1, pathy = -1, pathg = -1;
    long moving_sum = 0, moving_ticks = 0, hold_hist[GTA_HOLD_COUNT];
    int stopped_for[GTA_MAX_CARS], worst_wait = 0, gate_shown = 0;
    char out[512];

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) { gta_map_free(&mp); return 1; }
    canvas = (unsigned char *)calloc((size_t)W * H, 1);
    if (!canvas) { gta_tiles_free(&ti); gta_map_free(&mp); return 1; }

    gta_render_init(&view, &mp, &ti);
    gta_render_target(&view, canvas, W, H, W);
    gta_render_look_at_block(&view, bx, by);
    if (zoom > 0)
        gta_render_set_zoom(&view, zoom);

    for (i = 0; i < GTA_MAX_CARS; i++) stopped_for[i] = 0;
    for (i = 0; i < GTA_HOLD_COUNT; i++) hold_hist[i] = 0;
    for (i = 0; i < GTA_HOLD_COUNT; i++) sib_hold[i] = 0;
    for (i = 0; i < GTA_MAX_CARS; i++) { aw_turning[i] = 0; aw_fx[i] = 0; aw_fy[i] = 0; aw_who[i] = 0; }

    gta_traffic_init(&tr, &ti, seed);
    /* The reservation overlay in every frame this test writes - the same
     * audit the developer runs in the game, available offline. */
    gta_render_set_overlay(&view, &tr, 1);
    /* The navigation grid. Traffic needs it for routes, and without it cars
     * fall back to following the arrows block by block - which is what this
     * port did before the original's model was read out of the binary, and
     * makes a useful A/B if it is ever wanted. */
    if (gta_nav_build(&nav, &mp) == 0) {
        gta_traffic_set_nav(&tr, &nav);
        have_nav_grid = 1;
    } else
        printf("drive: no memory for the navigation grid - no routes\n");
    /* THE FLEET HAS TO KNOW HOW MUCH CITY IS ON SCREEN. Without this the game
     * removes cars fourteen blocks from the camera whatever the zoom, and at 8
     * pixels a block that is inside the picture - which is the "cars disappear
     * here and there" report. Passing a zoom to this command is how that is
     * reproduced and how the fix is checked. */
    {
        const char *e = getenv("GTA_TRACE_CAR");
        if (e) {
            trace_serial = (unsigned long)atol(e);
            printf("drive: tracing car#%lu every tick\n", trace_serial);
            gta_traffic_trace_serial = trace_serial;
        }
    }

    tr.diag_corners = 1;
    gta_traffic_set_view_blocks(&tr, (W / 2) / view.zoom_px + 1);
    /* GTA_FLEET sweeps the fleet size. The original runs seven cars near the
     * view; this port runs twenty, and that is a suspect for every congestion
     * number in here. */
    {
        const char *e = getenv("GTA_OPT_CROSS");
        if (e) tr.opt_cross_lock = atoi(e);
        e = getenv("GTA_OPT_SWEEP");
        if (e) tr.opt_sweep = atoi(e);
        e = getenv("GTA_KEEPCLEAR");
        if (e) tr.opt_keepclear = atoi(e);
        e = getenv("GTA_LIGHTS");
        if (e) tr.opt_lights = atoi(e);
        e = getenv("GTA_OPT_ARROWS");
        if (e) tr.opt_arrows = atoi(e);
        e = getenv("GTA_OCC");
        if (e) tr.opt_occ = atoi(e);
        e = getenv("GTA_OCC");
        if (e) tr.opt_occ = atoi(e);
        e = getenv("GTA_BODY");
        if (e) tr.opt_body = atoi(e);
        e = getenv("GTA_HOLDBOX");
        if (e) tr.opt_holdbox = atoi(e);
        e = getenv("GTA_OPT_BOXROOT");
        if (e) tr.opt_boxroot = atoi(e);
        e = getenv("GTA_ARCCLAIM");
        if (e) tr.opt_arcclaim = atoi(e);
        e = getenv("GTA_HORIZON");
        if (e) tr.opt_horizon = atoi(e);
        e = getenv("GTA_OPT_BOXGAP");
        if (e) tr.opt_boxgap = atoi(e);
        e = getenv("GTA_UNWEDGE");
        if (e) tr.opt_unwedge = atoi(e);
        e = getenv("GTA_OPT_NOOVL");
        if (e) tr.opt_nooverlap = atoi(e);
        e = getenv("GTA_CREEP");
        if (e) tr.opt_creep = atoi(e);
        e = getenv("GTA_FLEET");
        if (e) {
            int f = atoi(e);
            if (f > 0 && f <= GTA_MAX_CARS) tr.fleet_cap = f;
        }
    }
    n0 = gta_traffic_park(&tr, &mp, bx, by, 8, tr.fleet_cap);
    printf("drive: %d cars around (%d,%d), %d ticks, seed %lu\n",
           n0, bx, by, ticks, seed);

    for (t = 0; t < ticks; t++) {
        /* Verbose only while nothing has gone wrong yet: the FIRST tick that
         * puts a car off the road is the one worth reading, and a car that is
         * off the road usually stays there, so printing every tick after it
         * buries the useful line under a thousand copies of itself. */
        int off;
        long px[GTA_MAX_CARS], py[GTA_MAX_CARS];
        int nbefore = tr.n, movers = 0;
        for (i = 0; i < nbefore; i++) { px[i] = tr.cars[i].x; py[i] = tr.cars[i].y; }

        gta_traffic_tick(&tr, &mp, view.cam_x, view.cam_y);

        pave_shown = pavement_watch(&mp, &ti, &tr, t, pave_shown, 12);
        boxwatch(&mp, &tr, t, &box_multi, &box_noowner, &box_owner_out,
                 &box_turning, &box_stopped, &box_shown, 10);
        if (have_nav_grid)
            lanewatch(&nav, &tr, &lane_w1, &lane_w2l, &lane_w2r,
                      &lane_w3l, &lane_w3m, &lane_w3r);
        if (have_nav_grid)
            arrowwatch(&nav, &tr, aw_turning, aw_fx, aw_fy, aw_who,
                       &aw_legal, &aw_illegal, &aw_shown, 12);
        dt_total += drivethrough(&ti, &tr);
        /* STOPPED INSIDE A CROSSING - the one place a car must never stand.
         * "wjechaly na skrzyzowanie i sie zatrzymaly pare pikseli po". */
        if (have_nav_grid) {
            int q;
            for (q = 0; q < tr.n; q++) {
                unsigned char vv;
                int dd;
                if (tr.cars[q].done || tr.cars[q].speed != 0) continue;
                vv = gta_nav_at(&nav, (int)(tr.cars[q].x >> (16 + 5)),
                                (int)(tr.cars[q].y >> (16 + 5)),
                                tr.cars[q].layer);
                dd = gta_nav_dirs(vv);
                /* MORE THAN ONE DIRECTION BIT is what a crossing looks like in
                 * the data - `gtadump dirmap` shows straight lane blocks with
                 * exactly one. gta_traffic_is_junction() is looser than that
                 * (it accepts any block carrying both axes, which includes an
                 * ordinary lane that permits a turn) and counting with it put
                 * every queue on an approach into this figure. */
                if (dd && (dd & (dd - 1)) != 0) {
                    sib_total++;
                    if (tr.cars[q].crossing) sib_committed++;
                    if (tr.cars[q].hold >= 0 && tr.cars[q].hold < GTA_HOLD_COUNT)
                        sib_hold[tr.cars[q].hold]++;
                    /* THE FIRST FEW INCIDENTS, WITH THE CAR THEY STAND
                     * BEHIND. A histogram says "queue"; only the pair says
                     * who let whom in. */
                    if (sib_shown < 8) {
                        int w, bestw = -1;
                        long bestd = 0x7fffffffL;
                        for (w = 0; w < tr.n; w++) {
                            long ddx, ddy, fwd, d2;
                            int sdx, sdy;
                            if (w == q || tr.cars[w].done) continue;
                            ddx = (tr.cars[w].x - tr.cars[q].x) >> 16;
                            ddy = (tr.cars[w].y - tr.cars[q].y) >> 16;
                            sdx = (tr.cars[q].angle == 64) ? 1
                                : (tr.cars[q].angle == 192) ? -1 : 0;
                            sdy = (tr.cars[q].angle == 128) ? 1
                                : (tr.cars[q].angle == 0) ? -1 : 0;
                            fwd = ddx * sdx + ddy * sdy;
                            d2 = ddx * ddx + ddy * ddy;
                            if (fwd <= 0 || d2 > 80L * 80L) continue;
                            if (d2 < bestd) { bestd = d2; bestw = w; }
                        }
                        printf("  SIB t=%d car#%lu blk(%d,%d) ang %d cross %d "
                               "turn %d hold %d wait %d path %d/%d "
                               "why %d side %d fell %d",
                               t, tr.cars[q].serial,
                               (int)(tr.cars[q].x >> (16 + 5)),
                               (int)(tr.cars[q].y >> (16 + 5)),
                               tr.cars[q].angle, tr.cars[q].crossing,
                               tr.cars[q].turn, tr.cars[q].hold,
                               tr.cars[q].wait,
                               tr.cars[q].path_i, tr.cars[q].path_n,
                               tr.cars[q].why_box, tr.cars[q].why_side,
                               tr.cars[q].why_fell);
                        if (bestw >= 0)
                            printf("  BEHIND car#%lu blk(%d,%d) ang %d spd %ld "
                                   "cross %d turn %d hold %d\n",
                                   tr.cars[bestw].serial,
                                   (int)(tr.cars[bestw].x >> (16 + 5)),
                                   (int)(tr.cars[bestw].y >> (16 + 5)),
                                   tr.cars[bestw].angle, tr.cars[bestw].speed,
                                   tr.cars[bestw].crossing, tr.cars[bestw].turn,
                                   tr.cars[bestw].hold);
                        else
                            printf("  BEHIND nobody within 80px ahead\n");
                        {
                            int cc;
                            printf("    MINE:");
                            for (cc = 0; cc < GTA_CLAIM_MAX; cc++)
                                if (tr.claim_ttl[cc] > 0 &&
                                    tr.claim_car[cc] == tr.cars[q].convoy)
                                    printf(" (%d,%d)%s", tr.claim_x[cc],
                                           tr.claim_y[cc],
                                           tr.claim_seen[cc] ? "s" : "");
                            if (bestw >= 0) {
                                printf("  THEIRS:");
                                for (cc = 0; cc < GTA_CLAIM_MAX; cc++)
                                    if (tr.claim_ttl[cc] > 0 &&
                                        tr.claim_car[cc] ==
                                            tr.cars[bestw].serial)
                                        printf(" (%d,%d)%s", tr.claim_x[cc],
                                               tr.claim_y[cc],
                                               tr.claim_seen[cc] ? "s" : "");
                            }
                            printf("\n");
                        }
                        sib_shown++;
                    }
                }
            }
        }
        trace_car(&mp, &tr, t, trace_serial);
        if ((t % 250) == 0)
            audit_paths(&mp, &tr, &path_nodes, &path_bad_ground, &path_bad_step,
                        &pathx, &pathy, &pathg, path_bad_ground == 0);

        /* DID ANY CAR DISAPPEAR WHILE IT WAS ON SCREEN?
         *
         * The reported fault - "vehicles vanish here and there" - is invisible
         * to every other test in here: the fleet stays the right size, nothing
         * leaves the road and nothing overlaps. It is a fault relative to the
         * CAMERA, so that is what this measures. A car that was inside the
         * visible rectangle before the tick has to still be near where it was
         * afterwards; slots are reused, so the test is by position and not by
         * index. The rectangle is shrunk by a block so a car legitimately
         * driving out of the picture at the edge is not counted. */
        {
            long halfw = (((long)(W / 2) * 32L / view.zoom_px) - 32L) << 16;
            long halfh = (((long)(H / 2) * 32L / view.zoom_px) - 32L) << 16;
            int j;
            for (i = 0; i < nbefore; i++) {
                long ddx = px[i] - view.cam_x, ddy = py[i] - view.cam_y;
                int found = 0;
                if (ddx < 0) ddx = -ddx;
                if (ddy < 0) ddy = -ddy;
                if (ddx > halfw || ddy > halfh) continue;   /* off screen anyway */
                for (j = 0; j < tr.n; j++) {
                    long ex = tr.cars[j].x - px[i], ey = tr.cars[j].y - py[i];
                    if (ex < 0) ex = -ex;
                    if (ey < 0) ey = -ey;
                    if (ex + ey < (8L << 16)) { found = 1; break; }
                }
                if (!found) {
                    if (vanished < 8)
                        printf("  VANISHED t=%d car was at block (%ld,%ld), "
                               "%ld,%ld world px from the camera\n",
                               t, px[i] >> (16 + 5), py[i] >> (16 + 5),
                               ddx >> 16, ddy >> 16);
                    vanished++;
                }
            }
        }

        /* DISTANCE IS ACCUMULATED PER TICK, not measured end to end.
         *
         * The first version of this compared each car against where it started
         * and reported ZERO after 500 ticks on a fleet that was moving
         * perfectly well - because cars are retired and replaced as the camera
         * moves, the list compacts, and slot i is not the same car any more.
         * The metric was wrong, not the traffic, and it printed FAILED, which
         * is the expensive way for a test to be wrong: it sent me looking for
         * a deadlock that was not there. */
        for (i = 0; i < tr.n && i < nbefore; i++) {
            long dx = tr.cars[i].x - px[i], dy = tr.cars[i].y - py[i];
            if (dx == 0 && dy == 0) continue;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            /* Only a plausible one-tick step counts as travel. A slot that was
             * refilled shows as a jump of many blocks and is not movement. */
            if (dx + dy < (4L << 16))
                moved_total += (dx + dy) >> 16;
            movers++;
        }
        if (t < 3)
            printf("  t=%d  %d of %d cars moving\n", t, movers, nbefore);

        /* THE SNAPSHOT AT THE END IS NOT THE MEASUREMENT.
         *
         * "N of 20 standing still" is one tick out of nine hundred, and a
         * junction that serialises traffic properly has cars stopped in it at
         * any instant - that is what a queue IS. Two numbers say whether the
         * city is actually flowing: how much of the fleet is moving on
         * average, and the longest any one car went without moving. The second
         * is the one that catches a permanent blockage, which is the fault
         * that matters and the one a snapshot cannot distinguish from a wait
         * at a busy corner. */
        moving_sum += movers;
        moving_ticks += nbefore;
        /* WHY the fleet is stopped, over the whole run rather than at the end.
         * The snapshot says how many; this says what is holding them, which is
         * the difference between a queue behind one blocked car and twenty
         * cars each waiting for something different. */
        for (i = 0; i < tr.n; i++)
            if (tr.cars[i].speed == 0 && tr.cars[i].hold >= 0 &&
                tr.cars[i].hold < GTA_HOLD_COUNT)
                hold_hist[tr.cars[i].hold]++;
        for (i = 0; i < tr.n && i < nbefore; i++) {
            long dx = tr.cars[i].x - px[i], dy = tr.cars[i].y - py[i];
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx + dy > (4L << 16)) { stopped_for[i] = 0; continue; } /* refilled slot */
            if (dx + dy > 0) stopped_for[i] = 0;
            else if (++stopped_for[i] > worst_wait) worst_wait = stopped_for[i];
            /* GATE AUTOPSY: ten seconds at a gate is a wedge, and the SIB
             * probe cannot see it (the car's middle is OUTSIDE the box).
             * Print the gate's own refusal reason for the first few. */
            if (stopped_for[i] == 300 && tr.cars[i].hold == GTA_HOLD_BOX &&
                gate_shown < 8) {
                gate_shown++;
                printf("  GATE-WEDGE t=%d car#%lu blk(%ld,%ld) ang %d "
                       "why %d side %d fell %d cross %d path %d/%d "
                       "want_route %d\n",
                       t, tr.cars[i].serial,
                       tr.cars[i].x >> (16 + 5), tr.cars[i].y >> (16 + 5),
                       tr.cars[i].angle, tr.cars[i].why_box,
                       tr.cars[i].why_side, tr.cars[i].why_fell,
                       tr.cars[i].crossing,
                       tr.cars[i].path_i, tr.cars[i].path_n,
                       tr.cars[i].want_route);
            }
        }

        off = count_offroad(&mp, &ti, &tr, worst == 0);
        if (off > worst) worst = off;
        {
            int ov = count_overlaps(&ti, &tr, worst_ov == 0, t);
            if (ov > worst_ov) worst_ov = ov;
        }

        if (every > 0 && (t % every) == 0 && shots < 40) {
            gta_traffic_draw(&tr, &view);
            gta_render_frame(&view);
            sprintf(out, "%s%02d.bmp", prefix, shots++);
            write_bmp8(out, canvas, W, H, ti.palette);
        }
    }

    /* WHO IS STANDING STILL, and what the map says where they stand.
     *
     * "some of the traffic is stopped at the start" is a claim the on-road and
     * overlap tests cannot see at all - a parked car passes both perfectly.
     * A car legitimately stops behind another; a car with nowhere legal to go
     * is a placement fault. The difference is in the direction bits under it,
     * so they get printed. */
    {
        int stalled = 0;
        for (i = 0; i < tr.n; i++) {
            gta_block b;
            int cbx = (int)(tr.cars[i].x >> (16 + 5));
            int cby = (int)(tr.cars[i].y >> (16 + 5));
            int d = 0;
            if (tr.cars[i].speed > 0) continue;
            /* A car at a red light is stopped and healthy. Counting it as a
             * stall would push the one number this test has for "is the
             * traffic moving" up every time the lights are obeyed. */
            if (tr.cars[i].at_light) { lit++; continue; }
            if (gta_map_block(&mp, cbx, cby, tr.cars[i].layer, &b))
                d = gta_block_dirs(&b);
            {
                static const char *const why[GTA_HOLD_COUNT] = {
                    "-", "queue", "light", "junction-box", "no-room-to-turn",
                    "DEAD END", "road-ahead", "gap", "reversing"
                };
                int hold = tr.cars[i].hold;
                /* WORLD PIXELS AS WELL AS THE BLOCK INDEX. Seven cars were
                 * reported "at (32,148)", and a block is 32 pixels wide, so
                 * either seven 60-pixel vehicles were inside one another or
                 * the block index was hiding where they really were. There is
                 * no way to tell those apart from a block index. */
                printf("  STOPPED car %2d at (%3d,%3d) px(%5ld,%5ld) "
                       "layer %d angle %3d  "
                       "dirs=%c%c%c%c  model %d len %d  held by %s\n",
                       i, cbx, cby,
                       tr.cars[i].x >> 16, tr.cars[i].y >> 16,
                       tr.cars[i].layer, tr.cars[i].angle,
                       (d & 1) ? 'N' : '.', (d & 2) ? 'S' : '.',
                       (d & 4) ? 'W' : '.', (d & 8) ? 'E' : '.',
                       tr.cars[i].model, ti.cars[tr.cars[i].model].length,
                       (hold >= 0 && hold < GTA_HOLD_COUNT) ? why[hold] : "?");
                /* And WHO is in the way. A stalled car is only half a report:
                 * the chain matters, because the difference between a queue
                 * that will clear and one that never will is at its head. */
                if (hold == GTA_HOLD_MERGE || hold == GTA_HOLD_BOX) {
                    int j;
                    for (j = 0; j < tr.n; j++) {
                        long ex, ey;
                        if (j == i) continue;
                        ex = tr.cars[j].x - tr.cars[i].x;
                        ey = tr.cars[j].y - tr.cars[i].y;
                        if (ex < 0) ex = -ex;
                        if (ey < 0) ey = -ey;
                        if (ex > (64L << 16) || ey > (64L << 16)) continue;
                        printf("        near: car %2d at (%3d,%3d) angle %3d "
                               "speed %ld hold %d\n", j,
                               (int)(tr.cars[j].x >> (16 + 5)),
                               (int)(tr.cars[j].y >> (16 + 5)),
                               tr.cars[j].angle, tr.cars[j].speed >> 16,
                               tr.cars[j].hold);
                    }
                }
            }
            stalled++;
        }
        printf("  %d of %d cars standing still, and %d more held at a red "
               "light\n", stalled, tr.n, lit);
    }

    printf("drive: after %d ticks - %d cars, worst off-road %d, "
           "worst overlaps %d, %d vanished on screen, %ld world px moved "
           "(%ld blocks)\n",
           ticks, tr.n, worst, worst_ov, vanished, moved_total,
           moved_total / 32);
    /* Traffic hitting traffic. Without this a change that moves none of the
     * numbers above cannot be told apart from a change that never runs. */
    printf("drive: fleet hits %ld (cars knocked loose %ld, settled %ld)\n",
           tr.stat_fleet_hits, tr.stat_knocked, tr.stat_knock_ended);
    /* THE TWO IMPOSSIBLE THINGS. A car that turns more than GTA_SANE_TURN or
     * moves more than GTA_SANE_STEP in one tick is a bug being watched, not a
     * manoeuvre - the developer's "obraca sie niemal o 360 stopni" and
     * "teleportuje sie o jakas ilosc pikseli", counted. */
    printf("drive: impossible - %ld turns (worst %ld of 256, state %ld), %ld jumps "
           "(worst %ld px, state %ld)\n",
           tr.stat_face_jump, tr.stat_face_jump_max, tr.stat_face_jump_ctx,
           tr.stat_pos_jump, tr.stat_pos_jump_max, tr.stat_pos_jump_ctx);
    {
        int with_route = 0;
        for (i = 0; i < tr.n; i++)
            if (tr.cars[i].path_i < tr.cars[i].path_n) with_route++;
        printf("drive: routes - %ld found (%ld blocks each on average), "
               "%ld failed, %d of %d cars on one now\n",
               tr.routes_ok,
               tr.routes_ok ? tr.route_nodes / tr.routes_ok : 0,
               tr.routes_failed, with_route, tr.n);
        /* AND WHERE THE PORT GAVE UP ON A CAR. Any non-zero count here means
         * traffic is still being put somewhere it cannot drive out of; the
         * block is the one worth looking at with `gtadump dirmap`. */
        if (tr.stat_abandoned)
            printf("drive: ABANDONED %ld cars (had not moved a block in %d "
                   "ticks), last at (%d,%d) layer %d\n",
                   tr.stat_abandoned, GTA_TRAFFIC_ABANDON,
                   tr.abandon_x, tr.abandon_y, tr.abandon_z);
    }
    /* THE DEADLOCK, COUNTED DIRECTLY - three or more cars stopped inside one
     * crossing at the same time. See gta_traffic.stat_boxlock: this is the
     * developer's photograph turned into a number, and it is the one to move.
     * Zero is the target and nothing else about it is negotiable. */
    /* THE SLIDE AFTER A CORNER - the developer's report as a distance. See
     * gta_traffic.stat_slide: this is how far a car moves SIDEWAYS in the 40
     * ticks after its arc finishes, which is the thing that is visible from
     * the pavement and the thing two earlier numbers both failed to show. */
    {
        long ns = tr.stat_slide[0] + tr.stat_slide[1]
                + tr.stat_slide[2] + tr.stat_slide[3];
        printf("drive: SLIDE AFTER A CORNER over %ld corners"
               " - 0-1 px %ld%%, 2-3 px %ld%%, 4-7 px %ld%%, 8+ px %ld%%"
               " (%ld px each on average)\n", ns,
               ns ? tr.stat_slide[0] * 100 / ns : 0,
               ns ? tr.stat_slide[1] * 100 / ns : 0,
               ns ? tr.stat_slide[2] * 100 / ns : 0,
               ns ? tr.stat_slide[3] * 100 / ns : 0,
               ns ? tr.stat_slide_px / ns : 0);
    }
    /* WHAT THE TRAFFIC ACTUALLY DOES AT A CROSSING, at ONE site.
     *
     * The developer, standing at a junction in the game: *"cars mostly go
     * straight... only the one on the outer lane turns, the one nearer the
     * middle never turns right, and I do not see anybody turning left at
     * all."* City-wide totals cannot answer that - they average 96 junctions
     * with different arrows - so the same split is printed per site. */
    {
        long st = tr.stat_cross_straight[0] + tr.stat_cross_straight[1]
                + tr.stat_cross_straight[2] + tr.stat_cross_straight[3];
        long tu = tr.stat_cross_turned[0] + tr.stat_cross_turned[1]
                + tr.stat_cross_turned[2] + tr.stat_cross_turned[3];
        long r = tr.stat_slide_dir[0][0] + tr.stat_slide_dir[0][1]
               + tr.stat_slide_dir[0][2] + tr.stat_slide_dir[0][3];
        long l = tr.stat_slide_dir[1][0] + tr.stat_slide_dir[1][1]
               + tr.stat_slide_dir[1][2] + tr.stat_slide_dir[1][3];
        printf("drive: AT THE CROSSING - %ld went straight, %ld turned "
               "(%ld%% of crossings); of the turns %ld went RIGHT, %ld went "
               "LEFT\n", st, tu, (st + tu) ? tu * 100 / (st + tu) : 0, r, l);
    }
    /* THE RADIUS THE CORNERS ACTUALLY GET, per site, so it can be compared
     * against the same line the game logs on the Amiga. See
     * gta_traffic.stat_aim_r_sum: GTA_TURN_RADIUS is a ceiling, not a radius. */
    if (tr.stat_aim_r_n)
        printf("drive: RADIUS ISSUED - %ld.%ld px of a %d ceiling over %ld "
               "turns, %ld%% reach it; %ld.%ld ticks a corner\n",
               tr.stat_aim_r_sum * 10 / tr.stat_aim_r_n / 10,
               tr.stat_aim_r_sum * 10 / tr.stat_aim_r_n % 10,
               GTA_TURN_RADIUS, tr.stat_aim_r_n,
               tr.stat_aim_r_capped * 100 / tr.stat_aim_r_n,
               tr.stat_turn_ticks_n
                   ? tr.stat_turn_ticks_sum * 10 / tr.stat_turn_ticks_n / 10 : 0,
               tr.stat_turn_ticks_n
                   ? tr.stat_turn_ticks_sum * 10 / tr.stat_turn_ticks_n % 10 : 0);
    /* THE LINE ON THE STREET AGAINST THE LINE ON THE CROSSING - the developer's
     * report, and the one thing every other instrument here is blind to because
     * they all compare a car with itself INSIDE a junction. See
     * gta_traffic.stat_line_street_sum. */
    if (tr.stat_line_street_n && tr.stat_line_cross_n)
        printf("drive: THE LINE - %ld.%ld px off its own lane line on a junction block "
               "(%ld car-ticks), %ld.%ld px on ordinary road (%ld car-ticks)"
               "  [cars going STRAIGHT only]\n",
               tr.stat_line_cross_sum * 10 / tr.stat_line_cross_n / 10,
               tr.stat_line_cross_sum * 10 / tr.stat_line_cross_n % 10,
               tr.stat_line_cross_n,
               tr.stat_line_street_sum * 10 / tr.stat_line_street_n / 10,
               tr.stat_line_street_sum * 10 / tr.stat_line_street_n % 10,
               tr.stat_line_street_n);
    /* ON THE PAVEMENT. See gta_traffic.stat_offroad.
     *
     * This is here because `count_offroad()` above CANNOT SEE IT: that test
     * accepts ground type 2 or 3 - road OR pavement - so a car driving down
     * the footway passes it. The relaxation is right for what it was written
     * for (a 40 px bus in a 32 px lane always has paint over the kerb) and
     * wrong for the fault the developer photographed. Two days of traffic work
     * went past this fault because the acceptance test was built to ignore it. */
    printf("drive: ON THE PAVEMENT - %ld times a car put its MIDDLE on a "
           "non-road block", tr.stat_offroad_events);
    if (tr.stat_offroad_events)
        printf(", %ld car-ticks there, last at (%d,%d)",
               tr.stat_offroad_deep, tr.stat_offroad_x, tr.stat_offroad_y);
    printf("\n       (%ld car-ticks with any CORNER off the road - kerb "
           "overhang included, for A/B only; %ld put back on the road off "
           "screen)\n", tr.stat_offroad, tr.stat_offroad_recovered);
    if (tr.stat_offroad_events)
        printf("       why: mid-turn %ld, on a junction %ld, straight on a "
               "street %ld, no route %ld | lasted: <0.5s %ld, <2s %ld, <8s %ld,"
               " longer %ld\n",
               tr.stat_offroad_why[0], tr.stat_offroad_why[1],
               tr.stat_offroad_why[2], tr.stat_offroad_why[3],
               tr.stat_offroad_len[0], tr.stat_offroad_len[1],
               tr.stat_offroad_len[2], tr.stat_offroad_len[3]);
    printf("drive: ARROWS - %ld turns out of a block that allows that "
           "direction, %ld out of one that does NOT; %ld refused by the rule\n",
           aw_legal, aw_illegal, tr.stat_turn_refused_arrow);
    printf("drive: STOPPED IN THE BOX - %ld car-ticks a car stood still with "
           "its middle on a junction block\n", sib_total);
    printf("drive:   of those %ld were committed to the crossing; held by "
           "queue %ld light %ld box %ld merge %ld dead %ld road %ld gap %ld\n",
           sib_committed, sib_hold[GTA_HOLD_QUEUE], sib_hold[GTA_HOLD_LIGHT],
           sib_hold[GTA_HOLD_BOX], sib_hold[GTA_HOLD_MERGE],
           sib_hold[GTA_HOLD_DEADEND], sib_hold[GTA_HOLD_ROAD],
           sib_hold[GTA_HOLD_GAP]);
    printf("drive: DRIVE-THROUGH - %ld car-ticks a MOVING car was inside a "
           "STOPPED one\n", dt_total);
    printf("drive: LANE DISCIPLINE - %ld turns refused because another lane of "
           "the same carriageway lay on the side being turned towards\n",
           tr.stat_turn_refused_lane);
    printf("drive: LEFT SKIPPED - %ld left turns exchanged for straight-on "
           "because the turn's path was booked\n", tr.stat_left_skipped);
    printf("drive: CONVOY JOINS - %ld bookings shared an existing route "
           "(same exit, same bend)\n", tr.stat_joins);
    {
        extern long gta_join_why[6];
        printf("drive: JOIN CANDIDACIES - ok %ld, corridor %ld, tail-gone %ld, "
               "shape %ld, valve %ld, body %ld\n",
               gta_join_why[0], gta_join_why[1], gta_join_why[2],
               gta_join_why[3], gta_join_why[4], gta_join_why[5]);
    }
    printf("drive: GATE REFUSALS - body-in-line %ld, first-square-owned %ld, "
           "exit-full %ld, no-room-past %ld, booking-refused %ld\n",
           tr.stat_box_why[0], tr.stat_box_why[1], tr.stat_box_why[2],
           tr.stat_box_why[3], tr.stat_box_why[4]);
    printf("drive: LANES - car-ticks on a one-lane carriageway %ld; on a TWO "
           "lane one %ld in the left lane and %ld in the right (%ld%% left); "
           "on a three lane one %ld/%ld/%ld\n",
           lane_w1, lane_w2l, lane_w2r,
           (lane_w2l + lane_w2r) ? (lane_w2l * 100) / (lane_w2l + lane_w2r) : 0,
           lane_w3l, lane_w3m, lane_w3r);
    printf("drive: BOX SHARED - %ld car-ticks with more than one car inside "
           "one crossing; of those %ld had NO claim on the box at all and %ld "
           "a claim held by a car that had left; %ld were turning, %ld were "
           "stopped\n",
           box_multi, box_noowner, box_owner_out, box_turning, box_stopped);
    printf("drive: BLOCKED MOVES - %ld car-ticks a car was held exactly where "
           "it stood because the place it wanted was taken\n",
           tr.stat_blocked_move);
    printf("drive: TURNS REFUSED - %ld arc not clear, %ld already turned in "
           "this crossing, %ld lane lock, %ld no room in the exit lane\n",
           tr.stat_turn_refused_sweep, tr.stat_turn_refused_cross,
           tr.stat_turn_refused_lock, tr.stat_turn_refused_room);
    printf("drive: TURNS MISSED - %ld times a car left the block its route "
           "asked it to turn in, still going straight\n"
           "       (refused by the lane-change lock %ld, by no room in the "
           "exit lane %ld)\n",
           tr.stat_turn_missed, tr.stat_turn_refused_lock,
           tr.stat_turn_refused_room);
    printf("       of those, %ld were turns the ROUTE asked for and %ld were "
           "the arrow-following fallback\n",
           tr.stat_turn_missed_kind[1], tr.stat_turn_missed_kind[0]);
    printf("drive: ROUTE AUDIT - %ld nodes sampled, %ld on a non-road block "
           "(%ld%%), %ld diagonal steps",
           path_nodes, path_bad_ground,
           path_nodes ? path_bad_ground * 100 / path_nodes : 0, path_bad_step);
    if (pathx >= 0)
        printf(", first at (%d,%d) ground %d", pathx, pathy, pathg);
    printf("\n");
    printf("drive: BOX DEADLOCK %ld car-ticks", tr.stat_boxlock);
    if (tr.stat_boxlock)
        printf(", worst %d cars stopped in the crossing at (%d,%d)",
               tr.stat_boxlock_worst, tr.stat_boxlock_x, tr.stat_boxlock_y);
    printf("\n");
    printf("drive: held by - queue %ld, light %ld, junction box %ld, "
           "no room to turn %ld, dead end %ld, road ahead %ld, gap %ld\n",
           hold_hist[1], hold_hist[2], hold_hist[3], hold_hist[4],
           hold_hist[5], hold_hist[6], hold_hist[7]);
    printf("drive: flow - %ld%% of the fleet moving on average, longest a car "
           "stood still %d ticks (%d.%d s)\n",
           moving_ticks ? (moving_sum * 100) / moving_ticks : 0,
           worst_wait, worst_wait / 50, (worst_wait % 50) * 2 / 10);
    /* THE PAVEMENT IS PART OF THE GATE NOW. It was not, and that is exactly
     * why it went unfixed: a test that cannot fail on a fault will never
     * report progress on it either. */
    printf("drive: %s\n",
           (worst == 0 && worst_ov == 0 && vanished == 0 && moved_total > 0 &&
            tr.stat_offroad_events == 0)
           ? "PASSED - on the road, off the pavement, no car inside another, "
             "none vanished in view, and the fleet moved"
           : "*** FAILED ***");

    free(canvas);
    gta_nav_free(&nav);
    gta_render_free(&view);
    gta_tiles_free(&ti);
    gta_map_free(&mp);
    return (worst == 0 && worst_ov == 0 && vanished == 0 && moved_total > 0 &&
            tr.stat_offroad_events == 0)
           ? 0 : 1;
}

/* One frame with parked cars in it. The picture is the test: a car that faces
 * across the street instead of along it, or one standing in a building, is
 * obvious here and is not obvious at 14 pixels in a moving frame. */
/* THE DRIVE TEST, EVERYWHERE - and this exists because the one before it was
 * lying by omission.
 *
 * `drive` runs nine hundred ticks at ONE camera position and prints the
 * average. It said 92 to 98 per cent of the fleet moving while the developer
 * was photographing a dozen vehicles stopped dead across an avenue, and both
 * numbers were true: the game's player walks across the city and the test had
 * only ever looked at two streets. A renderer bug of exactly that shape was
 * caught here by `holesweep.sh`, which runs the hole check at 169 places
 * instead of 8, and traffic needed the same instrument.
 *
 * So this drives at a grid of places across the map, keeps the WORST, and says
 * where it was. A jam that happens in one street is a jam this can find and
 * `drive` cannot.
 */
/* ONE CAR THROUGH ONE CORNER, TICK BY TICK.
 *
 * The developer's report is "cars drive straight fine, then at a junction they
 * leave their lane and come back after it". Every test so far reports
 * percentages; none of them shows WHERE a car is while it turns. This prints
 * it: for the first few turns that happen, the car's world position, heading,
 * and its offset from the centre of the lane it is in - so the claim can be
 * read as a number instead of argued about.
 */
static int cmd_turntrace(const char *mapPath, const char *tilesPath,
                         int bx, int by, int ticks, unsigned long seed,
                         int want_turns)
{
    gta_map mp;
    gta_tiles ti;
    gta_traffic tr;
    gta_nav nav;
    int t, i, n0, turns = 0;
    int tracking = -1, after = 0;
    int prev_turn[GTA_MAX_CARS];

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) { gta_map_free(&mp); return 1; }

    gta_traffic_init(&tr, &ti, seed);
    if (gta_nav_build(&nav, &mp) == 0)
        gta_traffic_set_nav(&tr, &nav);
    tr.diag_corners = 1;
    gta_traffic_set_view_blocks(&tr, 6);
    n0 = gta_traffic_park(&tr, &mp, bx, by, 8, GTA_MAX_CARS);
    printf("turntrace: %d cars around (%d,%d), seed %lu\n", n0, bx, by, seed);
    for (i = 0; i < GTA_MAX_CARS; i++) prev_turn[i] = 0;

    for (t = 0; t < ticks && turns <= want_turns; t++) {
        gta_traffic_tick(&tr, &mp,
                         ((long)bx * 32 + 16) << 16, ((long)by * 32 + 16) << 16);

        if (tracking < 0) {
            for (i = 0; i < tr.n; i++) {
                if (tr.cars[i].turn != 0 && prev_turn[i] == 0) {
                    tracking = i;
                    after = 0;
                    turns++;
                    printf("\n--- turn %d: car %d (%s len %d) at t=%d, "
                           "from %d towards %s ---\n"
                           "   t     x      y  face ang turn spd   lat  blk\n",
                           turns, i,
                           gta_vehicle_class_name(ti.cars[tr.cars[i].model].vtype),
                           gta_car_world_len(&ti.cars[tr.cars[i].model]),
                           t, tr.cars[i].turn_from,
                           tr.cars[i].turn > 0 ? "right" : "left");
                    break;
                }
            }
        }
        for (i = 0; i < tr.n; i++) prev_turn[i] = tr.cars[i].turn;

        if (tracking >= 0) {
            const gta_car *c = &tr.cars[tracking];
            long wx = c->x >> 16, wy = c->y >> 16;
            int ew = ((c->angle & 127) == 64);
            long lat = ew ? (wy & 31) - 16 : (wx & 31) - 16;   /* from lane centre */
            int on_route = (c->path_i < c->path_n);
            printf("%4d %5ld %6ld   %3d %3d  %+d %4ld  %+4ld  (%ld,%ld)  %s %d/%d\n",
                   t, wx, wy, c->face, c->angle, c->turn,
                   c->speed >> 16, lat, wx >> 5, wy >> 5,
                   on_route ? "ROUTE" : "fall", c->path_i, c->path_n);
            if (c->turn == 0) {
                if (++after >= 24) { tracking = -1; }
            }
        }
    }

    gta_nav_free(&nav);
    gta_tiles_free(&ti);
    gta_map_free(&mp);
    return 0;
}

/* ONE SITE, EVERY U-TURN PRINTED. The counter in `drivesweep` says how many
 * there are; this says what each one WAS - which car, which block, whether it
 * was inside a junction, and whether it was following a route at the time.
 * That last field is the diagnosis: a car that reverses WITH a route is a
 * route-finder fault, and one that reverses WITHOUT is the arrow-following
 * fallback re-deciding its heading in the middle of a crossing. */
static int cmd_uturns(const char *mapPath, const char *tilesPath,
                      int bx, int by, int ticks, unsigned long seed)
{
    gta_map mp;
    gta_tiles ti;
    gta_nav nav;
    gta_traffic tr;
    ut_state ut;
    int t, have_nav, n0;

    if (ticks <= 0) ticks = 3000;
    if (gta_map_load(mapPath, &mp) != 0) return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) { gta_map_free(&mp); return 1; }
    have_nav = (gta_nav_build(&nav, &mp) == 0);

    gta_traffic_init(&tr, &ti, seed);
    if (have_nav) gta_traffic_set_nav(&tr, &nav);
    tr.diag_corners = 1;
    gta_traffic_set_view_blocks(&tr, 6);
    n0 = gta_traffic_park(&tr, &mp, bx, by, 8, GTA_MAX_CARS);
    printf("uturns: %d cars around (%d,%d), %d ticks, seed %lu\n",
           n0, bx, by, ticks, seed);

    ut_init(&ut);
    ut.verbose = 1;
    for (t = 0; t < ticks; t++) {
        ut.tick = t;
        gta_traffic_tick(&tr, &mp, ((long)bx * 32 + 16) << 16,
                         ((long)by * 32 + 16) << 16);
        ut_tick(&ut, &tr, &mp);
    }
    printf("uturns: %d gradual + %d instant, %d of them on a junction block\n",
           ut.uturns, ut.flips, ut.at_junction);
    printf("uturns: routes %ld ok / %ld failed, %ld of the good ones started "
           "BACKWARDS (%ld issued mid-turn)\n",
           tr.routes_ok, tr.routes_failed, tr.routes_backward,
           tr.routes_while_turning);

    if (have_nav) gta_nav_free(&nav);
    gta_tiles_free(&ti);
    gta_map_free(&mp);
    return 0;
}

static int cmd_drivesweep(const char *mapPath, const char *tilesPath,
                          int ticks, int step, unsigned long seed)
{
    gta_map mp;
    gta_tiles ti;
    gta_nav nav;
    int bx, by, sites = 0, have_nav;
    int worst_flow = 101, worst_x = 0, worst_y = 0;
    int worst_wait_all = 0, wwx = 0, wwy = 0;
    int worst_ov = 0, ovx = 0, ovy = 0;
    int worst_off = 0, offx = 0, offy = 0;
    int worst_ut = 0, utx = 0, uty = 0;
    long flow_sum = 0, uturn_sum = 0, uturn_junc = 0, frozen_sum = 0;
    long lanefix_sum = 0, corner_sum = 0, noroute_sum = 0;
    long boxlock_sum = 0; int boxlock_worst = 0, blx = 0, bly = 0;
    long slide_sum[4] = { 0, 0, 0, 0 };
    long slide_px_sum = 0;
    long slide_dir_sum[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    long land_n_dir[2] = { 0, 0 }, land_geom_dir[2] = { 0, 0 };
    long land_aim_dir[2] = { 0, 0 };
    long settled_sum[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    long aim_bias[2] = { 0, 0 }, aim_bias_n[2] = { 0, 0 };
    long aim_sign[2][2] = { { 0, 0 }, { 0, 0 } };
    long geom_step[5] = { 0, 0, 0, 0, 0 }, geom_step_n[5] = { 0, 0, 0, 0, 0 };
    long turn_ticks_b[5] = { 0, 0, 0, 0, 0 }, turn_ticks_n = 0, turn_ticks_sum = 0;
    long route_fail[GTA_ROUTE_FAIL_KINDS] = { 0, 0, 0, 0, 0, 0, 0 };
    long aim_r_sum = 0, aim_r_n = 0, aim_r_capped = 0;
    int  nostart_x = -1, nostart_y = -1, nostart_z = -1;
    long drop_uturn_sum = 0, drop_fits_sum = 0, drop_stray_sum = 0, drop_nodes_sum = 0;
    long routes_ok_sum = 0, routes_bad_sum = 0, nodes_sum = 0;
    long cr_sum = 0, cf_sum = 0, tr_sum = 0, tf_sum = 0;
    long land_sum = 0, geom_sum = 0, aim_sum = 0;
    long cs_str[4] = {0,0,0,0}, cs_trn[4] = {0,0,0,0}, cs_v = 0, cs_vn = 0;
    long lc_steer = 0, lc_route = 0;
    /* THE TWO FAULTS THE DEVELOPER REPORTED FROM THE GAME, city-wide. See
     * gta_traffic.stat_turn_missed and gta_traffic.stat_offroad: a missed turn
     * is what puts a car on the footway, and a car on the footway is what
     * later vanishes. Neither had a number before 2026-08-23. */
    long miss_sum = 0, miss_lock = 0, miss_room = 0;
    long pave_ev = 0, pave_tk = 0;
    int  worst_miss = 0, missx = 0, missy = 0;
    int  worst_pave = 0, pavex = 0, pavey = 0;

    /* THREE THOUSAND TICKS, NOT SIX HUNDRED, and the default matters here.
     *
     * A gridlock ring takes a minute of game time to form. Every traffic
     * measurement in this project up to 2026-08-22 was taken over 600 or 900
     * ticks - twelve to eighteen seconds - which is before the thing being
     * measured has happened, and it made a rule that fixes gridlock look like
     * a rule that does nothing. The developer found it by WAITING at the
     * emulator: "in 30-120 secs traffic stops due to conflict on the
     * crossroads". A minute is 3000 ticks; this is that, with room. */
    if (ticks <= 0) ticks = 3000;
    if (step  <= 0) step  = 24;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) { gta_map_free(&mp); return 1; }
    have_nav = (gta_nav_build(&nav, &mp) == 0);
    if (!have_nav)
        printf("drivesweep: no memory for the navigation grid - no routes\n");

    printf("drivesweep: %d ticks a site, every %d blocks, seed %lu\n",
           ticks, step, seed);

    for (by = step / 2; by < GTA_MAP_DIM; by += step)
    for (bx = step / 2; bx < GTA_MAP_DIM; bx += step) {
        gta_traffic tr;
        ut_state ut;
        long moving_sum = 0, moving_ticks = 0, moved = 0;
        int t, i, n0, flow, wait_worst = 0, ov = 0, off = 0;
        int stopped_for[GTA_MAX_CARS];
        long px[GTA_MAX_CARS], py[GTA_MAX_CARS];

        gta_traffic_init(&tr, &ti, seed);
        if (have_nav) gta_traffic_set_nav(&tr, &nav);
        tr.diag_corners = 1;
    gta_traffic_set_view_blocks(&tr, 6);
        n0 = gta_traffic_park(&tr, &mp, bx, by, 8, GTA_MAX_CARS);
        /* Fewer than a handful of cars means there is no road here worth
         * testing - the middle of the water, or a park. Not a fault. */
        if (n0 < 5) continue;
        sites++;

        for (i = 0; i < GTA_MAX_CARS; i++) stopped_for[i] = 0;
        ut_init(&ut);

        for (t = 0; t < ticks; t++) {
            int moving = 0;
            for (i = 0; i < tr.n; i++) { px[i] = tr.cars[i].x; py[i] = tr.cars[i].y; }
            gta_traffic_tick(&tr, &mp,
                             ((long)bx * 32 + 16) << 16,
                             ((long)by * 32 + 16) << 16);
            ut_tick(&ut, &tr, &mp);
            for (i = 0; i < tr.n; i++) {
                long dx = tr.cars[i].x - px[i], dy = tr.cars[i].y - py[i];
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                moved += (dx + dy) >> 16;
                if (tr.cars[i].speed > 0) {
                    moving++;
                    stopped_for[i] = 0;
                } else {
                    stopped_for[i]++;
                    if (stopped_for[i] > wait_worst) wait_worst = stopped_for[i];
                }
            }
            moving_sum += moving;
            moving_ticks += tr.n;
            if ((t & 63) == 0) {
                int o = count_overlaps(&ti, &tr, 0, t);
                int f = count_offroad(&mp, &ti, &tr, 0);
                if (o > ov) ov = o;
                if (f > off) off = f;
            }
        }
        flow = moving_ticks ? (int)((moving_sum * 100) / moving_ticks) : 0;
        flow_sum += flow;

        printf("  (%3d,%3d) %2d cars  flow %3d%%  stood %4d  overlaps %d  "
               "off-road %d  %ld blocks  u-turn %d+%d  lane-fix %ld\n",
               bx, by, n0, flow, wait_worst, ov, off, moved / 32,
               ut.uturns, ut.flips, tr.stat_lane_fix_corner);
        miss_sum  += tr.stat_turn_missed;
        miss_lock += tr.stat_turn_refused_lock;
        miss_room += tr.stat_turn_refused_room;
        pave_ev   += tr.stat_offroad_events;
        pave_tk   += tr.stat_offroad_deep;
        if (tr.stat_turn_missed > worst_miss) {
            worst_miss = (int)tr.stat_turn_missed; missx = bx; missy = by;
        }
        if (tr.stat_offroad_events > worst_pave) {
            worst_pave = (int)tr.stat_offroad_events; pavex = bx; pavey = by;
        }
        frozen_sum += tr.stat_frozen_turn;
        noroute_sum += tr.stat_no_route;
        drop_uturn_sum += tr.stat_drop_uturn;
        drop_fits_sum  += tr.stat_drop_fits;
        drop_stray_sum += tr.stat_drop_stray;
        drop_nodes_sum += tr.stat_drop_nodes;
        routes_ok_sum  += tr.routes_ok;
        routes_bad_sum += tr.routes_failed;
        nodes_sum      += tr.route_nodes;
        cr_sum += tr.stat_corner_routed;
        cf_sum += tr.stat_corner_fallback;
        tr_sum += tr.stat_turns_routed;
        tf_sum += tr.stat_turns_fallback;
        {
            int q;
            for (q = 0; q < 4; q++) {
                cs_str[q] += tr.stat_cross_straight[q];
                cs_trn[q] += tr.stat_cross_turned[q];
            }
        }
        cs_v  += tr.stat_cross_virt;
        cs_vn += tr.stat_cross_virt_n;
        lc_steer += tr.stat_lane_change_steered;
        lc_route += tr.stat_lane_change_routed;
        land_sum += tr.stat_landings;
        geom_sum += tr.stat_land_geom;
        aim_sum  += tr.stat_land_aim;
        lanefix_sum += tr.stat_lane_fix;
        boxlock_sum += tr.stat_boxlock;
        { int q; for (q = 0; q < 4; q++) slide_sum[q] += tr.stat_slide[q]; }
        slide_px_sum += tr.stat_slide_px;
        { int q; for (q = 0; q < 4; q++) { slide_dir_sum[0][q] += tr.stat_slide_dir[0][q]; slide_dir_sum[1][q] += tr.stat_slide_dir[1][q]; }
          for (q = 0; q < 2; q++) { land_n_dir[q] += tr.stat_land_n_dir[q]; land_geom_dir[q] += tr.stat_land_geom_dir[q]; land_aim_dir[q] += tr.stat_land_aim_dir[q]; { int r; for (r = 0; r < 4; r++) settled_sum[q][r] += tr.stat_settled[q][r]; }
            aim_bias[q] += tr.stat_aim_bias[q]; aim_bias_n[q] += tr.stat_aim_bias_n[q];
            aim_sign[q][0] += tr.stat_aim_sign[q][0]; aim_sign[q][1] += tr.stat_aim_sign[q][1]; } }
        { int q; for (q = 0; q < 5; q++) { geom_step[q] += tr.stat_geom_by_step[q]; geom_step_n[q] += tr.stat_geom_by_step_n[q]; turn_ticks_b[q] += tr.stat_turn_ticks[q]; } }
        turn_ticks_n += tr.stat_turn_ticks_n; turn_ticks_sum += tr.stat_turn_ticks_sum;
        aim_r_sum += tr.stat_aim_r_sum; aim_r_n += tr.stat_aim_r_n;
        aim_r_capped += tr.stat_aim_r_capped;
        { int q; for (q = 0; q < GTA_ROUTE_FAIL_KINDS; q++) route_fail[q] += tr.stat_route_fail[q]; }
        if (tr.stat_route_fail[GTA_ROUTE_NO_START]) {
            nostart_x = tr.nostart_x; nostart_y = tr.nostart_y; nostart_z = tr.nostart_z;
        }
        if (tr.stat_boxlock_worst > boxlock_worst) {
            boxlock_worst = tr.stat_boxlock_worst;
            blx = tr.stat_boxlock_x; bly = tr.stat_boxlock_y;
        }
        corner_sum  += tr.stat_lane_fix_corner;
        fflush(stdout);
        uturn_sum  += ut.uturns + ut.flips;
        uturn_junc += ut.at_junction;
        if (ut.uturns + ut.flips > worst_ut) {
            worst_ut = ut.uturns + ut.flips; utx = bx; uty = by;
        }

        if (flow < worst_flow)      { worst_flow = flow; worst_x = bx; worst_y = by; }
        if (wait_worst > worst_wait_all) { worst_wait_all = wait_worst; wwx = bx; wwy = by; }
        if (ov > worst_ov)          { worst_ov = ov; ovx = bx; ovy = by; }
        if (off > worst_off)        { worst_off = off; offx = bx; offy = by; }
    }

    printf("drivesweep: %d sites\n", sites);
    if (sites > 0) {
        printf("drivesweep: flow - %ld%% on average, WORST %d%% at (%d,%d)\n",
               flow_sum / sites, worst_flow, worst_x, worst_y);
        printf("drivesweep: longest anyone stood still %d ticks (%d.%d s) "
               "at (%d,%d)\n", worst_wait_all, worst_wait_all / 50,
               (worst_wait_all % 50) * 2 / 10, wwx, wwy);
        printf("drivesweep: worst overlaps %d at (%d,%d), worst off-road %d "
               "at (%d,%d)\n", worst_ov, ovx, ovy, worst_off, offx, offy);
        /* The deadlock the developer photographed, summed over the city. Any
         * non-zero figure is cars standing inside a crossing blocking each
         * other; see gta_traffic.stat_boxlock. */
        {
            long ns = slide_sum[0] + slide_sum[1] + slide_sum[2] + slide_sum[3];
            printf("drivesweep: SLIDE AFTER A CORNER over %ld corners - "
                   "0-1 px %ld%%, 2-3 px %ld%%, 4-7 px %ld%%, 8+ px %ld%% "
                   "(%ld px each)\n"
                   "            (how far a car moves SIDEWAYS in the %d ticks "
                   "after its arc ends - the reported fault, as a distance)\n",
                   ns,
                   ns ? slide_sum[0] * 100 / ns : 0,
                   ns ? slide_sum[1] * 100 / ns : 0,
                   ns ? slide_sum[2] * 100 / ns : 0,
                   ns ? slide_sum[3] * 100 / ns : 0,
                   ns ? slide_px_sum / ns : 0, GTA_AFTER_TURN);
            /* WHICH HALF OF THE LANDING ERROR CARRIES THE LEFT/RIGHT
             * DIFFERENCE, and whether the turn was even aimed at the block the
             * car finished in. See gta_traffic.stat_aim_block_wrong. */
            {
                int w;
                printf("            LANDING split by direction:\n");
                for (w = 0; w < 2; w++) {
                    long n = land_n_dir[w];
                    printf("              %-5s %6ld corners - geometry %ld.%ld px"
                           ", aim %ld.%ld px, SETTLED on the line %ld%%\n",
                           w ? "LEFT" : "RIGHT", n,
                           n ? land_geom_dir[w] * 10 / n / 10 : 0,
                           n ? land_geom_dir[w] * 10 / n % 10 : 0,
                           n ? land_aim_dir[w] * 10 / n / 10 : 0,
                           n ? land_aim_dir[w] * 10 / n % 10 : 0,
                           n ? settled_sum[w][0] * 100 / (settled_sum[w][0] + settled_sum[w][1] + settled_sum[w][2] + settled_sum[w][3] + 1) : 0);
                }
                /* HOW LONG A CORNER TAKES. The developer, playing the DOS
                 * original beside the port, says ours snap round where the
                 * original is gradual. See gta_traffic.stat_turn_ticks. */
                {
                    static const char *lab[5] = { "1-4", "5-8", "9-16",
                                                  "17-32", "33+" };
                    long n = turn_ticks_n;
                    printf("            HOW LONG A CORNER TAKES over %ld turns"
                           " - %ld.%ld ticks on average (%ld.%02ld s):\n", n,
                           n ? turn_ticks_sum * 10 / n / 10 : 0,
                           n ? turn_ticks_sum * 10 / n % 10 : 0,
                           n ? turn_ticks_sum / n / 50 : 0,
                           n ? turn_ticks_sum * 2 / n % 100 : 0);
                    for (w = 0; w < 5; w++)
                        if (turn_ticks_b[w])
                            printf("              %-6s ticks %7ld turns  %ld%%\n",
                                   lab[w], turn_ticks_b[w],
                                   n ? turn_ticks_b[w] * 100 / n : 0);
                }
                /* THE RADIUS ACTUALLY ISSUED against the one asked for - see
                 * gta_traffic.stat_aim_r_sum. GTA_TURN_RADIUS is a ceiling,
                 * and if hardly any turn reaches it then raising it further is
                 * pointless: the car is not being given enough approach. */
                if (aim_r_n)
                    printf("            RADIUS ISSUED - %ld.%ld px on average "
                           "against a ceiling of %d, and %ld%% of turns reach "
                           "the ceiling\n",
                           aim_r_sum * 10 / aim_r_n / 10,
                           aim_r_sum * 10 / aim_r_n % 10, GTA_TURN_RADIUS,
                           aim_r_capped * 100 / aim_r_n);
                /* AND THE GEOMETRY ERROR AGAINST THE LENGTH OF ONE STEP, which
                 * is what separates a trigger fault from an integration one.
                 * See gta_traffic.stat_geom_by_step: rising with the step means
                 * `ready` fires up to a step early, flat means the arc itself. */
                printf("            GEOMETRY error by step length at the tick "
                       "the turn was issued:\n");
                for (w = 0; w < 5; w++) {
                    long n = geom_step_n[w];
                    if (!n) continue;
                    printf("              %d.%d px/tick %7ld corners - %ld.%ld px "
                           "of geometry error\n",
                           w / 2, (w & 1) * 5, n, geom_step[w] * 10 / n / 10,
                           geom_step[w] * 10 / n % 10);
                }
                /* AND THE SIGN OF THE AIM ERROR, which is what separates a
                 * systematic offset from noise. See gta_traffic.stat_aim_bias. */
                printf("            AIM vs the lane the car SETTLES in (signed):\n");
                for (w = 0; w < 2; w++) {
                    long n = aim_bias_n[w];
                    long lo = aim_sign[w][0], hi = aim_sign[w][1];
                    printf("              %-5s %6ld corners - mean %s%ld.%ld px"
                           "  (aimed low %ld%%, dead on %ld%%, aimed high %ld%%)\n",
                           w ? "LEFT" : "RIGHT", n,
                           (aim_bias[w] < 0) ? "-" : "+",
                           n ? (aim_bias[w] < 0 ? -aim_bias[w] : aim_bias[w]) * 10 / n / 10 : 0,
                           n ? (aim_bias[w] < 0 ? -aim_bias[w] : aim_bias[w]) * 10 / n % 10 : 0,
                           n ? lo * 100 / n : 0,
                           n ? (n - lo - hi) * 100 / n : 0,
                           n ? hi * 100 / n : 0);
                }
            }
            /* AND SPLIT BY WHICH WAY THE CAR TURNED, because the fault is
             * reported as one-sided and a total cannot answer that. */
            {
                int w;
                for (w = 0; w < 2; w++) {
                    long t = slide_dir_sum[w][0] + slide_dir_sum[w][1]
                           + slide_dir_sum[w][2] + slide_dir_sum[w][3];
                    printf("            turning %-5s %6ld corners - "
                           "on the line %ld%%, slid 2+ px %ld%%, 8+ px %ld%%\n",
                           w ? "LEFT" : "RIGHT", t,
                           t ? slide_dir_sum[w][0] * 100 / t : 0,
                           t ? (t - slide_dir_sum[w][0]) * 100 / t : 0,
                           t ? slide_dir_sum[w][3] * 100 / t : 0);
                }
            }
        }
        /* WHY the searches failed, not just how many. See gta_route.h. */
        {
            static const char *why[GTA_ROUTE_FAIL_KINDS] = {
                "found one", "car not on a road block", "target not a road block",
                "target outside the window", "target is this block",
                "ran out of budget", "explored all it could reach"
            };
            int w;
            printf("drivesweep: WHY SEARCHES FAIL -");
            for (w = 1; w < GTA_ROUTE_FAIL_KINDS; w++)
                if (route_fail[w])
                    printf(" %s %ld;", why[w], route_fail[w]);
            printf("\n");
            if (nostart_x >= 0)
                printf("            last car with no exits under it: "
                       "(%d,%d) layer %d\n",
                       nostart_x, nostart_y, nostart_z);
        }
        /* THE HEADLINE PAIR. Everything else in this report is a consequence:
     * a car that misses its turn leaves the road, and a car that is off the
     * road is the one that later vanishes. */
    printf("\n=== THE TURN THAT DID NOT HAPPEN ===\n");
    printf("  %ld times a car left the block its route asked it to turn in, "
           "still going straight\n", miss_sum);
    printf("  refused %ld tick-times by the lane-change lock, %ld by no room "
           "in the exit lane\n", miss_lock, miss_room);
    if (worst_miss)
        printf("  worst site (%d,%d) with %d\n", missx, missy, worst_miss);
    printf("=== AND WHERE THAT PUTS THEM ===\n");
    printf("  %ld times a car put its MIDDLE on a non-road block, %ld "
           "car-ticks spent there\n", pave_ev, pave_tk);
    if (worst_pave)
        printf("  worst site (%d,%d) with %d\n", pavex, pavey, worst_pave);
    printf("\n");
    printf("drivesweep: BOX DEADLOCK %ld car-ticks in all", boxlock_sum);
        if (boxlock_sum)
            printf(", worst %d cars in one crossing at (%d,%d)",
                   boxlock_worst, blx, bly);
        printf("\n");
        printf("drivesweep: lane correction %ld px in all, %ld px of it within "
               "%d ticks of a corner\n"
               "            (the second number is the reported fault; the "
               "rest is ordinary lane changing)\n",
               lanefix_sum, corner_sum, GTA_AFTER_TURN);
        printf("drivesweep: car-ticks stopped mid-turn: %ld, car-ticks with no "
               "route: %ld\n", frozen_sum, noroute_sum);
        printf("drivesweep: routes thrown away - %ld U-turn, %ld will not fit, "
               "%ld car-ticks strayed off a live route; %ld nodes discarded\n",
               drop_uturn_sum, drop_fits_sum, drop_stray_sum, drop_nodes_sum);
        printf("drivesweep: searches %ld found / %ld failed (%ld%% failed), "
               "%ld nodes each on average\n",
               routes_ok_sum, routes_bad_sum,
               (routes_ok_sum + routes_bad_sum)
                 ? routes_bad_sum * 100 / (routes_ok_sum + routes_bad_sum) : 0,
               routes_ok_sum ? nodes_sum / routes_ok_sum : 0);
        printf("drivesweep: corners taken ON A ROUTE %ld (%ld px of correction, "
               "%ld px each)\n"
               "            corners taken on the FALLBACK %ld (%ld px, "
               "%ld px each)\n",
               tr_sum, cr_sum, tr_sum ? cr_sum / tr_sum : 0,
               tf_sum, cf_sum, tf_sum ? cf_sum / tf_sum : 0);
        {
            long st = cs_str[0] + cs_str[1] + cs_str[2] + cs_str[3];
            long tu = cs_trn[0] + cs_trn[1] + cs_trn[2] + cs_trn[3];
            printf("\n=== DID THE CAR LEAVE THE JUNCTION ON THE LINE IT CAME "
                   "IN ON? ===\n");
            printf("STRAIGHT THROUGH  %ld crossings\n", st);
            if (st) printf("   same line (0-1 px) %ld = %ld%%\n"
                           "   2-3 px            %ld = %ld%%\n"
                           "   4-7 px            %ld = %ld%%\n"
                           "   8+ px (LANE CHANGE) %ld = %ld%%\n"
                           "   worst wander while INSIDE the crossing: "
                           "%ld px on average\n",
                           cs_str[0], cs_str[0] * 100 / st,
                           cs_str[1], cs_str[1] * 100 / st,
                           cs_str[2], cs_str[2] * 100 / st,
                           cs_str[3], cs_str[3] * 100 / st,
                           cs_vn ? cs_v / cs_vn : 0);
            printf("TURNED            %ld crossings   (distance from the "
                   "centre of the lane joined)\n", tu);
            if (tu) printf("   on the line (0-1 px) %ld = %ld%%\n"
                           "   2-3 px             %ld = %ld%%\n"
                           "   4-7 px             %ld = %ld%%\n"
                           "   8+ px              %ld = %ld%%\n",
                           cs_trn[0], cs_trn[0] * 100 / tu,
                           cs_trn[1], cs_trn[1] * 100 / tu,
                           cs_trn[2], cs_trn[2] * 100 / tu,
                           cs_trn[3], cs_trn[3] * 100 / tu);
            printf("   of those lane changes: %ld steered across, "
                   "%ld were following a route\n", lc_steer, lc_route);
            if (st + tu)
                printf("ALL CROSSINGS: %ld%% leave on the correct line "
                       "(within 1 px), %ld%% do not\n\n",
                       (cs_str[0] + cs_trn[0]) * 100 / (st + tu),
                       (st + tu - cs_str[0] - cs_trn[0]) * 100 / (st + tu));
        }
        printf("drivesweep: LANDING ERROR over %ld corners - %ld px missed the "
               "aim (geometry), %ld px the aim was wrong\n"
               "            = %ld.%ld px and %ld.%ld px per corner\n",
               land_sum, geom_sum, aim_sum,
               land_sum ? geom_sum / land_sum : 0,
               land_sum ? (geom_sum * 10 / land_sum) % 10 : 0,
               land_sum ? aim_sum / land_sum : 0,
               land_sum ? (aim_sum * 10 / land_sum) % 10 : 0);
        printf("drivesweep: U-TURNS %ld in all (%ld on a junction block), "
               "worst site %d at (%d,%d)\n",
               uturn_sum, uturn_junc, worst_ut, utx, uty);
        printf("drivesweep: re-run the worst one with\n"
               "    gtadump drive <map> <til> %d %d out/w %d 0 %lu\n",
               worst_x, worst_y, ticks, seed);
    }

    if (have_nav) gta_nav_free(&nav);
    gta_tiles_free(&ti);
    gta_map_free(&mp);
    /* The gate is the same as `drive`'s, applied to the worst site rather than
     * to the only one: nobody inside anybody, nobody in a building, and no
     * street where the traffic has stopped. 50% is deliberately generous -
     * this is meant to catch a JAM, not to argue about a queue. */
    return (worst_ov == 0 && worst_off == 0 && worst_flow >= 50) ? 0 : 1;
}

static int cmd_traffic(const char *mapPath, const char *tilesPath,
                       int bx, int by, const char *out, int zoom_px,
                       unsigned long seed)
{
    gta_map mp;
    gta_tiles ti;
    gta_view view;
    gta_traffic tr;
    unsigned char *canvas;
    const int W = 320, H = 200;
    int placed, i;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) {
        gta_map_free(&mp);
        return 1;
    }
    canvas = (unsigned char *)calloc((size_t)W * H, 1);
    if (!canvas) { gta_tiles_free(&ti); gta_map_free(&mp); return 1; }

    gta_render_init(&view, &mp, &ti);
    if (zoom_px > 0)
        gta_render_set_zoom(&view, zoom_px);
    gta_render_target(&view, canvas, W, H, W);
    gta_render_look_at_block(&view, bx, by);

    gta_traffic_init(&tr, &ti, seed);
    placed = gta_traffic_park(&tr, &mp, bx, by, 8, GTA_MAX_CARS);
    printf("traffic: %d cars parked around (%d,%d), zoom %d\n",
           placed, bx, by, view.zoom_px);
    for (i = 0; i < tr.n; i++) {
        const gta_car_info *ci = &ti.cars[tr.cars[i].model];
        printf("  car %2d: block (%3ld,%3ld) layer %d angle %3d  %-5s "
               "model %2d sprite %d remap %d\n",
               i, tr.cars[i].x >> (16 + 5), tr.cars[i].y >> (16 + 5),
               tr.cars[i].layer, tr.cars[i].angle,
               gta_vehicle_class_name(ci->vtype),
               tr.cars[i].model, ci->sprite_index, tr.cars[i].remap);
    }

    /* THE PLACEMENT TEST, and it is a count rather than a look.
     *
     * "Is that bus sticking into a building?" cannot be answered honestly from
     * a 320x200 frame - the first Amiga run of this had two fire trucks lying
     * across a pavement and it took a magnified crop to be sure. So every
     * placed car is re-checked here against the map: every block its LENGTH
     * covers must be road. Anything else is printed and counted, and the
     * command's exit status says so. */
    {
        int off = count_offroad(&mp, &ti, &tr, 1);
        printf("  %d of %d cars off the road\n", off, tr.n);
        if (off) {
            free(canvas);
            gta_render_free(&view);
            gta_tiles_free(&ti);
            gta_map_free(&mp);
            return 1;
        }
    }

    gta_traffic_draw(&tr, &view);
    gta_render_frame(&view);
    printf("  %ld sprites drawn\n", view.sprites_drawn);

    if (write_bmp8(out, canvas, W, H, ti.palette) == 0)
        printf("  wrote %s\n", out);

    free(canvas);
    gta_render_free(&view);
    gta_tiles_free(&ti);
    gta_map_free(&mp);
    return 0;
}

/* THE ROUND-TRIP PROOF for the car table.
 *
 * Reads the definitions out of the .GRY and out of the baked .til and compares
 * every field of every record. The two paths share no code below gta_car.h:
 * one is a little-endian walk of a variable-length section, the other a
 * big-endian read of a fixed-size one. If they agree on all 38 records, the
 * pack and the unpack agree, which is the only thing that can be checked
 * without a machine to run them on.
 *
 * This is deliberately a COMPARISON and not a "does it look right" dump. The
 * table already prints plausibly when a field is wrong. */
static int cmd_tilecars(const char *stylePath, const char *tilPath)
{
    gta_style st;
    gta_tiles ti;
    int i, bad = 0;

    if (gta_style_load(stylePath, &st) != 0)
        return 1;
    if (gta_tiles_load(tilPath, &ti) != 0) {
        gta_style_free(&st);
        return 1;
    }

    printf("%s: %d cars     %s: %d cars\n",
           stylePath, st.car_count, tilPath, ti.n_cars);
    if (st.car_count != ti.n_cars) {
        printf("*** the counts DISAGREE ***\n");
        bad = 1;
    }

    for (i = 0; i < st.car_count && i < ti.n_cars; i++) {
        const gta_car_info *a = &st.cars[i];
        const gta_car_info *b = &ti.cars[i];
        int d = 0, j;

#define DIFF(field) \
        do { if (a->field != b->field) { \
                 printf("car %d: %-22s .gry %ld  .til %ld\n", i, #field, \
                        (long)a->field, (long)b->field); d = 1; } } while (0)
        DIFF(width); DIFF(length); DIFF(vert);
        DIFF(sprite_num); DIFF(weight);
        DIFF(max_speed); DIFF(min_speed); DIFF(accel); DIFF(braking);
        DIFF(grip); DIFF(handling);
        DIFF(vtype); DIFF(model_id); DIFF(turning); DIFF(damagable);
        DIFF(cx); DIFF(cy); DIFF(moment);
        DIFF(mass); DIFF(thrust);
        DIFF(tyre_adhesion_x); DIFF(tyre_adhesion_y);
        DIFF(handbrake_friction); DIFF(footbrake_friction);
        DIFF(front_brake_bias);
        DIFF(turn_ratio); DIFF(drive_wheel_offset); DIFF(steering_wheel_offset);
        DIFF(back_end_slide); DIFF(handbrake_slide);
        DIFF(convertible); DIFF(engine); DIFF(radio); DIFF(horn);
        DIFF(sound_function); DIFF(fast_change);
        DIFF(n_doors); DIFF(sprite_index);
        for (j = 0; j < 4; j++)
            if (a->value[j] != b->value[j]) {
                printf("car %d: value[%d] %d vs %d\n", i, j,
                       a->value[j], b->value[j]);
                d = 1;
            }
        for (j = 0; j < GTA_CAR_REMAPS; j++)
            if (a->remap8[j] != b->remap8[j]) {
                printf("car %d: remap8[%d] %02x vs %02x\n", i, j,
                       a->remap8[j], b->remap8[j]);
                d = 1;
            }
        for (j = 0; j < a->n_doors && j < GTA_CAR_DOORS; j++)
            if (a->doors[j].rpx != b->doors[j].rpx ||
                a->doors[j].rpy != b->doors[j].rpy ||
                a->doors[j].object != b->doors[j].object ||
                a->doors[j].delta != b->doors[j].delta) {
                printf("car %d: door %d differs\n", i, j);
                d = 1;
            }
#undef DIFF
        if (d) bad = 1;
    }

    /* The sprite each definition points at must exist in the BAKED file, and
     * its size has to look like the vehicle's - that is the check that the
     * absolute index survived baking, not just that the number did. */
    for (i = 0; i < ti.n_cars; i++) {
        int si = ti.cars[i].sprite_index;
        if (si < 0) {
            printf("car %d (%s): no sprite\n", i,
                   gta_vehicle_class_name(ti.cars[i].vtype));
            continue;
        }
        if (si >= ti.n_sprites) {
            printf("car %d: sprite %d is past the end (%d)\n", i, si,
                   ti.n_sprites);
            bad = 1;
            continue;
        }
        /* The collision length should be close to the sprite's height - that
         * is what identified the field in the first place - but it is NOT
         * required to match: a collision box is deliberately a little smaller
         * than the artwork, and some vehicles carry a trailer in the sprite.
         * So this is advisory, and only a gross mismatch is worth a line. */
        {
            int d = (int)ti.sprites[si].h - ti.cars[i].length;
            if (d < 0) d = -d;
            if (d > 16)
                printf("note: car %2d (%-10s) length %3d vs sprite %d at "
                       "%dx%d - %d apart\n",
                       i, gta_vehicle_class_name(ti.cars[i].vtype),
                       ti.cars[i].length, si,
                       ti.sprites[si].w, ti.sprites[si].h, d);
        }
    }

    printf("\n%s\n", bad ? "*** ROUND TRIP FAILED ***"
                         : "round trip clean: every field of every car agrees");
    gta_tiles_free(&ti);
    gta_style_free(&st);
    return bad;
}

static int cmd_spriteinfo(const char *stylePath)
{
    gta_style st;
    int t, sum = 0;

    if (gta_style_load(stylePath, &st) != 0)
        return 1;

    printf("=== %s ===\n", stylePath);
    printf("sprites in the info section: %d\n", st.sprite_count);
    printf("%-14s %5s %6s %6s  %s\n",
           "category", "count", "base", "sizes", "(w x h of the first few)");

    for (t = 0; t < GTA_SPR_TYPE_COUNT; t++) {
        int base  = gta_style_sprite_base(&st, (gta_sprite_type)t);
        int count = gta_style_sprite_count(&st, (gta_sprite_type)t);
        int i, shown;
        int minw = 999, maxw = 0, minh = 999, maxh = 0;

        sum += count;
        for (i = 0; i < count && base + i < st.sprite_count; i++) {
            const struct gta_sprite *sp = &st.sprites[base + i];
            if (sp->w < minw) minw = sp->w;
            if (sp->w > maxw) maxw = sp->w;
            if (sp->h < minh) minh = sp->h;
            if (sp->h > maxh) maxh = sp->h;
        }
        if (count == 0) { minw = maxw = minh = maxh = 0; }

        printf("%-14s %5d %6d  %2dx%-2d..%2dx%-2d ",
               gta_sprite_type_name[t], count, base, minw, minh, maxw, maxh);
        shown = count < 8 ? count : 8;
        for (i = 0; i < shown && base + i < st.sprite_count; i++)
            printf(" %dx%d", st.sprites[base + i].w, st.sprites[base + i].h);
        printf("\n");
    }
    printf("\nsum of counts %d, sprite records %d%s\n",
           sum, st.sprite_count,
           sum == st.sprite_count ? "  (agree)" : "  *** DISAGREE ***");

    gta_style_free(&st);
    return 0;
}

/* One frame in a chosen render mode, so every combination the Amiga offers can
 * be looked at on the host before it is built.
 *
 *   gtadump mode <map.cmp> <tiles.til> <bx> <by> <out.bmp> <flat> <sx> <sy> [zoom]
 *
 * flat 0/1, sx and sy 1 or 2. The output BMP is always the full 320x200 the
 * player would see, expanded, so two modes can be compared as pictures rather
 * than as numbers. */
static int cmd_mode(const char *mapPath, const char *tilesPath,
                    int bx, int by, const char *out,
                    int flat, int sx, int sy, int zoom_px)
{
    gta_map mp;
    gta_tiles ti;
    gta_view view;
    unsigned char *low, *full;
    const int W = 320, H = 200;
    int lw, lh;

    if (sx != 1 && sx != 2) sx = 1;
    if (sy != 1 && sy != 2) sy = 1;
    lw = W / sx;
    lh = H / sy;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) { gta_map_free(&mp); return 1; }

    low  = (unsigned char *)calloc((size_t)lw * lh, 1);
    full = (unsigned char *)calloc((size_t)W * H, 1);
    if (!low || !full) { free(low); free(full);
                         gta_tiles_free(&ti); gta_map_free(&mp); return 1; }

    gta_render_init(&view, &mp, &ti);
    view.flat_2d = flat;
    if (zoom_px > 0)
        gta_render_set_zoom(&view, zoom_px);
    /* The camera height, so the host tool can render the same view at several
     * of them and they can be compared side by side without a rebuild. The
     * game has it on F7/F8; this is the same knob for `gtadump mode`. */
    {
        const char *e = getenv("GTA_CAM_H_OVERRIDE");
        if (e) {
            gta_render_set_cam_h(&view, atoi(e));
            printf("mode: camera height override %d\n", view.cam_h);
        }
    }
    gta_render_target(&view, low, lw, lh, lw);
    gta_render_look_at_block(&view, bx, by);
    gta_render_frame(&view);
    gta_render_expand(low, lw, lh, lw, full, W, sx, sy);

    printf("mode: %s %dx%d (x%d,x%d) zoom %d -> %ld columns, %ld lids, "
           "%ld walls\n",
           flat ? "flat-2D" : "2.5D", lw, lh, sx, sy, view.zoom_px,
           view.columns_visited, view.lids_drawn, view.walls_drawn);

    if (write_bmp8(out, full, W, H, ti.palette) == 0)
        printf("  wrote %s\n", out);

    free(low);
    free(full);
    gta_render_free(&view);
    gta_tiles_free(&ti);
    gta_map_free(&mp);
    return 0;
}

/* Draw one sprite at 16 angles, marching it off every edge of the frame.
 *
 *   gtadump spritetest <tiles.til> <out.bmp> [index] [zoom]
 *
 * The rotozoom's clipping is the riskiest new code in the renderer: it steps
 * the source coordinate to the clip boundary instead of testing inside the
 * loop, and getting that wrong writes outside the frame - which on the Amiga
 * is silent corruption of whatever else is in fast RAM rather than a crash.
 * This walks a sprite across all four edges at sixteen angles, so an
 * off-by-one in any of the four adjustments shows up as a torn sprite here and
 * as an ASan report under gtadump_asan.
 *
 * Nothing but sprites is drawn, on a flat background, so a stray pixel has
 * nowhere to hide. */
static int cmd_spritetest(const char *tilesPath, const char *out,
                          int index, int zoom_px)
{
    gta_map mp;
    gta_tiles ti;
    gta_view view;
    unsigned char *canvas;
    const int W = 320, H = 200;
    int i;

    if (gta_tiles_load(tilesPath, &ti) != 0)
        return 1;
    if (index < 0)
        index = gta_tiles_sprite_base(&ti, 7);      /* first pedestrian */
    if (index >= ti.n_sprites) index = 0;

    memset(&mp, 0, sizeof(mp));
    canvas = (unsigned char *)malloc((size_t)W * H);
    if (!canvas) { gta_tiles_free(&ti); return 1; }
    memset(canvas, 200, (size_t)W * H);

    /* The view needs its per-level tables, and gta_render_frame() is what
     * fills them - but it would also draw a city. An empty map draws nothing,
     * so one frame sets the tables up and leaves the canvas alone. */
    gta_render_init(&view, &mp, &ti);
    if (zoom_px > 0)
        gta_render_set_zoom(&view, zoom_px);
    gta_render_target(&view, canvas, W, H, W);
    gta_render_look_at_block(&view, 32, 32);
    view.clear_index = 200;
    gta_render_frame(&view);

    /* Sixteen positions on a ring that straddles the frame's edges, at sixteen
     * different angles, plus one in the middle.
     *
     * The radius is in WORLD pixels but chosen so it lands at a fixed SCREEN
     * radius whatever the zoom: 32 world pixels are `zoom` screen pixels, so
     * r_world = 110 * 32 / zoom puts the ring 110 screen pixels out - past the
     * top and bottom edges of a 200-line frame and inside the left and right
     * ones. The first version used a fixed world radius and quietly culled
     * sixteen of its seventeen sprites at the default zoom, which is a test
     * that passes by not running. */
    for (i = 0; i < 16; i++) {
        int a = i * 16;
        long r = ((110L * 32L) / view.zoom_px) << 16;
        long dx = ((long)gta_sin(a) * (r >> 14));
        long dy = -((long)gta_cos(a) * (r >> 14));
        gta_render_add_sprite(&view, view.cam_x + dx, view.cam_y + dy,
                              -1, GTA_GREF, index, a);
    }
    gta_render_add_sprite(&view, view.cam_x, view.cam_y, -1, GTA_GREF,
                          index, 0);

    /* clear_index stays 200, so the frame the sprites land on is the flat
     * background and every drawn pixel is one of theirs. */
    gta_render_frame(&view);

    printf("spritetest: sprite %d (%dx%d), zoom %d, %ld drawn of 17\n",
           index, (int)ti.sprites[index].w, (int)ti.sprites[index].h,
           view.zoom_px, view.sprites_drawn);

    if (write_bmp8(out, canvas, W, H, ti.palette) == 0)
        printf("  wrote %s\n", out);

    free(canvas);
    gta_render_free(&view);
    gta_tiles_free(&ti);
    return 0;
}

/* THE SCRIPTABLE HOST HARNESS.
 *
 *   gtadump walk <map.cmp> <tiles.til> <script.txt> <out-prefix> [w h zoom]
 *
 * Every renderer bug this project has had was found by a host script rather
 * than by looking at the emulator, and until now there was no way to drive the
 * PLAYER from one - only the camera. This is the same idea as the Amiga's
 * Work:autoinput.txt and takes the same shape of file, one line per leg:
 *
 *     turn forward walk ticks      e.g.  0 1 0 60   RUN straight for 60 ticks
 *                                        0 1 1 60   walk straight instead
 *                                       -1 0 0 32   turn left on the spot
 *
 * The third column is `walk`, and zero RUNS - GTA 1 has no walk key and the
 * player is always jogging, so the default had to be the run.
 *
 * A frame is written at the END of each leg, which is what makes the output a
 * contact sheet of decisions rather than of time. Lines starting with # are
 * comments; a blank line is ignored.
 *
 * It prints the player's position, layer and ground type after every leg, so
 * "he walked into the building" is answered by the log and only the surprises
 * need the pictures. */
static int cmd_walk(const char *mapPath, const char *tilesPath,
                    const char *scriptPath, const char *prefix,
                    int w, int h, int zoom_px)
{
    static const char *const ground_name[8] = {
        "air", "water", "road", "pavement", "field", "building", "?6", "?7"
    };
    gta_map mp;
    gta_tiles ti;
    gta_view view;
    gta_player pl;
    unsigned char *canvas;
    FILE *script;
    char line[256];
    char out[512];
    int leg = 0, total_ticks = 0;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) {
        gta_map_free(&mp);
        return 1;
    }
    script = fopen(scriptPath, "r");
    if (!script) {
        fprintf(stderr, "gtadump: cannot open %s\n", scriptPath);
        gta_tiles_free(&ti);
        gta_map_free(&mp);
        return 1;
    }
    canvas = (unsigned char *)calloc((size_t)w * h, 1);
    if (!canvas) {
        fclose(script);
        gta_tiles_free(&ti);
        gta_map_free(&mp);
        return 1;
    }

    gta_render_init(&view, &mp, &ti);
    if (zoom_px > 0)
        gta_render_set_zoom(&view, zoom_px);
    gta_render_target(&view, canvas, w, h, w);

    /* The first line of the script is the starting block. Putting it in the
     * script rather than on the command line means a script is reproducible on
     * its own. */
    {
        int sx = 64, sy = 64, got = 0;
        /* Comments and blank lines are skipped to find it. A script whose
         * first line is a comment quietly starting the player somewhere else
         * is exactly the kind of wrong answer this harness exists to prevent -
         * and it happened on its very first run. */
        while (fgets(line, sizeof(line), script)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
                continue;
            if (sscanf(line, "start %d %d", &sx, &sy) == 2) { got = 1; break; }
            fprintf(stderr, "gtadump: first non-comment line must be "
                            "\"start <bx> <by>\", got: %s", line);
            break;
        }
        if (!got) {
            fprintf(stderr, "gtadump: no start line, using (64,64)\n");
            sx = 64; sy = 64;
        }
        if (!gta_player_init(&pl, &mp, &ti, sx, sy))
            printf("walk: WARNING - block (%d,%d) has no walkable layer\n",
                   sx, sy);
        printf("walk: start (%d,%d) layer %d ground %s, ped sprites %d from %d\n",
               sx, sy, pl.layer, ground_name[pl.ground & 7],
               pl.ped_count, pl.ped_base);
    }

    while (fgets(line, sizeof(line), script)) {
        int turn = 0, fwd = 0, walk = 0, ticks = 0, i;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (sscanf(line, "%d %d %d %d", &turn, &fwd, &walk, &ticks) != 4)
            continue;

        for (i = 0; i < ticks; i++)
            gta_player_update(&pl, &mp, turn, fwd, walk);
        total_ticks += ticks;

        /* The camera is the player. That is GTA's own behaviour on foot and it
         * is also the only way a bug in the player's position shows up as a
         * bug in the picture rather than as a sprite drifting off the edge. */
        view.cam_x = pl.x;
        view.cam_y = pl.y;
        gta_render_add_sprite(&view, pl.x, pl.y, pl.layer, gta_player_grid(&pl),
                              gta_player_sprite(&pl), gta_player_draw_angle(&pl));
        gta_render_frame(&view);

        printf("leg %2d: turn %2d fwd %2d walk %d x%3d  ->  block (%3d,%3d) "
               "+%2ld.%02ld,%2ld.%02ld  layer %d  angle %3d  ground %-8s%s%s\n",
               leg, turn, fwd, walk, ticks,
               (int)(pl.x >> 21), (int)(pl.y >> 21),
               (pl.x >> 16) & 31, (((pl.x & 0xFFFF) * 100) >> 16),
               (pl.y >> 16) & 31, (((pl.y & 0xFFFF) * 100) >> 16),
               pl.layer, pl.angle, ground_name[pl.ground & 7],
               pl.blocked_x ? "  BLOCKED-X" : "",
               pl.blocked_y ? "  BLOCKED-Y" : "");

        sprintf(out, "%s%02d.bmp", prefix, leg);
        write_bmp8(out, canvas, w, h, ti.palette);
        leg++;
    }

    printf("walk: %d legs, %d ticks, %ld sprites drawn on the last frame\n",
           leg, total_ticks, view.sprites_drawn);

    fclose(script);
    free(canvas);
    gta_render_free(&view);
    gta_tiles_free(&ti);
    gta_map_free(&mp);
    return 0;
}

/* Contact sheet of one sprite category read back out of the BAKED file.
 *
 * This is the round-trip proof, not a convenience: gtabake writes the sprite
 * section and gta_tiles.c reads it, and the only way to know the index survived
 * the trip is to look at the pictures it points to. A wrong offset shows as a
 * sprite sliced in half, which is unmistakable and which a byte-count check
 * would not catch. */
static int cmd_spritedelta(const char *tilPath, int sprite, const char *out)
{
    gta_tiles t;
    int nd, cols, rows, cell, w, h, i;
    unsigned char *sheet, *scratch;

    if (gta_tiles_load(tilPath, &t) != 0)
        return 1;
    if (sprite < 0 || sprite >= t.n_sprites) {
        fprintf(stderr, "gtadump: sprite %d is not in 0..%d\n",
                sprite, t.n_sprites - 1);
        gta_tiles_free(&t);
        return 1;
    }
    nd = gta_tiles_delta_count(&t, sprite);
    printf("sprite %d: %dx%d, %d deltas\n", sprite,
           t.sprites[sprite].w, t.sprites[sprite].h, nd);

    cell = (t.sprites[sprite].w > t.sprites[sprite].h
            ? t.sprites[sprite].w : t.sprites[sprite].h) + 2;
    cols = nd + 2;                      /* base, each delta, then all the
                                         * damage panels laid over one copy */
    if (cols > 8) cols = 8;
    rows = (nd + 2 + cols - 1) / cols;
    w = cols * cell;
    h = rows * cell;

    sheet = (unsigned char *)malloc((size_t)w * (size_t)h);
    scratch = (unsigned char *)malloc((size_t)t.sprites[sprite].w
                                      * t.sprites[sprite].h);
    if (!sheet || !scratch) {
        free(sheet); free(scratch); gta_tiles_free(&t); return 1;
    }
    /* Index 0 is transparent, so the sheet is cleared to something else -
     * otherwise transparency and black are the same picture, which is the
     * mistake this project already made once with the tiles. */
    memset(sheet, 255, (size_t)w * (size_t)h);

    for (i = 0; i <= nd + 1; i++) {
        int cx = (i % cols) * cell + 1;
        int cy = (i / cols) * cell + 1;
        int x, y, runs;
        /* i == 0 is the base: delta -1 decodes to a plain copy. The LAST cell
         * is every damage panel over ONE copy - a car shot to pieces, and the
         * proof that the mask apply the game draws with agrees with the
         * single-delta one beside it. */
        if (i == nd + 1) {
            runs = gta_tiles_delta_apply_mask(&t, sprite,
                                              GTA_DELTA_DMG_MASK, scratch);
            printf("  all damage panels -> %d runs\n", runs);
        } else {
            runs = gta_tiles_delta_apply(&t, sprite, i - 1, scratch);
            if (i > 0)
                printf("  delta %2d -> %d runs\n", i - 1, runs);
        }
        for (y = 0; y < t.sprites[sprite].h; y++)
            for (x = 0; x < t.sprites[sprite].w; x++) {
                unsigned char px = scratch[(long)y * t.sprites[sprite].w + x];
                if (px) sheet[(long)(cy + y) * w + cx + x] = px;
            }
    }

    if (write_bmp8(out, sheet, w, h, t.palette) == 0)
        printf("  wrote %s (%dx%d, base + %d deltas)\n",
               out, w, h, nd);
    free(sheet);
    free(scratch);
    gta_tiles_free(&t);
    return 0;
}

static int cmd_tilesprites(const char *tilPath, const char *out, int type)
{
    gta_tiles t;
    int cols = 24, cell = 40;
    int base, count, rows, w, h, i;
    unsigned char *sheet;

    if (gta_tiles_load(tilPath, &t) != 0)
        return 1;
    gta_tiles_describe(&t, stdout);

    if (type < 0) { base = 0; count = t.n_sprites; }
    else { base = gta_tiles_sprite_base(&t, type);
           count = gta_tiles_sprite_count(&t, type); }

    printf("category %d: %d sprites from index %d\n", type, count, base);
    if (count <= 0) { gta_tiles_free(&t); return 1; }

    /* The largest sprite in the file is 220x128, so a fixed cell would either
     * clip it or waste the sheet. The cell is sized for the category. */
    for (i = 0; i < count; i++) {
        const gta_tile_sprite *sp = &t.sprites[base + i];
        if (sp->w > cell) cell = sp->w;
        if (sp->h > cell) cell = sp->h;
    }
    cell += 2;
    if (count < cols) cols = count;
    rows = (count + cols - 1) / cols;
    w = cols * cell;
    h = rows * cell;

    /* Index 0 is the sprite's transparent colour, so the sheet is cleared to
     * something else - otherwise transparency and black are the same picture,
     * which is the mistake this project already made once with the tiles. */
    sheet = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!sheet) { gta_tiles_free(&t); return 1; }
    memset(sheet, 255, (size_t)w * (size_t)h);

    for (i = 0; i < count; i++) {
        const gta_tile_sprite *sp = &t.sprites[base + i];
        const unsigned char *src = t.sprite_pixels + sp->off;
        int cx = (i % cols) * cell + 1;
        int cy = (i / cols) * cell + 1;
        int x, y;
        for (y = 0; y < sp->h; y++)
            for (x = 0; x < sp->w; x++) {
                unsigned char px = src[(long)y * sp->w + x];
                if (px) sheet[(long)(cy + y) * w + cx + x] = px;
            }
    }

    if (write_bmp8(out, sheet, w, h, t.palette) == 0)
        printf("  wrote %s (%dx%d, %d cells of %d px)\n",
               out, w, h, count, cell);
    free(sheet);
    gta_tiles_free(&t);
    return 0;
}

/* Contact sheet of every sprite, laid out on a fixed grid. The cells are
 * generous because sprites vary in size; a car is far bigger than a bullet. */
static int cmd_sprites(const char *stylePath, const char *out)
{
    gta_style st;
    int cols = 24, cell = 72;
    int rows, w, h, i;
    unsigned char *sheet;
    unsigned char bg;

    if (gta_style_load(stylePath, &st) != 0)
        return 1;

    printf("=== %s ===\n", stylePath);
    printf("sprites: %d\n", st.sprite_count);
    if (st.sprite_count <= 0) {
        gta_style_free(&st);
        return 1;
    }

    rows = (st.sprite_count + cols - 1) / cols;
    w = cols * cell;
    h = rows * cell;

    /* Sprites are transparent where the palette index is 0, so the background
     * must not be 0 or transparency would be invisible. Index 255 is as good a
     * marker as any and is rarely part of the artwork. */
    bg = 255;
    sheet = (unsigned char *)malloc((size_t)w * (size_t)h);
    if (!sheet) {
        fprintf(stderr, "out of memory for a %dx%d sheet\n", w, h);
        gta_style_free(&st);
        return 1;
    }
    memset(sheet, bg, (size_t)w * (size_t)h);

    for (i = 0; i < st.sprite_count; i++) {
        int cx = (i % cols) * cell;
        int cy = (i / cols) * cell;
        gta_style_get_sprite(&st, i, sheet + (long)cy * w + cx, w);
    }

    if (write_bmp8(out, sheet, w, h, st.palette) == 0)
        printf("  wrote %s (%dx%d, %d cells of %d px)\n",
               out, w, h, st.sprite_count, cell);

    free(sheet);
    gta_style_free(&st);
    return 0;
}

/* Same view as cmd_map, but through the 2:1 downscaler, so the two can be
 * compared pixel for pixel at the size the port will actually run. */
static int cmd_map32(const char *mapPath, const char *stylePath,
                     int x0, int y0, int tiles_w, int tiles_h,
                     const char *modeName, const char *out)
{
    gta_map mp;
    gta_style st;
    unsigned char *canvas, full[GTA_BLOCK_AREA];
    int half = GTA_BLOCK_DIM / 2;
    int w, h, tx, ty, z, drawn = 0;
    int mode = (strcmp(modeName, "avg") == 0) ? SCALE_AVERAGE : SCALE_NEAREST;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_style_load(stylePath, &st) != 0) {
        gta_map_free(&mp);
        return 1;
    }

    w = tiles_w * half;
    h = tiles_h * half;
    canvas = (unsigned char *)calloc(1, (size_t)w * (size_t)h);
    if (!canvas) {
        fprintf(stderr, "out of memory for a %dx%d canvas\n", w, h);
        gta_map_free(&mp);
        gta_style_free(&st);
        return 1;
    }

    for (ty = 0; ty < tiles_h; ty++) {
        for (tx = 0; tx < tiles_w; tx++) {
            int height = gta_map_column_height(&mp, x0 + tx, y0 + ty);
            unsigned char *dst = canvas + (long)(ty * half) * w + tx * half;
            for (z = 0; z < height; z++) {
                gta_block b;
                if (!gta_map_block(&mp, x0 + tx, y0 + ty, z, &b))
                    continue;
                if (b.faces[GTA_FACE_LID] == 0)
                    continue;
                if (gta_style_get_block(&st, GTA_BLOCK_LID, b.faces[GTA_FACE_LID],
                                        full, GTA_BLOCK_DIM) != 0)
                    continue;
                downscale_block(full, GTA_BLOCK_DIM, dst, w, st.palette, mode);
                drawn++;
            }
        }
    }

    printf("--- %s: %d lids at %dx%d px ---\n",
           mode == SCALE_AVERAGE ? "average+snap" : "nearest", drawn, half, half);
    if (write_bmp8(out, canvas, w, h, st.palette) == 0)
        printf("  wrote %s (%dx%d)\n", out, w, h);

    free(canvas);
    gta_map_free(&mp);
    gta_style_free(&st);
    return 0;
}

/* One frame of the real 2.5D renderer, straight out of native/gta_render.c and
 * into a BMP.
 *
 * This is the whole reason gta_render.c has no Amiga headers in it. A build,
 * deploy, boot and screenshot round trip through WinUAE is a couple of minutes;
 * this is a second, and it answers the only question that matters early on -
 * does the projection look like GTA. The Amiga run then proves it is fast
 * enough and does not Guru, which is a different question.
 */
/* narrow <map> <til> [bx by] - the offset-target regression.
 *
 * The narrow render modes (F4 on the Amiga) point the renderer at the MIDDLE
 * of the frame buffer: dst = buffer + 27, with the pitch still 320. Every
 * assumption of the form "the target starts at the beginning of a row and
 * fills it" is wrong there, and gta_render_frame() held exactly one of them -
 * a clear of dst_pitch * dst_h bytes from dst, which ran off the end of the
 * allocation by the x offset. On the Amiga at half resolution that landed in
 * the statics behind the low-resolution buffer and zeroed the mode variables,
 * so F4 appeared to do nothing at all; at full resolution it wrote past the
 * chunky allocation every frame and got away with it.
 *
 * The whole buffer is filled with a marker first, so this catches BOTH the
 * overrun (the guard band after the buffer) and any pixel written outside the
 * target rectangle (the marker still standing inside it). One second here
 * against a two-minute emulator round trip. */
static int cmd_narrow(const char *mapPath, const char *tilesPath, int bx, int by)
{
    static const struct { int w, x; const char *name; } vm[3] = {
        { 320, 0,  "full 320" }, { 266, 27, "4:3 266" }, { 256, 32, "5:4 256" }
    };
    const int W = 320, H = 200, GUARD = 64, MARK = 0xAA;
    gta_map mp;
    gta_tiles ti;
    int mi, scale, bad = 0;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) { gta_map_free(&mp); return 1; }

    for (mi = 0; mi < 3; mi++) {
        for (scale = 1; scale <= 2; scale++) {
            int pitch = W / scale, h = H / scale;
            int w = vm[mi].w / scale, x = vm[mi].x / scale;
            long bytes = (long)pitch * h;
            unsigned char *buf = (unsigned char *)malloc((size_t)bytes + GUARD);
            gta_view view;
            int y, cx, stray = 0, guard = 0;

            if (!buf) { fprintf(stderr, "gtadump: out of memory\n"); bad = 1; break; }
            memset(buf, MARK, (size_t)bytes + GUARD);

            gta_render_init(&view, &mp, &ti);
            gta_render_set_zoom(&view, GTA_TILE_DIM / scale);
            gta_render_target(&view, buf + x, w, h, pitch);
            gta_render_look_at_block(&view, bx, by);
            gta_render_frame(&view);

            for (y = 0; y < GUARD; y++)
                if (buf[bytes + y] != MARK) guard++;

            /* Anything the renderer touched outside its own rectangle. The
             * marker is a colour the renderer never writes: it clears to
             * clear_index and draws map pixels, and 0xAA is neither unless the
             * art happens to use it - which is why a stray count is reported
             * rather than trusted blindly when it is small. */
            for (y = 0; y < h; y++) {
                const unsigned char *row = buf + (long)y * pitch;
                for (cx = 0; cx < pitch; cx++) {
                    if (cx >= x && cx < x + w) continue;
                    if (row[cx] != MARK) stray++;
                }
            }

            printf("narrow: %-9s scale %d  target %dx%d at x=%d pitch %d  "
                   "guard %d  stray %d  %s\n",
                   vm[mi].name, scale, w, h, x, pitch, guard, stray,
                   (guard == 0 && stray == 0) ? "ok" : "FAILED");
            if (guard || stray) bad = 1;
            free(buf);
        }
    }

    gta_tiles_free(&ti);
    gta_map_free(&mp);
    printf("--- narrow target test %s ---\n", bad ? "FAILED" : "PASSED");
    return bad ? 1 : 0;
}

static int cmd_view(const char *mapPath, const char *tilesPath,
                    int bx, int by, int w, int h, const char *out,
                    int clear_index, int zoom_px)
{
    gta_map mp;
    gta_tiles ti;
    gta_view view;
    unsigned char *canvas;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) {
        gta_map_free(&mp);
        return 1;
    }
    gta_tiles_describe(&ti, stdout);

    canvas = (unsigned char *)calloc((size_t)w * h, 1);
    if (!canvas) {
        fprintf(stderr, "gtadump: out of memory for a %dx%d frame\n", w, h);
        gta_tiles_free(&ti);
        gta_map_free(&mp);
        return 1;
    }

    gta_render_init(&view, &mp, &ti);
    /* Optional 9th argument: the clear index, for telling a hole from a black
     * tile. See gta_view.clear_index. */
    if (zoom_px > 0)
        gta_render_set_zoom(&view, zoom_px);
    /* GTA_CAM_H_OVERRIDE, the same knob `gtadump mode` has - and it is here
     * so that holecheck.sh can sweep the CAMERA HEIGHT as well as the camera
     * position. The gaps reported as "roofs in the wrong place, roofs
     * vanishing" appear only at a low camera, so a hole test that only ever
     * runs at the default height cannot see them. */
    {
        const char *e = getenv("GTA_CAM_H_OVERRIDE");
        if (e)
            gta_render_set_cam_h(&view, atoi(e));
    }
    if (clear_index >= 0)
        view.clear_index = clear_index;
    gta_render_target(&view, canvas, w, h, w);
    gta_render_look_at_block(&view, bx, by);
    gta_render_frame(&view);

    printf("view: block (%d,%d), %dx%d, %ld columns, %ld lids, %ld walls\n",
           bx, by, w, h, view.columns_visited, view.lids_drawn,
           view.walls_drawn);
    printf("      step x per grid level:");
    {
        int g;
        for (g = 0; g < GTA_GRID_LEVELS; g++)
            printf(" %ld.%02ld", view.step[g] >> 16,
                   ((view.step[g] & 0xFFFF) * 100) >> 16);
    }
    /* And the Y pitch beside it, because it is NOT the same number - the
     * projection squashes y to five sixths (gta_view.stepy). Printing only one
     * of the two is how a squash bug hides. */
    printf("\n      step y per grid level:");
    {
        int g;
        for (g = 0; g < GTA_GRID_LEVELS; g++)
            printf(" %ld.%02ld", view.stepy[g] >> 16,
                   ((view.stepy[g] & 0xFFFF) * 100) >> 16);
    }
    printf("\n");

    if (write_bmp8(out, canvas, w, h, ti.palette) == 0)
        printf("wrote %s\n", out);

    free(canvas);
    gta_render_free(&view);      /* the lid cache: 1.5 MB, and ASan says so */
    gta_tiles_free(&ti);
    gta_map_free(&mp);
    return 0;
}

/* Every block of one column, decoded. The renderer draws black wherever a
 * column has no lid, and "is that the map or is that us?" is a question that
 * only the raw numbers answer. */
static int cmd_column(const char *mapPath, int bx, int by)
{
    gta_map mp;
    int z, h;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;

    h = gta_map_column_height(&mp, bx, by);
    printf("column (%d,%d): height %d\n", bx, by, h);
    for (z = 0; z < GTA_MAP_LAYERS; z++) {
        gta_block b;
        if (!gta_map_block(&mp, bx, by, z, &b)) {
            printf("  z=%d  air\n", z);
            continue;
        }
        printf("  z=%d  W=%-3u E=%-3u N=%-3u S=%-3u LID=%-3u  "
               "type_map=%04x ext=%02x  slope=%-2d rot=%d flat=%d "
               "lr=%d tb=%d rail=%d remap=%d  dirs=%c%c%c%c hint=%d\n",
               z, b.faces[GTA_FACE_W], b.faces[GTA_FACE_E],
               b.faces[GTA_FACE_N], b.faces[GTA_FACE_S],
               b.faces[GTA_FACE_LID],
               (unsigned)b.type_map, (unsigned)b.type_map_ext,
               gta_block_slope(&b), gta_block_lid_rotation(&b),
               gta_block_is_flat(&b) ? 1 : 0,
               gta_block_flip_lr(&b) ? 1 : 0,
               gta_block_flip_tb(&b) ? 1 : 0,
               gta_block_is_railway(&b) ? 1 : 0,
               gta_block_remap(&b),
               gta_block_dir_north(&b) ? 'N' : '.',
               gta_block_dir_south(&b) ? 'S' : '.',
               gta_block_dir_west(&b)  ? 'W' : '.',
               gta_block_dir_east(&b)  ? 'E' : '.',
               gta_block_traffic_hint(&b));
    }
    gta_map_free(&mp);
    return 0;
}

/* Every face index the map actually uses, against what the style actually has.
 *
 * The renderer skips any face index it cannot resolve, and a skipped face is
 * not a hole - whatever was drawn there earlier stays. So an out-of-range index
 * shows up as a block-sized piece of the WRONG thing, which holecheck.sh cannot
 * see and the eye reads as "a square of the building is missing". This command
 * settles that in one run instead of by staring at screenshots. */
static int cmd_stats(const char *mapPath, const char *tilesPath)
{
    gta_map mp;
    gta_tiles ti;
    int x, y, z, f;
    long max_side = -1, max_lid = -1;
    long bad_side = 0, bad_lid = 0, no_lid_on_top = 0, columns = 0;

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) {
        gta_map_free(&mp);
        return 1;
    }

    for (y = 0; y < GTA_MAP_DIM; y++) {
        for (x = 0; x < GTA_MAP_DIM; x++) {
            int h = gta_map_column_height(&mp, x, y);
            if (h <= 0)
                continue;
            columns++;
            for (z = 0; z < h; z++) {
                gta_block b;
                if (!gta_map_block(&mp, x, y, z, &b))
                    continue;
                for (f = 0; f < GTA_FACE_LID; f++) {
                    long v = b.faces[f];
                    if (v > max_side) max_side = v;
                    if (v && v >= ti.n_side) bad_side++;
                }
                if (b.faces[GTA_FACE_LID] > max_lid)
                    max_lid = b.faces[GTA_FACE_LID];
                if (b.faces[GTA_FACE_LID] && b.faces[GTA_FACE_LID] >= ti.n_lid)
                    bad_lid++;
                if (z == h - 1 && b.faces[GTA_FACE_LID] == 0)
                    no_lid_on_top++;
            }
        }
    }

    printf("%s against %s\n", mapPath, tilesPath);
    printf("  columns with blocks        %ld\n", columns);
    printf("  style has                  %d side, %d lid\n",
           ti.n_side, ti.n_lid);
    printf("  map uses side up to        %ld\n", max_side);
    printf("  map uses lid  up to        %ld\n", max_lid);
    printf("  side faces out of range    %ld\n", bad_side);
    printf("  lid  faces out of range    %ld\n", bad_lid);
    printf("  top blocks with no lid     %ld\n", no_lid_on_top);
    printf("  per layer: blocks / with a side face / with a lid\n");
    for (z = 0; z < GTA_MAP_LAYERS; z++) {
        long n = 0, sides = 0, lids = 0;
        for (y = 0; y < GTA_MAP_DIM; y++) {
            for (x = 0; x < GTA_MAP_DIM; x++) {
                gta_block b;
                if (!gta_map_block(&mp, x, y, z, &b))
                    continue;
                n++;
                if (b.faces[GTA_FACE_W] || b.faces[GTA_FACE_E] ||
                    b.faces[GTA_FACE_N] || b.faces[GTA_FACE_S])
                    sides++;
                if (b.faces[GTA_FACE_LID])
                    lids++;
            }
        }
        printf("    z=%d  %8ld  %8ld  %8ld\n", z, n, sides, lids);
    }

    /* Slopes are drawn flat by the renderer, so knowing how many there are is
     * knowing how wrong the city is. A sloped block's surface runs from grid z
     * to grid z+1 across the block; drawn flat at z+1 it sits a whole level
     * too high at its low edge, which reads as a square cut out of the ground
     * rather than as a missing ramp. */
    {
        long sloped = 0, flat_blocks = 0, railway = 0;
        for (y = 0; y < GTA_MAP_DIM; y++) {
            for (x = 0; x < GTA_MAP_DIM; x++) {
                for (z = 0; z < GTA_MAP_LAYERS; z++) {
                    gta_block b;
                    if (!gta_map_block(&mp, x, y, z, &b))
                        continue;
                    if (gta_block_slope(&b)) sloped++;
                    if (gta_block_is_flat(&b)) flat_blocks++;
                    if (gta_block_is_railway(&b)) railway++;
                }
            }
        }
        printf("  sloped blocks              %ld\n", sloped);
        printf("  flat blocks                %ld\n", flat_blocks);
        printf("  railway blocks             %ld\n", railway);
    }

    /* Where the elevated railway is. Same reason as the bridge list below: a
     * report about the track needs a coordinate before anything can be
     * rendered at it. */
    {
        int found = 0;
        printf("  railway blocks (bx,by,z):\n");
        for (y = 0; y < GTA_MAP_DIM && found < 10; y++) {
            for (x = 0; x < GTA_MAP_DIM && found < 10; x++) {
                for (z = 0; z < GTA_MAP_LAYERS && found < 10; z++) {
                    gta_block b;
                    if (!gta_map_block(&mp, x, y, z, &b))
                        continue;
                    if (!gta_block_is_railway(&b))
                        continue;
                    printf("    %3d,%3d z=%d  lid=%-3u flat=%d slope=%-2d "
                           "W=%-3u E=%-3u N=%-3u S=%-3u\n",
                           x, y, z, b.faces[GTA_FACE_LID],
                           gta_block_is_flat(&b) ? 1 : 0, gta_block_slope(&b),
                           b.faces[GTA_FACE_W], b.faces[GTA_FACE_E],
                           b.faces[GTA_FACE_N], b.faces[GTA_FACE_S]);
                    found++;
                }
            }
        }
    }

    /* Where the bridges are: a column with water on layer 0 and something with
     * a lid three or more layers up is a deck over water. Printed because a bug
     * report that says "under the bridge" needs a coordinate before it can be
     * rendered, and hunting for one by driving the camera is slow. */
    {
        int found = 0;
        printf("  decks over water (bx,by, deck layer):\n");
        for (y = 0; y < GTA_MAP_DIM && found < 10; y += 3) {
            for (x = 0; x < GTA_MAP_DIM && found < 10; x += 3) {
                gta_block b;
                int deck = -1;
                if (!gta_map_block(&mp, x, y, 0, &b) ||
                    b.faces[GTA_FACE_LID] == 0)
                    continue;                      /* layer 0 must be water */
                for (z = 3; z < GTA_MAP_LAYERS; z++) {
                    gta_block d;
                    if (gta_map_block(&mp, x, y, z, &d) &&
                        d.faces[GTA_FACE_LID])
                        deck = z;
                }
                if (deck >= 0) {
                    printf("    %3d,%3d  layer %d\n", x, y, deck);
                    found++;
                }
            }
        }
    }

    gta_tiles_free(&ti);
    gta_map_free(&mp);
    return (bad_side || bad_lid) ? 1 : 0;
}


/* DRIVE ONE VEHICLE BY SCRIPT - the physics module's test bench.
 *
 * The script is one order per line:
 *     start <bx> <by> <angle>          place it (block coords, 0..255 angle)
 *     run <ticks> <thr> <brk> <steer> <hb>
 * and the output is telemetry every 25 ticks plus a 512x512 BMP of the path:
 * 4 px per block, 128x128 blocks around the start, road in grey, the path in
 * white with a brighter dot every 10 ticks - the same look-at-the-picture
 * rule as every other tool here. */

/* ==========================================================================
 * THE COLLISION REGRESSION TEST - `gtadump ram`
 *
 * A black tile and an undrawn pixel look identical, and two overlapping cars
 * look like one slightly odd car. So the geometry is measured, not eyeballed:
 * one vehicle is driven into another head-on and the DEEPEST overlap of the
 * whole run is reported in world pixels. Zero means they touched and stopped;
 * anything approaching the target's half-length means the attacker drove
 * through it.
 *
 * The overlap is measured by a separating-axis test written HERE rather than
 * called out of gta_traffic.c on purpose - a test that shares its geometry
 * with the code under test cannot fail. This one is deliberately naive and
 * uses doubles, which is fine: it never goes near the Amiga.
 * ========================================================================== */
static double ram_overlap(double ax, double ay, int aang, double ahl, double ahw,
                          double bx, double by, int bang, double bhl, double bhw)
{
    double axis[4][2], best = 1e9;
    double ac = gta_cos(aang) / 16384.0, as = gta_sin(aang) / 16384.0;
    double bc = gta_cos(bang) / 16384.0, bs = gta_sin(bang) / 16384.0;
    double dx = bx - ax, dy = by - ay;
    int k;

    axis[0][0] =  as; axis[0][1] = -ac;       /* a forward */
    axis[1][0] =  ac; axis[1][1] =  as;       /* a right   */
    axis[2][0] =  bs; axis[2][1] = -bc;
    axis[3][0] =  bc; axis[3][1] =  bs;

    for (k = 0; k < 4; k++) {
        double nx = axis[k][0], ny = axis[k][1];
        double r = dx * nx + dy * ny, pa, pb, t, over;
        if (r < 0) r = -r;
        t = axis[0][0] * nx + axis[0][1] * ny; pa  = (t < 0 ? -t : t) * ahl;
        t = axis[1][0] * nx + axis[1][1] * ny; pa += (t < 0 ? -t : t) * ahw;
        t = axis[2][0] * nx + axis[2][1] * ny; pb  = (t < 0 ? -t : t) * bhl;
        t = axis[3][0] * nx + axis[3][1] * ny; pb += (t < 0 ? -t : t) * bhw;
        over = pa + pb - r;
        if (over <= 0) return 0.0;            /* an axis separates them */
        if (over < best) best = over;
    }
    return best;
}

/* One run. Returns the deepest overlap in tenths of a pixel, and sets *drove
 * if the attacker ended up beyond the target. `verbose` prints the header and
 * the per-tick trace. */
static int ram_run(gta_tiles *tip, int pmodel, int tmodel,
                   int want_speed, int tface, int verbose, int *drove)
{
    gta_tiles ti_unused_;
    static gta_traffic tr;
    gta_veh v;
    const gta_car_info *pi, *tci;
    long start_x, start_y, cruise_vx = 0, cruise_vy = 0;
    double worst = 0.0, tgt_x0, tgt_y0;
    int k, contact = 0, through = 0, first_touch = -1;

    (void)ti_unused_;
    pi  = &tip->cars[pmodel];
    tci = &tip->cars[tmodel];
    *drove = 0;

    /* Run up to speed on an empty road first, so the impact speed is one the
     * model can actually reach rather than one poked into the struct. */
    start_x = 400L << 16;
    start_y = 400L << 16;
    gta_veh_init(&v, tip, pmodel, start_x, start_y, 0);
    for (k = 0; k < 4000; k++) {
        long sp;
        gta_veh_step(&v, 1, 0, 0, 0, 0);
        sp = v.vy < 0 ? -v.vy : v.vy;
        if ((sp >> 16) >= want_speed) break;
    }
    cruise_vx = v.vx;
    cruise_vy = v.vy;

    /* Now put it back at the line, facing north, with that speed, and park the
     * target eighty pixels up the road. */
    gta_veh_init(&v, tip, pmodel, start_x, start_y, 0);
    v.vx = cruise_vx;
    v.vy = cruise_vy;

    gta_traffic_init(&tr, tip, 7UL);
    memset(&tr.cars[0], 0, sizeof tr.cars[0]);
    tr.n = 1;
    tr.cars[0].model = tmodel;
    tr.cars[0].layer = 0;
    tr.cars[0].x = start_x;
    tr.cars[0].y = start_y - (80L << 16);
    tr.cars[0].face = tface;
    tr.cars[0].angle = tface;
    tr.cars[0].speed = 0;
    tr.cars[0].remap = -1;
    tr.cars[0].done = 0;
    tgt_x0 = (double)(tr.cars[0].x >> 16);
    tgt_y0 = (double)(tr.cars[0].y >> 16);

    if (verbose)
    printf("ram: %s (%dx%d px, mass %ld) at %ld px/tick into "
           "%s (%dx%d px, mass %ld) facing %d\n",
           gta_vehicle_class_name(pi->vtype),
           gta_car_world_wid(pi), gta_car_world_len(pi), pi->mass >> 16,
           (cruise_vy < 0 ? -cruise_vy : cruise_vy) >> 16,
           gta_vehicle_class_name(tci->vtype),
           gta_car_world_wid(tci), gta_car_world_len(tci), tci->mass >> 16,
           tface);

    for (k = 0; k < 120; k++) {
        long rvx, rvy, ryaw, rpx, rpy;
        double ov;

        {
            /* THE GAME'S OWN SWEEP, or this measures a game nobody plays:
             * the striker is stopped at the contact instead of ending the
             * tick inside the target, which is what keeps the correction
             * afterwards small. See gta_traffic_sweep_box(). */
            long px0 = v.ox, py0 = v.oy, pa0 = v.ang16;
            long nx_, ny_, na_;
            gta_veh_step(&v, 0, 0, 0, 0, 0);      /* coasting into it */
            nx_ = v.ox; ny_ = v.oy; na_ = v.ang16;
            if (gta_traffic_sweep_box(&tr, px0, py0, pa0, &nx_, &ny_, &na_,
                                      v.len / 2, v.wid / 2, 0)) {
                v.x += nx_ - v.ox;
                v.y += ny_ - v.oy;
                v.ox = nx_; v.oy = ny_; v.ang16 = na_;
            }
        }
        gta_traffic_ram(&tr, v.ox, v.oy, gta_veh_angle(&v),
                        v.len / 2, v.wid / 2, v.vx, v.vy, v.mass, 0,
                        &rvx, &rvy, &ryaw, &rpx, &rpy);
        if (rpx || rpy) {
            v.x += rpx; v.ox += rpx;
            v.y += rpy; v.oy += rpy;
        }
        v.vx += rvx;
        v.vy += rvy;
        v.ang16 = (v.ang16 + ryaw) & 0xFFFFFFL;

        ov = ram_overlap((double)(v.ox >> 16), (double)(v.oy >> 16),
                         gta_veh_angle(&v), v.len / 2.0, v.wid / 2.0,
                         (double)(tr.cars[0].x >> 16),
                         (double)(tr.cars[0].y >> 16),
                         tr.cars[0].face,
                         gta_car_world_len(tci) / 2.0,
                         gta_car_world_wid(tci) / 2.0);
        if (ov > 0.0) {
            contact++;
            if (first_touch < 0) first_touch = k;
            if (ov > worst) worst = ov;
        }
        /* Driving THROUGH is the failure this test exists for: the attacker's
         * centre ends up beyond the target's, along the direction of travel. */
        if (v.oy < tr.cars[0].y)
            through = 1;
        if (getenv("GTA_RAMTICK"))
            printf("  t=%3d me y=%5ld v=%6ld  it y=%5ld  overlap %5.1f px\n",
                   k, v.oy >> 16, v.vy >> 16, tr.cars[0].y >> 16, ov);
    }

    if (verbose)
    printf("ram: deepest overlap %.1f px, %d ticks in contact "
           "(first at t=%d), target moved %.1f px, i ended %.1f px "
           "%s it, my speed %ld px/tick -> %s\n",
           worst, contact, first_touch,
           (double)(tr.cars[0].y >> 16) - tgt_y0,
           (double)((tr.cars[0].y - v.oy) >> 16),
           through ? "PAST" : "short of",
           (v.vy < 0 ? -v.vy : v.vy) >> 16,
           through ? "DROVE THROUGH IT" : "stopped against it");

    *drove = through;
    (void)tgt_x0;
    return (int)(worst * 10.0 + 0.5);
}

static int cmd_ram(const char *tilesPath, int pmodel, int tmodel,
                   int want_speed, int tface)
{
    gta_tiles ti;
    int drove = 0, r;

    if (gta_tiles_load(tilesPath, &ti) != 0) return 1;
    if (pmodel < 0 || pmodel >= ti.n_cars || tmodel < 0 || tmodel >= ti.n_cars) {
        fprintf(stderr, "ram: model out of range (0..%d)\n", ti.n_cars - 1);
        gta_tiles_free(&ti);
        return 1;
    }
    r = ram_run(&ti, pmodel, tmodel, want_speed, tface, 1, &drove);
    (void)r;
    gta_tiles_free(&ti);
    return drove ? 2 : 0;
}

/* EVERY VEHICLE INTO EVERY OTHER - the collision regression test proper.
 *
 * Head-on and broadside, at a parking speed and at a speed most of the fleet
 * cannot even reach, which is the point: the failure this test exists for is
 * a fast heavy vehicle penetrating a light one faster than the impulse can
 * stop it, and that only shows up at the top of the range. Prints the worst
 * offenders and one summary line; the summary is what belongs in PROGRESS.md.
 */
static int cmd_ramsweep(const char *tilesPath, int speed)
{
    gta_tiles ti;
    int p, t, fa, drove, worst = 0, wp = 0, wt = 0, wf = 0;
    int runs = 0, through = 0, over2 = 0;

    if (gta_tiles_load(tilesPath, &ti) != 0) return 1;
    printf("ramsweep: %d models, %d px/tick, head-on and broadside\n",
           ti.n_cars, speed);
    for (p = 0; p < ti.n_cars; p++)
    for (t = 0; t < ti.n_cars; t++)
    for (fa = 0; fa <= 64; fa += 64) {
        int d = ram_run(&ti, p, t, speed, fa, 0, &drove);
        runs++;
        if (drove) {
            through++;
            printf("  THROUGH: %d into %d, face %d\n", p, t, fa);
        }
        if (d > 20) {
            over2++;
            if (over2 <= 12)
                printf("  DEEP: %d into %d, face %d - %d.%d px\n",
                       p, t, fa, d / 10, d % 10);
        }
        if (d > worst) { worst = d; wp = p; wt = t; wf = fa; }
    }
    printf("ramsweep: %d runs, %d drove through, %d deeper than 2 px, "
           "worst %d.%d px (%d into %d, face %d)\n",
           runs, through, over2, worst / 10, worst % 10, wp, wt, wf);
#ifdef GTA_RANGE_CHECK
    {
        extern long gta_rchk_max;
        extern const char *gta_rchk_where;
        printf("ramsweep: widest intermediate %ld (%s) - 32-bit limit is "
               "2147483647, %ld%% of it\n",
               gta_rchk_max, gta_rchk_where,
               gta_rchk_max / 21474836L);
    }
#endif
    gta_tiles_free(&ti);
    return (through || over2) ? 2 : 0;
}




/* HOW MUCH OF THE BODY IS INSIDE A WALL?
 *
 * The game tests ONE point - the nose, half a body length ahead of the centre.
 * That is blind to three things a long vehicle does constantly: reversing into
 * something (the nose is at the other end), clipping a corner with a rear
 * quarter while the nose is past it, and sliding sideways into a kerb. This
 * samples the whole outline instead - the four corners, the thirds of each
 * long side, and the middle of each end - and counts how many of those points
 * are standing on ground a car cannot be on.
 *
 * Ten points, not four: a bus is 60 px long and a block is 32, so two corners
 * can straddle a wall block entirely without either of them being in it. */
static const int WALL_SAMP[10][2] = {   /* along, across; thousandths */
    {-1000, -1000}, {-1000, 0}, {-1000, 1000},
    { -333, -1000}, { -333, 1000},
    {  333, -1000}, {  333, 1000},
    { 1000, -1000}, { 1000, 0}, { 1000, 1000}
};

static int body_in_wall(const gta_nav *nav, int layer, long cx, long cy,
                        int ang, int hl, int hw)
{
    long fx = gta_sin(ang), fy = -gta_cos(ang);
    long rx = gta_cos(ang), ry = gta_sin(ang);
    int i, bad = 0;

    for (i = 0; i < 10; i++) {
        long al = (long)hl * WALL_SAMP[i][0] / 1000;
        long si = (long)hw * WALL_SAMP[i][1] / 1000;
        long px = cx + (fx * al + rx * si) * 4;
        long py = cy + (fy * al + ry * si) * 4;
        int g = gta_nav_ground(gta_nav_at(nav, (int)(px >> 21),
                                          (int)(py >> 21), layer));
        if (g < 2 || g == 5)
            bad++;
    }
    return bad;
}


/* EVERY PEDESTRIAN COLOUR AT ONCE.
 *
 * One walk frame drawn through each of the ped remap tables in turn, so
 * "the crowd is all wearing the player's shirt" is a thing you can look at
 * rather than argue about. Also reports, per table, how many of the sprite's
 * OWN palette entries it actually moves - a table that changes nothing would
 * produce another clone however good the plumbing is.
 */
static int cmd_remapsheet(const char *tilesPath, const char *outBmp, int frame)
{
    gta_tiles ti;
    const gta_tile_sprite *rec;
    const unsigned char *src;
    unsigned char *canvas;
    int base, count, idx, i, x, y, cols, rows, cw, ch, n_show;
    int lo = GTA_REMAP_PED_LO, hi = GTA_REMAP_PED_HI;
    int distinct_total = 0;

    if (gta_tiles_load(tilesPath, &ti) != 0) return 1;
    if (!ti.remaps || ti.n_remaps <= 0) {
        fprintf(stderr, "remapsheet: %s has no remap tables - re-bake it\n",
                tilesPath);
        gta_tiles_free(&ti);
        return 1;
    }
    base = gta_tiles_sprite_base(&ti, 7);
    count = gta_tiles_sprite_count(&ti, 7);
    if (frame < 0 || frame >= count) frame = 0;
    idx = base + frame;
    rec = &ti.sprites[idx];
    src = ti.sprite_pixels + rec->off;
    cw = (int)rec->w + 2;
    ch = (int)rec->h + 2;

    n_show = hi - lo + 2;                 /* +1 for the un-remapped original */
    cols = 12;
    rows = (n_show + cols - 1) / cols;
    canvas = (unsigned char *)calloc((size_t)cols * cw * rows * ch, 1);
    if (!canvas) { gta_tiles_free(&ti); return 1; }

    printf("remapsheet: ped frame %d (sprite %d, %dx%d), remaps %d..%d\n",
           frame, idx, rec->w, rec->h, lo, hi);

    for (i = 0; i < n_show; i++) {
        int table = (i == 0) ? 0 : lo + i - 1;
        const unsigned char *rmp = ti.remaps
            + (long)table * GTA_TIL_REMAP_STRIDE;
        int ox = (i % cols) * cw + 1, oy = (i / cols) * ch + 1;
        int changed = 0, seen[256];

        memset(seen, 0, sizeof seen);
        for (y = 0; y < (int)rec->h; y++)
        for (x = 0; x < (int)rec->w; x++) {
            unsigned char px = src[(long)y * rec->w + x];
            unsigned char out = px ? rmp[px] : 0;
            if (px && !seen[px]) { seen[px] = 1; if (out != px) changed++; }
            canvas[(long)(oy + y) * (cols * cw) + ox + x] = out;
        }
        if (i > 0 && changed) distinct_total++;
        if (i > 0 && i <= 6)
            printf("  remap %3d: %d of the sprite's own colours moved\n",
                   table, changed);
    }
    printf("remapsheet: %d of %d ped remaps actually recolour this sprite\n",
           distinct_total, hi - lo + 1);

    if (write_bmp8(outBmp, canvas, cols * cw, rows * ch, ti.palette) == 0)
        printf("remapsheet: wrote %s (%dx%d)\n", outBmp, cols * cw, rows * ch);
    free(canvas);
    gta_tiles_free(&ti);
    return distinct_total > 0 ? 0 : 2;
}


/* WHAT HAPPENS TO THE CAR THAT GETS HIT.
 *
 * `ram` measures how far the two bodies get inside each other. This measures
 * the OTHER half: the victim. A real collision should move it, turn it, and
 * take it a moment to recover; a car that is shoved a few pixels and carries
 * on down its lane as though nothing happened is the complaint.
 *
 * The player's car is driven straight at a chosen traffic car on a real map
 * with the real AI running, and every tick after first contact reports the
 * victim's displacement from where it was hit, its change of heading, and its
 * speed.
 */
static int cmd_hitcar(const char *mapPath, const char *tilesPath,
                      int pmodel, int want_speed, int ticks, int lateral,
                      int vmodel, int vface)
{
    gta_map mp;
    gta_tiles ti;
    gta_nav nav;
    gta_veh v;
    static gta_traffic tr;
    int t, victim = -1, contact_at = -1;
    unsigned long victim_serial = 0;
    long vx0 = 0, vy0 = 0;
    int vface0 = 0;
    long best_push = 0, best_along = 0, best_speed = 0;
    int best_turn = 0, moving_again = -1;
    /* THE JITTER COUNTER. A body being resolved smoothly moves the same way
     * on consecutive ticks; one being over-corrected goes out and comes
     * straight back. Inside the impact window, count every tick whose
     * displacement points AGAINST the previous tick's, both above a quarter
     * of a pixel so resting noise does not register. The developer's report
     * is literally this number: "teleportuje w te i we wte na moment". */
    int rev_victim = 0, rev_player = 0, have_last = 0;
    long worst_player_push = 0;         /* px the player was shoved, one tick */
    int  worst_player_at = -1;
    long trace_x = 0, trace_y = 0;      /* the victim, as it was last tick */
    int  trace_f = 0, trace_have = 0;
    long lvx = 0, lvy = 0, lpx = 0, lpy = 0;        /* last positions     */
    long ldvx = 0, ldvy = 0, ldpx = 0, ldpy = 0;    /* last displacements */

    if (gta_map_load(mapPath, &mp) != 0) return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) { gta_map_free(&mp); return 1; }
    if (gta_nav_build(&nav, &mp) != 0) { gta_tiles_free(&ti); gta_map_free(&mp); return 1; }
    if (pmodel < 0 || pmodel >= ti.n_cars) pmodel = 17;
    if (ticks <= 0) ticks = 200;

    /* The northbound lane at x=62 - the same street car_street.txt uses. */
    gta_traffic_init(&tr, &ti, 4242UL);
    gta_traffic_set_nav(&tr, &nav);
    gta_traffic_set_view_blocks(&tr, 10);
    if (vmodel >= 0 && vmodel < ti.n_cars) {
        /* ONE car, of a KNOWN model, parked on the lane. Anything else and the
         * striker's mass is being compared against a different victim each
         * time, which is how "a bus pushes less than a saloon" happened. */
        memset(&tr.cars[0], 0, sizeof tr.cars[0]);
        tr.n = 1;
        tr.cars[0].model = vmodel;
        tr.cars[0].layer = 2;
        tr.cars[0].x = ((long)62 * 32 + 16) << 16;
        tr.cars[0].y = ((long)66 * 32 + 16) << 16;
        /* THE VICTIM'S FACING IS AN ARGUMENT, because a car hit in the FLANK
         * behaves nothing like one hit in the tail and the developer's two
         * reports are both about the flank: "gdy pukne auto na rogu reaguje
         * realistycznie, ale gdy pukne z boku jakby teleportuje sie". 0 is a
         * rear-end shunt, 64 puts the victim broadside across the street. */
        tr.cars[0].face = vface & 255;
        tr.cars[0].angle = vface & 255;
        tr.cars[0].speed = 0;
        tr.cars[0].remap = -1;
        tr.cars[0].serial = 1;
        tr.cars[0].lane_target = 16;
        /* A second victim, one car-length further up the lane. Without
         * something in front of it the shunted car has nothing to hit and
         * "fleet hits 0" says nothing about whether the chain works. */
        memcpy(&tr.cars[1], &tr.cars[0], sizeof tr.cars[0]);
        tr.cars[1].y = tr.cars[0].y
                     - ((long)(gta_car_world_len(&ti.cars[vmodel]) + 4) << 16);
        tr.cars[1].serial = 2;
        tr.n = 2;
    } else {
        gta_traffic_park(&tr, &mp, 62, 66, 4, 12);
    }

    gta_veh_init(&v, &ti, pmodel, ((long)62 * 32 + 16) << 16,
                 ((long)74 * 32 + 16) << 16, 0);
    for (t = 0; t < 4000; t++) {
        long sp;
        gta_veh_step(&v, 1, 0, 0, 0, 1);
        sp = v.vy < 0 ? -v.vy : v.vy;
        if ((sp >> 16) >= want_speed) break;
    }
    /* `lateral` slides the run-up sideways, so the nose arrives on a corner
     * of the victim instead of square on its centre line. */
    v.x = v.ox = (((long)62 * 32 + 16) << 16) + ((long)lateral << 16);
    v.y = v.oy = ((long)74 * 32 + 16) << 16;

    printf("hitcar: %d traffic cars, player model %d (mass %ld) at %ld "
           "px/tick, aiming %d px off centre\n",
           tr.n, pmodel, ti.cars[pmodel].mass >> 16,
           (v.vy < 0 ? -v.vy : v.vy) >> 16, lateral);

    for (t = 0; t < ticks; t++) {
        long rvx, rvy, ryaw, rpx, rpy, sp_, sq_;
        int nh;

        {
            /* THE SAME SWEEP THE GAME DOES - the player's car is stopped at
             * the contact rather than ending the tick inside a fleet car.
             * The harness has to run it too or it measures a game nobody
             * plays (gta_main.c, and gta_traffic_sweep_box). */
            long px0 = v.ox, py0 = v.oy, pa0 = v.ang16;
            long nx_, ny_, na_;
            gta_veh_step(&v, 1, 0, 0, 0, 1);
            nx_ = v.ox; ny_ = v.oy; na_ = v.ang16;
            if (gta_traffic_sweep_box(&tr, px0, py0, pa0, &nx_, &ny_, &na_,
                                      v.len / 2, v.wid / 2, 2)) {
                v.x += nx_ - v.ox;
                v.y += ny_ - v.oy;
                v.ox = nx_; v.oy = ny_; v.ang16 = na_;
            }
        }
        {
            extern int gta_traffic_debug;
            gta_traffic_debug = getenv("GTA_HITCAR_DEBUG") && t > 40 && t < 60;
        }
        if (getenv("GTA_HITCAR_DEBUG") && (t % 5) == 0 && tr.n > 0)
            printf("  dbg t=%3d player (%ld,%ld) v(%ld,%ld) car0 (%ld,%ld) "
                   "gap %ld px\n", t, v.ox >> 16, v.oy >> 16,
                   v.vx >> 16, v.vy >> 16,
                   tr.cars[0].x >> 16, tr.cars[0].y >> 16,
                   ((v.oy - tr.cars[0].y) >> 16));
        sp_ = v.vx < 0 ? -v.vx : v.vx;
        sq_ = v.vy < 0 ? -v.vy : v.vy;
        gta_traffic_set_player(&tr, 1, v.ox, v.oy, sp_ > sq_ ? sp_ : sq_,
                               gta_veh_angle(&v), 2, v.len / 2, v.wid / 2);
        gta_traffic_tick(&tr, &mp, v.ox, v.oy);
        nh = gta_traffic_ram(&tr, v.ox, v.oy, gta_veh_angle(&v),
                             v.len / 2, v.wid / 2, v.vx, v.vy, v.mass, 2,
                             &rvx, &rvy, &ryaw, &rpx, &rpy);
        /* WHAT THE COLLISION DOES TO THE PLAYER'S OWN CAR, in pixels, in the
         * tick it happens. The developer feels this one in his hands:
         * "teleportuje mnie o 10 pikseli w 1 klatce". The victim's jump was
         * measured and fixed; this is the same measurement on the other
         * body, and it is the number that says whether the fix simply moved
         * the fault across. */
        {
            long jp = ((rpx < 0 ? -rpx : rpx) + (rpy < 0 ? -rpy : rpy)) >> 16;
            if (jp > worst_player_push) {
                worst_player_push = jp;
                worst_player_at = t;
            }
        }
        if (rpx || rpy) { v.x += rpx; v.ox += rpx; v.y += rpy; v.oy += rpy; }
        v.vx += rvx; v.vy += rvy;
        v.ang16 = (v.ang16 + ryaw) & 0xFFFFFFL;

        /* WHICH CAR WAS ACTUALLY HIT. "The nearest one" is not the same
         * question and gave a car three lanes away mid-U-turn, which is where
         * a reading of 380 px sideways and 180 degrees came from. The car
         * that was hit is the one whose damage cooldown was set on THIS tick
         * - gta_traffic_ram sets it to GTA_RAM_COOL and nothing else does. */
        if (nh && victim < 0) {
            int i, bi = -1;
            for (i = 0; i < tr.n; i++)
                if (tr.cars[i].ram_cool == GTA_RAM_COOL) { bi = i; break; }
            victim = bi;
            contact_at = t;
            if (victim >= 0) {
                victim_serial = tr.cars[victim].serial;
                vx0 = tr.cars[victim].x;
                vy0 = tr.cars[victim].y;
                vface0 = tr.cars[victim].face;
                printf("hitcar: contact at t=%d with car %d "
                       "(model %d, mass %ld, face %d)\n",
                       t, victim, tr.cars[victim].model,
                       ti.cars[tr.cars[victim].model].mass >> 16, vface0);
            }
        }
        /* THE WINDOW IS THE HIT, not the rest of the journey. Fifteen ticks
         * is long enough for an impulse to have finished doing whatever it is
         * going to do and short enough that ordinary driving cannot dominate
         * the number. */
        /* Re-find it every tick: the fleet is compacted as cars retire. */
        if (victim >= 0) {
            int q;
            victim = -1;
            for (q = 0; q < tr.n; q++)
                if (tr.cars[q].serial == victim_serial) { victim = q; break; }
        }
        /* THE TICK-BY-TICK TRACE, which is the only way to tell a shove from
         * a teleport: a shove moves a pixel or two a tick in one direction, a
         * teleport is one line with eight pixels in it and the next line back
         * where it was. Same for the heading - a car that "turns 350 degrees"
         * has taken the long way round a wrap and it shows up here as one
         * step of +250 instead of -6. */
        /* THE PREVIOUS TICK IS TRACKED FROM THE START, not from the contact.
         * The interesting tick is the one the hit lands on, and a trace that
         * takes its first sample there reports that tick's jump as zero -
         * which is exactly the number under investigation. */
        if (tr.n > 0) {
            const gta_car *c0 = &tr.cars[victim >= 0 ? victim : 0];
            long sx = c0->x >> 16, sy = c0->y >> 16;
            if (contact_at >= 0 && t <= contact_at + 20 && victim >= 0)
                printf("  t%+3d  x %5ld y %5ld  (d %+4ld %+4ld)  face %3d "
                       "(d %+4d)  knock %2d recover %2d  kv %+5ld %+5ld  "
                       "komega %+7ld\n",
                       t - contact_at, sx, sy,
                       trace_have ? sx - trace_x : 0,
                       trace_have ? sy - trace_y : 0,
                       c0->face,
                       trace_have ? (((c0->face - trace_f + 128) & 255) - 128)
                                  : 0,
                       c0->knock, c0->recover,
                       c0->kvx >> 10, c0->kvy >> 10, c0->komega);
            trace_x = sx; trace_y = sy; trace_f = c0->face; trace_have = 1;
        }
        if (victim >= 0 && contact_at >= 0 && t <= contact_at + 15) {
            const gta_car *c = &tr.cars[victim];
            long dx = (c->x - vx0) >> 16, dy = (c->y - vy0) >> 16;
            /* SIDEWAYS, in the victim's own frame at the moment of impact.
             * Along its heading it would have moved anyway; across it is
             * what only a collision does. */
            long fx = gta_sin(vface0), fy = -gta_cos(vface0);
            long push  = ((dx * -fy) + (dy *  fx)) >> 14;   /* across */
            long along = ((dx *  fx) + (dy *  fy)) >> 14;   /* forwards */
            int turn = c->face - vface0;
            if (push < 0) push = -push;
            if (along < 0) along = -along;
            if (along > best_along) best_along = along;
            /* THE SPEED THAT MATTERS IS THE LOOSE ONE.  is the
             * rails speed and is not what moves a knocked-loose car - kvx/kvy
             * are. Reading c->speed made every striker look identical, because
             * the shove-to-4 rule sets exactly that field. */
            {
                long kx = c->kvx < 0 ? -c->kvx : c->kvx;
                long ky = c->kvy < 0 ? -c->kvy : c->kvy;
                long kk = kx > ky ? kx + (ky >> 1) : ky + (kx >> 1);
                /* ONLY the loose velocity. c->speed is the RAILS speed and
                 * does not move a knocked-loose car at all - including it hid
                 * the mass difference behind the flat shove value. */
                if (kk > best_speed) best_speed = kk;
            }
            while (turn > 128) turn -= 256;
            while (turn < -128) turn += 256;
            if (push > best_push) best_push = push;
            if ((turn < 0 ? -turn : turn) > (best_turn < 0 ? -best_turn : best_turn))
                best_turn = turn;
            if (moving_again < 0 && contact_at >= 0 && t > contact_at + 2 &&
                c->speed > (2L << 16))
                moving_again = t - contact_at;
            if (getenv("GTA_HITTICK") && t >= contact_at && t < contact_at + 40)
                printf("  t=%3d victim at (%ld,%ld) push %ld px, face %d "
                       "(%+d), speed %ld.%02ld\n",
                       t, c->x >> 16, c->y >> 16, push, c->face, turn,
                       c->speed >> 16, ((c->speed & 0xFFFF) * 100) >> 16);

            /* Out-and-back, victim and player alike. Three stages: prime the
             * position, prime the displacement, then compare displacements. */
            if (!have_last) {
                lvx = c->x; lvy = c->y; lpx = v.ox; lpy = v.oy;
                have_last = 1;
            } else {
                long dvx2 = c->x - lvx, dvy2 = c->y - lvy;
                long dpx2 = v.ox - lpx, dpy2 = v.oy - lpy;
                if (have_last == 2) {
                    long q, m1, m0;
                    q  = (dvx2 >> 8) * (ldvx >> 8) + (dvy2 >> 8) * (ldvy >> 8);
                    m1 = (dvx2 < 0 ? -dvx2 : dvx2) + (dvy2 < 0 ? -dvy2 : dvy2);
                    m0 = (ldvx < 0 ? -ldvx : ldvx) + (ldvy < 0 ? -ldvy : ldvy);
                    if (q < 0 && m1 > 16384 && m0 > 16384) rev_victim++;
                    q  = (dpx2 >> 8) * (ldpx >> 8) + (dpy2 >> 8) * (ldpy >> 8);
                    m1 = (dpx2 < 0 ? -dpx2 : dpx2) + (dpy2 < 0 ? -dpy2 : dpy2);
                    m0 = (ldpx < 0 ? -ldpx : ldpx) + (ldpy < 0 ? -ldpy : ldpy);
                    if (q < 0 && m1 > 16384 && m0 > 16384) rev_player++;
                }
                ldvx = dvx2; ldvy = dvy2; ldpx = dpx2; ldpy = dpy2;
                lvx = c->x; lvy = c->y; lpx = v.ox; lpy = v.oy;
                have_last = 2;
            }
        }
    }

    /* Traffic hitting traffic, as a knock-on from the player's hit: the car
     * that was rammed carries the impulse into whatever is in front of it. */
    printf("hitcar: fleet hits %ld, cars knocked loose %ld\n",
           tr.stat_fleet_hits, tr.stat_knocked);
    if (victim < 0)
        printf("hitcar: never touched anything - move the start block\n");
    else
        printf("hitcar: 15 ticks after the hit - victim moved %ld px along "
               "and %ld px sideways, turned %+d of 256 (%+d deg), top speed "
               "%ld.%02ld px/tick\n",
               best_along, best_push, best_turn, best_turn * 360 / 256,
               best_speed >> 16, ((best_speed & 0xFFFF) * 100) >> 16);
    if (victim >= 0 || contact_at >= 0)
        printf("hitcar: WORST PUSH ON THE PLAYER - %ld px in one tick "
               "(t=%d)\n", worst_player_push, worst_player_at);
    printf("hitcar: REVERSALS in the window - victim %d, player %d "
               "(0 = smooth, >0 = the out-and-back jump)\n",
               rev_victim, rev_player);

    gta_nav_free(&nav);
    gta_tiles_free(&ti);
    gta_map_free(&mp);
    return 0;
}

static int cmd_drivecar(const char *mapPath, const char *tilesPath,
                        int model, const char *scriptPath, const char *outBmp)
{
    gta_map mp;
    gta_tiles ti;
    gta_nav nav;
    gta_veh v;
    static gta_traffic tr;
    static gta_peds pd;
    FILE *f;
    unsigned char *canvas;
    const int CW = 512, CH = 512, PXB = 4;
    int ox, oy, have_nav = 0, tick = 0, placed = 0;
    long wall_ticks = 0, wall_worst = 0, wall_points = 0;
    long wedged_ticks = 0, wedged_run = 0, wedged_worst = 0;
    long prev_x = 0, prev_y = 0;
    int with_traffic = getenv("GTA_DRIVECAR_TRAFFIC") != 0;
    long rams = 0;
    char line[128];

    if (gta_map_load(mapPath, &mp) != 0)
        return 1;
    if (gta_tiles_load(tilesPath, &ti) != 0) { gta_map_free(&mp); return 1; }
    if (model < 0 || model >= ti.n_cars) {
        fprintf(stderr, "drivecar: model %d of %d\n", model, ti.n_cars);
        return 1;
    }
    if (gta_nav_build(&nav, &mp) == 0)
        have_nav = 1;
    f = fopen(scriptPath, "r");
    if (!f) {
        fprintf(stderr, "drivecar: cannot open %s\n", scriptPath);
        return 1;
    }
    canvas = (unsigned char *)calloc((size_t)CW * CH, 1);
    if (!canvas) return 1;

    ox = oy = 0;
    {
        const gta_car_info *ci = &ti.cars[model];
        printf("drivecar: model %d, %dx%d px, max %d, thrust %ld.%02ld, "
               "mass %ld.%02ld\n",
               model, gta_car_world_wid(ci), gta_car_world_len(ci),
               ci->max_speed,
               ci->thrust >> 16, ((ci->thrust & 0xFFFF) * 100) >> 16,
               ci->mass >> 16, ((ci->mass & 0xFFFF) * 100) >> 16);
    }

    while (fgets(line, sizeof line, f)) {
        int bx, by, ang, n, thr, brk, st, hb, k;
        if (line[0] == '#' || line[0] == ';' || line[0] == '\n')
            continue;
        if (sscanf(line, "start %d %d %d", &bx, &by, &ang) == 3) {
            gta_veh_init(&v, &ti, model,
                         ((long)bx * 32 + 16) << 16,
                         ((long)by * 32 + 16) << 16, ang);
            ox = bx - 64; oy = by - 64;
            if (ox < 0) ox = 0;
            if (oy < 0) oy = 0;
            if (have_nav) {
                int gx, gy;
                for (gy = 0; gy < 128; gy++)
                for (gx = 0; gx < 128; gx++) {
                    unsigned char b = gta_nav_at(&nav, ox + gx, oy + gy, 2);
                    unsigned char col = 0;
                    int g = gta_nav_ground(b);
                    if (g == 1) col = 7;            /* road */
                    else if (g == 2 || g == 3) col = 4;
                    if (col) {
                        int px, py;
                        for (py = 0; py < PXB; py++)
                        for (px = 0; px < PXB; px++)
                            canvas[(long)(gy * PXB + py) * CW
                                   + gx * PXB + px] = col;
                    }
                }
            }
            placed = 1;
            printf("drivecar: start (%d,%d) angle %d\n", bx, by, ang);
            if (with_traffic && have_nav) {
                gta_traffic_init(&tr, &ti, 7UL);
                gta_traffic_set_nav(&tr, &nav);
                gta_traffic_set_view_blocks(&tr, 6);
                printf("drivecar: traffic on - %d parked\n",
                       gta_traffic_park(&tr, &mp, bx, by, 10, 20));
                gta_peds_init(&pd, &ti, 777UL);
                gta_peds_set_nav(&pd, &nav);
            }
            continue;
        }
        if (sscanf(line, "run %d %d %d %d %d", &n, &thr, &brk, &st, &hb) == 5
            && placed) {
            for (k = 0; k < n; k++) {
                long cx, cy, wx0, wy0, wox0, woy0, wang0;
                wx0 = v.x; wy0 = v.y; wox0 = v.ox; woy0 = v.oy;
                wang0 = v.ang16;
                gta_veh_step(&v, thr, brk, st, hb, 0);
                if (have_nav)
                    v.damage += gta_veh_wall(&v, &nav, 2,
                                             wx0, wy0, wox0, woy0, wang0) > 1
                                ? 1 : 0;
                tick++;
                if (with_traffic && have_nav) {
                    long rvx, rvy, ryaw, rpx, rpy, sp_, sq_;
                    int nh;
                    {
                        /* The game's own sweep - stopped at the contact
                         * rather than inside it. See gta_traffic_sweep_box();
                         * without it here the harness measures a game
                         * nobody plays. */
                        long nx_ = v.ox, ny_ = v.oy, na_ = v.ang16;
                        if (gta_traffic_sweep_box(&tr, wox0, woy0, wang0,
                                                  &nx_, &ny_, &na_,
                                                  v.len / 2, v.wid / 2, 2)) {
                            v.x += nx_ - v.ox;
                            v.y += ny_ - v.oy;
                            v.ox = nx_; v.oy = ny_; v.ang16 = na_;
                        }
                    }
                    sp_ = v.vx < 0 ? -v.vx : v.vx;
                    sq_ = v.vy < 0 ? -v.vy : v.vy;
                    gta_traffic_set_player(&tr, 1, v.x, v.y,
                        sp_ > sq_ ? sp_ : sq_,
                        gta_veh_angle(&v), 2, v.len / 2, v.wid / 2);
                    gta_traffic_tick(&tr, &mp, v.x, v.y);
                    gta_peds_tick(&pd, &mp, v.x, v.y);
                    gta_peds_ram(&pd, v.x, v.y, gta_veh_angle(&v),
                                 v.len / 2, v.wid / 2, 2,
                                 sp_ > sq_ ? sp_ + sq_ / 2 : sq_ + sp_ / 2);
                    {
                        int pi_;
                        for (pi_ = 0; pi_ < GTA_MAX_PEDS; pi_++)
                            if (pd.p[pi_].alive && (tick & 3) == 0) {
                                long pcx = ((pd.p[pi_].x >> 16)
                                            - (long)ox * 32) * PXB / 32;
                                long pcy = ((pd.p[pi_].y >> 16)
                                            - (long)oy * 32) * PXB / 32;
                                if (pcx >= 0 && pcx < CW &&
                                    pcy >= 0 && pcy < CH)
                                    canvas[pcy * CW + pcx] =
                                        pd.p[pi_].down ? 12 : 14;
                            }
                    }
                    nh = gta_traffic_ram(&tr, v.ox, v.oy, gta_veh_angle(&v),
                                         v.len / 2, v.wid / 2,
                                         v.vx, v.vy, v.mass, 2,
                                         &rvx, &rvy, &ryaw, &rpx, &rpy);
                    if (rpx || rpy) {
                        v.x += rpx; v.ox += rpx;
                        v.y += rpy; v.oy += rpy;
                    }
                    if (nh) {
                        v.vx += rvx; v.vy += rvy;
                        v.ang16 = (v.ang16 + ryaw) & 0xFFFFFFL;
                        v.damage += nh;
                        rams += nh;
                        if (rams <= 12)
                            printf("t=%4d RAM x%d dv (%ld,%ld) px/t "
                                   "yaw %ld damage %d\n",
                                   tick, nh, rvx >> 16, rvy >> 16,
                                   ryaw >> 16, v.damage);
                    }
                }
                /* WEDGED: the throttle is down and the car did not move a
                 * pixel. A car pressed against a wall it cannot leave shows up
                 * here and nowhere else. */
                if (thr != 0 || brk != 0) {
                    if (((v.ox >> 16) == prev_x) && ((v.oy >> 16) == prev_y)) {
                        wedged_ticks++;
                        if (++wedged_run > wedged_worst) wedged_worst = wedged_run;
                    } else {
                        wedged_run = 0;
                    }
                }
                prev_x = v.ox >> 16;
                prev_y = v.oy >> 16;
                if (have_nav) {
                    int inw = body_in_wall(&nav, 2, v.ox, v.oy,
                                           gta_veh_angle(&v),
                                           v.len / 2, v.wid / 2);
                    if (inw) {
                        wall_ticks++;
                        wall_points += inw;
                        if (inw > wall_worst) wall_worst = inw;
                    }
                }
                cx = ((v.x >> 16) - (long)ox * 32) * PXB / 32;
                cy = ((v.y >> 16) - (long)oy * 32) * PXB / 32;
                if (cx >= 0 && cx < CW && cy >= 0 && cy < CH)
                    canvas[cy * CW + cx] = (tick % 10 == 0) ? 15 : 1;
                if (tick % (getenv("GTA_CARTICK") ? 1 : 25) == 0) {
                    long sp = v.vx, sq = v.vy, mag;
                    if (sp < 0) sp = -sp;
                    if (sq < 0) sq = -sq;
                    mag = sp > sq ? sp + (sq >> 1) : sq + (sp >> 1);
                    printf("t=%4d block (%ld,%ld) px (%ld,%ld) "
                           "heading %3d speed %ld.%02ld px/tick\n",
                           tick, (v.x >> 16) / 32, (v.y >> 16) / 32,
                           v.x >> 16, v.y >> 16, gta_veh_angle(&v),
                           mag >> 16, ((mag & 0xFFFF) * 100) >> 16);
                }
            }
            continue;
        }
    }
    fclose(f);
    if (with_traffic) {
        printf("drivecar: RAMS %ld, player damage %d; peds spawned %ld, run over %ld\n",
               rams, v.damage, pd.stat_spawned, pd.stat_runover);
        /* The two impossible things, on the path where they were reported:
         * a car being hit by the PLAYER. See GTA_SANE_TURN / GTA_SANE_STEP. */
        printf("drivecar: impossible - %ld turns (worst %ld of 256, state %ld), %ld jumps"
               " (worst %ld px); fleet hits %ld, knocked %ld\n",
               tr.stat_face_jump, tr.stat_face_jump_max, tr.stat_face_jump_ctx,
               tr.stat_pos_jump, tr.stat_pos_jump_max,
               tr.stat_fleet_hits, tr.stat_knocked);
    }
    printf("drivecar: BODY IN WALL - %ld of %d ticks, worst %ld of 10 outline "
           "points, %ld point-ticks in all\n",
           wall_ticks, tick, wall_worst, wall_points);
    printf("drivecar: WEDGED - %ld of %d ticks going nowhere under power, "
           "longest run %ld ticks (%ld.%02ld s)\n",
           wedged_ticks, tick, wedged_worst,
           wedged_worst / 50, (wedged_worst % 50) * 2);
    if (write_bmp8(outBmp, canvas, CW, CH, ti.palette) == 0)
        printf("drivecar: wrote %s (%d ticks driven)\n", outBmp, tick);
    free(canvas);
    if (have_nav) gta_nav_free(&nav);
    gta_tiles_free(&ti);
    gta_map_free(&mp);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && strcmp(argv[1], "stats") == 0)
        return cmd_stats(argv[2], argv[3]);

    if (argc >= 5 && strcmp(argv[1], "column") == 0)
        return cmd_column(argv[2], atoi(argv[3]), atoi(argv[4]));

    if (argc >= 9 && strcmp(argv[1], "view") == 0)
        return cmd_view(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]),
                        atoi(argv[6]), atoi(argv[7]), argv[8],
                        argc >= 10 ? atoi(argv[9]) : -1,
                        argc >= 11 ? atoi(argv[10]) : 0);

    if (argc >= 4 && strcmp(argv[1], "narrow") == 0)
        return cmd_narrow(argv[2], argv[3],
                          argc >= 5 ? atoi(argv[4]) : 64,
                          argc >= 6 ? atoi(argv[5]) : 64);

    if (argc >= 4 && strcmp(argv[1], "style") == 0)
        return cmd_style(argv[2], argv[3]);

    if (argc >= 10 && strcmp(argv[1], "map32") == 0)
        return cmd_map32(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]),
                         atoi(argv[6]), atoi(argv[7]), argv[8], argv[9]);

    if (argc >= 4 && strcmp(argv[1], "sprites") == 0)
        return cmd_sprites(argv[2], argv[3]);

    if (argc >= 3 && strcmp(argv[1], "spriteinfo") == 0)
        return cmd_spriteinfo(argv[2]);

    if (argc >= 3 && strcmp(argv[1], "slopes") == 0)
        return cmd_slopes(argv[2], argc >= 4 ? atoi(argv[3]) : 24);

    if (argc >= 7 && strcmp(argv[1], "lights") == 0)
        return cmd_lights(argv[2], atoi(argv[3]), atoi(argv[4]),
                          atoi(argv[5]), atoi(argv[6]),
                          argc >= 8 ? atoi(argv[7]) : 2);

    if (argc >= 4 && strcmp(argv[1], "findtile") == 0)
        return cmd_findtile(argv[2], atoi(argv[3]),
                            argc >= 5 ? atoi(argv[4]) : 12);

    if (argc >= 7 && strcmp(argv[1], "dirmap") == 0)
        return cmd_dirmap(argv[2], atoi(argv[3]), atoi(argv[4]),
                          atoi(argv[5]), atoi(argv[6]),
                          argc >= 8 ? atoi(argv[7]) : 2);

    if (argc >= 7 && strcmp(argv[1], "boxmap") == 0)
        return cmd_boxmap(argv[2], atoi(argv[3]), atoi(argv[4]),
                          atoi(argv[5]), atoi(argv[6]),
                          argc >= 8 ? atoi(argv[7]) : 2);

    if (argc >= 3 && strcmp(argv[1], "overhead") == 0)
        return cmd_overhead(argv[2], argc >= 4 ? atoi(argv[3]) : 20);

    if (argc >= 6 && strcmp(argv[1], "turntrace") == 0)
        return cmd_turntrace(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]),
                             argc >= 7 ? atoi(argv[6]) : 1500,
                             argc >= 8 ? strtoul(argv[7], 0, 10) : 12345UL,
                             argc >= 9 ? atoi(argv[8]) : 3);

    if (argc >= 6 && strcmp(argv[1], "uturns") == 0)
        return cmd_uturns(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]),
                          argc >= 7 ? atoi(argv[6]) : 3000,
                          argc >= 8 ? strtoul(argv[7], 0, 10) : 12345UL);

    if (argc >= 4 && strcmp(argv[1], "drivesweep") == 0)
        return cmd_drivesweep(argv[2], argv[3],
                              argc >= 5 ? atoi(argv[4]) : 600,
                              argc >= 6 ? atoi(argv[5]) : 24,
                              argc >= 7 ? strtoul(argv[6], 0, 10) : 12345UL);

    if (argc >= 4 && strcmp(argv[1], "hitcar") == 0)
        return cmd_hitcar(argv[2], argv[3],
                          argc >= 5 ? atoi(argv[4]) : 17,
                          argc >= 6 ? atoi(argv[5]) : 12,
                          argc >= 7 ? atoi(argv[6]) : 200,
                          argc >= 8 ? atoi(argv[7]) : 0,
                          argc >= 9 ? atoi(argv[8]) : -1,
                          argc >= 10 ? atoi(argv[9]) : 0);
    if (argc >= 4 && strcmp(argv[1], "remapsheet") == 0)
        return cmd_remapsheet(argv[2], argv[3],
                              argc >= 5 ? atoi(argv[4]) : 0);
    if (argc >= 3 && strcmp(argv[1], "ramsweep") == 0)
        return cmd_ramsweep(argv[2], argc >= 4 ? atoi(argv[3]) : 20);
    if (argc >= 5 && strcmp(argv[1], "ram") == 0)
        return cmd_ram(argv[2], atoi(argv[3]), atoi(argv[4]),
                       argc >= 6 ? atoi(argv[5]) : 8,
                       argc >= 7 ? atoi(argv[6]) : 0);
    if (argc >= 7 && strcmp(argv[1], "drivecar") == 0)
        return cmd_drivecar(argv[2], argv[3], atoi(argv[4]), argv[5], argv[6]);

    if (argc >= 7 && strcmp(argv[1], "drive") == 0)
        return cmd_drive(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]),
                         argv[6], argc >= 8 ? atoi(argv[7]) : 500,
                         argc >= 9 ? atoi(argv[8]) : 50,
                         argc >= 10 ? strtoul(argv[9], 0, 10) : 12345UL,
                         argc >= 11 ? atoi(argv[10]) : 0);

    if (argc >= 7 && strcmp(argv[1], "traffic") == 0)
        return cmd_traffic(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]),
                           argv[6], argc >= 8 ? atoi(argv[7]) : 0,
                           argc >= 9 ? strtoul(argv[8], 0, 10) : 12345UL);

    if (argc >= 4 && strcmp(argv[1], "tilecars") == 0)
        return cmd_tilecars(argv[2], argv[3]);

    if (argc >= 3 && strcmp(argv[1], "carinfo") == 0)
        return cmd_carinfo(argv[2], argc >= 4 && strcmp(argv[3], "-v") == 0);
    if (argc >= 3 && strcmp(argv[1], "objinfo") == 0)
        return cmd_objinfo(argv[2]);

    if (argc >= 10 && strcmp(argv[1], "mode") == 0)
        return cmd_mode(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]), argv[6],
                        atoi(argv[7]), atoi(argv[8]), atoi(argv[9]),
                        argc >= 11 ? atoi(argv[10]) : 0);

    if (argc >= 4 && strcmp(argv[1], "spritetest") == 0)
        return cmd_spritetest(argv[2], argv[3],
                              argc >= 5 ? atoi(argv[4]) : -1,
                              argc >= 6 ? atoi(argv[5]) : 0);

    if (argc >= 6 && strcmp(argv[1], "walk") == 0)
        return cmd_walk(argv[2], argv[3], argv[4], argv[5],
                        argc >= 7 ? atoi(argv[6]) : 320,
                        argc >= 8 ? atoi(argv[7]) : 200,
                        argc >= 9 ? atoi(argv[8]) : 0);

    if (argc >= 4 && strcmp(argv[1], "tilesprites") == 0)
        return cmd_tilesprites(argv[2], argv[3],
                               argc >= 5 ? atoi(argv[4]) : -1);

    /* spritedelta <til> <sprite> <out.bmp>
     *
     * The base sprite and every overlay it has, decoded OUT OF THE BAKED FILE
     * by the same code the game will use. The point is to compare it against
     * tools/bin/deltaprobe.py, which decodes the same thing out of the .GRY
     * by a completely separate path in Python: two decoders agreeing is what
     * makes this readable-and-correct rather than merely readable. */
    if (argc >= 5 && strcmp(argv[1], "spritedelta") == 0)
        return cmd_spritedelta(argv[2], atoi(argv[3]), argv[4]);

    if (argc >= 9 && strcmp(argv[1], "map") == 0)
        return cmd_map(argv[2], argv[3], atoi(argv[4]), atoi(argv[5]),
                       atoi(argv[6]), atoi(argv[7]), argv[8]);

    fprintf(stderr,
            "usage:\n"
            "  gtadump style <style.gry> <out-prefix>\n"
            "      Writes <out-prefix>_{side,lid,aux}.bmp contact sheets.\n"
            "\n"
            "  gtadump map <map.cmp> <style.gry> <x> <y> <w> <h> <out.bmp>\n"
            "      Renders a w-by-h tile rectangle of the city from (x,y).\n"
            "\n"
            "  gtadump map32 <map.cmp> <style.gry> <x> <y> <w> <h> <near|avg> <out.bmp>\n"
            "      The same view through the 2:1 downscaler, so the filters\n"
            "      can be compared side by side.\n"
            "\n"
            "  gtadump carinfo <style.gry> [-v]\n"
            "      The car table: dimensions, sprite, mass, handling and\n"
            "      doors for every vehicle. -v prints every field.\n"
            "\n"
            "  gtadump objinfo <style.gry>\n"
            "      The object table: which sprite each object TYPE draws\n"
            "      (0x4a the bullet, 0xd the splat, 0x54 the crate...).\n"
            "\n"
            "  gtadump spriteinfo <style.gry>\n"
            "      The sprite_numbers table: which sprites are pedestrians,\n"
            "      which are cars, and how big each category's frames are.\n"
            "\n"
            "  gtadump walk <map.cmp> <tiles.til> <script.txt> <prefix> [w h zoom]\n"
            "      Drives the PLAYER from a text script and writes one frame\n"
            "      per leg. The script's first line is \"start <bx> <by>\";\n"
            "      every line after it is \"turn forward run ticks\".\n"
            "\n"
            "  gtadump mode <map> <tiles> <bx> <by> <out.bmp> <flat> <sx> <sy> [zoom]\n"
            "      One frame in a chosen render mode: flat 0/1, sx/sy 1 or 2.\n"
            "      Always writes the full 320x200 the player would see.\n"
            "\n"
            "  gtadump spritetest <tiles.til> <out.bmp> [index] [zoom]\n"
            "      One sprite at 16 angles, marched off all four edges -\n"
            "      the rotozoom's clipping regression test.\n"
            "\n"
            "  gtadump tilesprites <tiles.til> <out.bmp> [category]\n"
            "      The same, but read back out of the BAKED file - the\n"
            "      round-trip proof. Category 7 is the pedestrians.\n"
            "\n"
            "  gtadump sprites <style.gry> <out.bmp>\n"
            "      One contact sheet of every sprite in the file.\n"
            "\n"
            "  gtadump view <map.cmp> <tiles.til> <bx> <by> <w> <h> <out.bmp>\n"
            "      ONE FRAME OF THE REAL 2.5D RENDERER, camera on block\n"
            "      (bx,by). Needs the baked tile set from gtabake, not a\n"
            "      .GRY. This is the fast way to see what the Amiga will\n"
            "      draw.\n");
    return 2;
}
