/* The original's fonts - see gta_font.h.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gta_font.h"

/* Nearest game-palette index to (r,g,b): a plain scan of 256 entries, done
 * 256 times at load and never again. Index 0 is reserved for "clear" - the
 * game palette's own 0 is never a good match anyway (it is the border). */
static void build_remap(unsigned char *remap, const unsigned char *fpal,
                        const unsigned char *gpal)
{
    int i, k;
    remap[0] = 0;
    for (i = 1; i < 256; i++) {
        int r = fpal[i * 3], g = fpal[i * 3 + 1], b = fpal[i * 3 + 2];
        long best = -1;
        int bi = 1;
        for (k = 1; k < 256; k++) {
            long dr = r - gpal[k * 3], dg = g - gpal[k * 3 + 1], db = b - gpal[k * 3 + 2];
            long d = dr * dr + dg * dg + db * db;
            if (best < 0 || d < best) { best = d; bi = k; }
        }
        remap[i] = (unsigned char)bi;
    }
}

int gta_font_load(gta_font *f, const char *path, const unsigned char *palette)
{
    FILE *fp;
    unsigned char hdr[2];
    unsigned char fpal[768];
    unsigned char remap[256];
    unsigned long total = 0, off = 0;
    int i;
    long body_start;

    memset(f, 0, sizeof *f);
    fp = fopen(path, "rb");
    if (!fp) {
        printf("gta_font: cannot open %s\n", path);
        return -1;
    }
    if (fread(hdr, 1, 2, fp) != 2) { fclose(fp); return -1; }
    f->n_chars = hdr[0];
    f->height = hdr[1];
    if (f->n_chars <= 0 || f->height <= 0) { fclose(fp); return -1; }
    f->widths  = (unsigned char *)malloc(f->n_chars);
    f->offsets = (unsigned long *)malloc(f->n_chars * sizeof(unsigned long));
    f->blank   = (unsigned char *)malloc(f->n_chars);
    if (!f->widths || !f->offsets || !f->blank) { fclose(fp); gta_font_free(f); return -1; }

    /* First pass: the widths and the total, so the pixels are one block. */
    body_start = ftell(fp);
    for (i = 0; i < f->n_chars; i++) {
        int w = fgetc(fp);
        if (w < 0) { fclose(fp); gta_font_free(f); return -1; }
        f->widths[i] = (unsigned char)w;
        f->offsets[i] = total;
        total += (unsigned long)w * f->height;
        fseek(fp, (long)w * f->height, SEEK_CUR);
    }
    if (fread(fpal, 1, 768, fp) != 768) { fclose(fp); gta_font_free(f); return -1; }
    f->pixels = (unsigned char *)malloc(total ? total : 1);
    if (!f->pixels) { fclose(fp); gta_font_free(f); return -1; }

    /* Second pass: the glyphs, remapped as they come in. */
    if (palette) build_remap(remap, fpal, palette);
    else for (i = 0; i < 256; i++) remap[i] = (unsigned char)i;
    fseek(fp, body_start, SEEK_SET);
    for (i = 0; i < f->n_chars; i++) {
        unsigned long n = (unsigned long)f->widths[i] * f->height, k;
        unsigned char *g = f->pixels + off;
        fgetc(fp);                      /* the width again */
        if (fread(g, 1, n, fp) != n) { fclose(fp); gta_font_free(f); return -1; }
        for (k = 0; k < n; k++) g[k] = remap[g[k]];
        /* A glyph with no pixel at all: the pager and score fonts carry
         * every lower-case letter that way - the original prints them in
         * capitals, and gta_font_draw does the same. */
        f->blank[i] = 1;
        for (k = 0; k < n; k++) if (g[k]) { f->blank[i] = 0; break; }
        off += n;
    }
    fclose(fp);
    f->space = f->height / 3;
    if (f->space < 2) f->space = 2;
    return 0;
}

void gta_font_free(gta_font *f)
{
    free(f->widths); free(f->offsets); free(f->pixels); free(f->blank);
    f->widths = NULL; f->offsets = NULL; f->pixels = NULL; f->blank = NULL;
    f->n_chars = 0;
}

int gta_font_draw(const gta_font *f, unsigned char *dst, int pitch, int w, int h,
                  int x, int y, const char *s)
{
    for (; *s; s++) {
        int c = (unsigned char)*s - '!';
        /* No lower case in the pager and score fonts (their widths are 0):
         * the original prints those in capitals, and so does this. */
        if (*s >= 'a' && *s <= 'z' && (c >= f->n_chars || f->blank[c]))
            c = (*s - 'a' + 'A') - '!';
        int gw, row, col;
        const unsigned char *g;
        if (c < 0 || c >= f->n_chars) { x += f->space; continue; }
        gw = f->widths[c];
        g = f->pixels + f->offsets[c];
        for (row = 0; row < f->height; row++) {
            int py = y + row;
            unsigned char *d;
            if (py < 0 || py >= h) continue;
            d = dst + (long)py * pitch;
            for (col = 0; col < gw; col++) {
                int px = x + col;
                unsigned char v = g[row * gw + col];
                if (v && px >= 0 && px < w) d[px] = v;
            }
        }
        x += gw + 1;
    }
    return x;
}

int gta_font_width(const gta_font *f, const char *s)
{
    int x = 0;
    for (; *s; s++) {
        int c = (unsigned char)*s - '!';
        /* No lower case in the pager and score fonts (their widths are 0):
         * the original prints those in capitals, and so does this. */
        if (*s >= 'a' && *s <= 'z' && (c >= f->n_chars || f->blank[c]))
            c = (*s - 'a' + 'A') - '!';
        if (c < 0 || c >= f->n_chars) x += f->space;
        else x += f->widths[c] + 1;
    }
    return x;
}
