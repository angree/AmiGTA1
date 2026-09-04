/* GTA 1 ".CMP" city map reader. See gta_map.h for the design note on why the
 * map stays compressed in memory, and for attribution. */

#include <stdlib.h>
#include <string.h>

#include "gta_map.h"

#define GTA_CMP_HEADER_BYTES 28
#define GTA_BLOCK_RECORD_BYTES 8

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

int gta_map_load(const char *path, gta_map *m)
{
    FILE *f = NULL;
    unsigned long route_size, object_pos_size, column_size, block_size;
    unsigned long nav_size, numbers;
    /* Size by the in-memory element, not by the 4 bytes each occupies in the
     * file. `unsigned long` is 32-bit on m68k-amigaos and 64-bit on the Linux
     * host, so getting this wrong overruns the buffer on the host only - which
     * is exactly the sort of bug the host build exists to catch. */
    unsigned long base_bytes =
        (unsigned long)GTA_MAP_DIM * GTA_MAP_DIM * sizeof(unsigned long);
    unsigned char *raw = NULL;
    unsigned long i;

    memset(m, 0, sizeof *m);

    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gta_map: cannot open %s\n", path);
        return -1;
    }

    if (read_u32le(f, &m->version) != 0) goto fail;
    if (m->version != GTA_CMP_VERSION) {
        fprintf(stderr, "gta_map: %s has version %lu, expected %d\n",
                path, m->version, GTA_CMP_VERSION);
        goto fail;
    }
    /* style_number and sample_number share one dword with two reserved bytes. */
    if (read_u32le(f, &numbers) != 0) goto fail;
    m->style_number  = (int)(numbers & 0xff);
    m->sample_number = (int)((numbers >> 8) & 0xff);

    if (read_u32le(f, &route_size) != 0) goto fail;
    if (read_u32le(f, &object_pos_size) != 0) goto fail;
    if (read_u32le(f, &column_size) != 0) goto fail;
    if (read_u32le(f, &block_size) != 0) goto fail;
    if (read_u32le(f, &nav_size) != 0) goto fail;

    if ((column_size % 2UL) != 0UL) {
        fprintf(stderr, "gta_map: column section is %lu bytes, not a whole "
                        "number of 16-bit words\n", column_size);
        goto fail;
    }
    if ((block_size % GTA_BLOCK_RECORD_BYTES) != 0UL) {
        fprintf(stderr, "gta_map: block section is %lu bytes, not a multiple "
                        "of the %d-byte block record\n",
                block_size, GTA_BLOCK_RECORD_BYTES);
        goto fail;
    }

    /* --- base: one byte offset into the column data per (x, y) --- */
    m->base = (unsigned long *)malloc(base_bytes);
    if (!m->base) goto oom;
    for (i = 0; i < (unsigned long)GTA_MAP_DIM * GTA_MAP_DIM; i++) {
        if (read_u32le(f, &m->base[i]) != 0) {
            fprintf(stderr, "gta_map: short read on base table\n");
            goto fail;
        }
    }

    /* --- columns --- */
    m->column_words = column_size / 2UL;
    if (m->column_words) {
        m->columns = (unsigned short *)malloc(column_size);
        if (!m->columns) goto oom;
        raw = (unsigned char *)malloc(column_size);
        if (!raw) goto oom;
        if (fread(raw, 1, column_size, f) != column_size) {
            fprintf(stderr, "gta_map: short read on column data\n");
            goto fail;
        }
        for (i = 0; i < m->column_words; i++)
            m->columns[i] = (unsigned short)(raw[i * 2] | (raw[i * 2 + 1] << 8));
        free(raw);
        raw = NULL;
    }

    /* --- blocks: u16 type_map, u8 type_map_ext, then W E N S Lid --- */
    m->block_count = block_size / GTA_BLOCK_RECORD_BYTES;
    if (m->block_count) {
        m->blocks = (gta_block *)malloc(m->block_count * sizeof(gta_block));
        if (!m->blocks) goto oom;
        raw = (unsigned char *)malloc(block_size);
        if (!raw) goto oom;
        if (fread(raw, 1, block_size, f) != block_size) {
            fprintf(stderr, "gta_map: short read on block data\n");
            goto fail;
        }
        for (i = 0; i < m->block_count; i++) {
            const unsigned char *p = raw + i * GTA_BLOCK_RECORD_BYTES;
            int k;
            m->blocks[i].type_map = (unsigned short)(p[0] | (p[1] << 8));
            m->blocks[i].type_map_ext = p[2];
            for (k = 0; k < GTA_FACE_COUNT; k++)
                m->blocks[i].faces[k] = p[3 + k];
        }
        free(raw);
        raw = NULL;
    }

    fclose(f);
    return 0;

oom:
    fprintf(stderr, "gta_map: out of memory loading %s\n", path);
fail:
    free(raw);
    if (f) fclose(f);
    gta_map_free(m);
    return -1;
}

void gta_map_free(gta_map *m)
{
    if (!m) return;
    free(m->base);
    free(m->columns);
    free(m->blocks);
    m->base = NULL;
    m->columns = NULL;
    m->blocks = NULL;
    m->column_words = 0;
    m->block_count = 0;
}

/* A column record is: [0] = number of empty layers at the bottom, then one
 * block index per occupied layer, stored top-down. So the block for layer z
 * lives at [height - z], counting from the record start. */
static const unsigned short *column_at(const gta_map *m, int x, int y,
                                       int *height_out)
{
    unsigned long elem;
    int height;

    if (!m->base || !m->columns) return NULL;
    if (x < 0 || x >= GTA_MAP_DIM || y < 0 || y >= GTA_MAP_DIM) return NULL;

    elem = m->base[(unsigned long)y * GTA_MAP_DIM + x] / 2UL;
    if (elem >= m->column_words) return NULL;

    height = GTA_MAP_LAYERS - (int)m->columns[elem];
    if (height < 0 || height > GTA_MAP_LAYERS) return NULL;
    if (elem + (unsigned long)height >= m->column_words) return NULL;

    *height_out = height;
    return m->columns + elem;
}

int gta_map_column_height(const gta_map *m, int x, int y)
{
    int height;
    return column_at(m, x, y, &height) ? height : 0;
}

int gta_map_block(const gta_map *m, int x, int y, int z, gta_block *out)
{
    const unsigned short *col;
    int height;
    unsigned short index;

    memset(out, 0, sizeof *out);
    if (z < 0 || z >= GTA_MAP_LAYERS) return 0;

    col = column_at(m, x, y, &height);
    if (!col || z >= height) return 0;

    index = col[height - z];
    if ((unsigned long)index >= m->block_count) return 0;

    *out = m->blocks[index];
    return 1;
}

void gta_map_describe(const gta_map *m, FILE *out)
{
    int x, y, z;
    long occupied = 0;
    int max_height = 0;

    fprintf(out, "version        %lu\n", m->version);
    fprintf(out, "style number   %d  (expects style%03d.gry)\n",
            m->style_number, m->style_number);
    fprintf(out, "sample number  %d\n", m->sample_number);
    fprintf(out, "column words   %lu\n", m->column_words);
    fprintf(out, "blocks         %lu distinct\n", m->block_count);

    for (y = 0; y < GTA_MAP_DIM; y++) {
        for (x = 0; x < GTA_MAP_DIM; x++) {
            int h = gta_map_column_height(m, x, y);
            if (h > max_height) max_height = h;
            for (z = 0; z < h; z++) occupied++;
        }
    }
    fprintf(out, "occupied cells %ld of %ld  (tallest column %d)\n",
            occupied, (long)GTA_MAP_DIM * GTA_MAP_DIM * GTA_MAP_LAYERS,
            max_height);
}

/* --- ramps ---------------------------------------------------------------
 *
 * See gta_map.h for what the slope field holds and how the direction was
 * settled. These moved out of gta_player.c when the driven car needed them
 * too; the code is that file's, unchanged.
 */
int gta_map_slope_up_dir(const gta_map *m, int bx, int by, int z)
{
    gta_block b;
    int s;

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM)
        return -1;
    if (z < 0 || z >= GTA_MAP_LAYERS)
        return -1;
    if (!gta_map_block(m, bx, by, z, &b))
        return -1;

    s = gta_block_slope(&b);
    if (s == 0)  return -1;
    if (s <= 2)  return 0;      /* 26 degrees, north */
    if (s <= 4)  return 128;
    if (s <= 6)  return 192;
    if (s <= 8)  return 64;
    if (s <= 16) return 0;      /* 7 degrees, eight steps */
    if (s <= 24) return 128;
    if (s <= 32) return 192;
    if (s <= 40) return 64;
    if (s == 41) return 0;      /* 45 degrees */
    if (s == 42) return 128;
    if (s == 43) return 192;
    if (s == 44) return 64;
    return -1;
}

int gta_map_slope_is_top(const gta_map *m, int bx, int by, int z)
{
    gta_block b;
    int s;

    if (bx < 0 || bx >= GTA_MAP_DIM || by < 0 || by >= GTA_MAP_DIM) return 0;
    if (z < 0 || z >= GTA_MAP_LAYERS) return 0;
    if (!gta_map_block(m, bx, by, z, &b)) return 0;

    s = gta_block_slope(&b);
    if (s == 2 || s == 4 || s == 6 || s == 8)       return 1;  /* 26 degrees */
    if (s == 16 || s == 24 || s == 32 || s == 40)   return 1;  /* 7 degrees  */
    if (s >= 41 && s <= 44)                         return 1;  /* 45, one block */
    return 0;
}

int gta_map_step_dir(long dx, long dy)
{
    long ax = dx < 0 ? -dx : dx;
    long ay = dy < 0 ? -dy : dy;
    if (ax >= ay) return dx >= 0 ? 64 : 192;
    return dy >= 0 ? 128 : 0;
}
