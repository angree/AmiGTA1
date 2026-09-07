/* The front end's art - see gta_front.h for the format and how it was found.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gta_front.h"

int gta_front_load(gta_front *f, const char *path)
{
    FILE *fp;
    unsigned char hdr[GTA_FRONT_HDR];
    long nlogo, nback;

    memset(f, 0, sizeof *f);
    fp = fopen(path, "rb");
    if (!fp)
        return 1;
    if (fread(hdr, 1, sizeof hdr, fp) != sizeof hdr ||
        hdr[0] != 'G' || hdr[1] != 'T' || hdr[2] != 'A' || hdr[3] != 'F' ||
        hdr[4] != 1) {
        fclose(fp);
        return 1;
    }
    f->frames = hdr[5];
    f->w      = ((int)hdr[6] << 8) | hdr[7];
    f->up_h   = hdr[8];
    f->low_h  = hdr[9];
    if (f->frames <= 0 || f->frames > 16 || f->w <= 0 || f->w > 1024 ||
        f->up_h <= 0 || f->low_h <= 0) {
        fclose(fp);
        return 1;
    }
    if (fread(f->pal, 1, 768, fp) != 768) {
        fclose(fp);
        return 1;
    }
    nlogo = (long)f->frames * f->w * f->up_h;
    nback = (long)f->w * f->low_h;
    f->logo = (unsigned char *)malloc((size_t)nlogo);
    f->back = (unsigned char *)malloc((size_t)nback);
    if (!f->logo || !f->back) {
        gta_front_free(f);
        fclose(fp);
        return 1;
    }
    if ((long)fread(f->logo, 1, (size_t)nlogo, fp) != nlogo ||
        (long)fread(f->back, 1, (size_t)nback, fp) != nback) {
        gta_front_free(f);
        fclose(fp);
        return 1;
    }
    fclose(fp);
    f->ok = 1;
    return 0;
}

void gta_front_free(gta_front *f)
{
    free(f->logo);
    free(f->back);
    f->logo = f->back = 0;
    f->ok = 0;
}

void gta_front_draw(const gta_front *f, unsigned char *dst, int pitch,
                    int x0, int frame)
{
    const unsigned char *src;
    int y;

    if (!f->ok)
        return;
    if (frame < 0 || frame >= f->frames)
        frame = 0;
    src = f->logo + (long)frame * f->w * f->up_h;
    for (y = 0; y < f->up_h; y++)
        memcpy(dst + (long)y * pitch + x0, src + (long)y * f->w,
               (size_t)f->w);
    src = f->back;
    for (y = 0; y < f->low_h; y++)
        memcpy(dst + (long)(f->up_h + y) * pitch + x0,
               src + (long)y * f->w, (size_t)f->w);
}
