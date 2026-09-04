/* The on-screen readout. See gta_hud.h for why it exists and why the font is
 * ours rather than downloaded.
 *
 * Licence: MIT (ours).
 */
#include "gta_hud.h"

#define GW 3            /* glyph width  */
#define GH 5            /* glyph height */

/* One byte per row, the low three bits used, top row first. Written out rather
 * than packed cleverly: at 55 bytes there is nothing to save, and a glyph you
 * can read in the source is a glyph you can fix without a tool. */
static const unsigned char glyph[][GH] = {
    { 7, 5, 5, 5, 7 },   /* 0 */
    { 2, 6, 2, 2, 7 },   /* 1 */
    { 7, 1, 7, 4, 7 },   /* 2 */
    { 7, 1, 7, 1, 7 },   /* 3 */
    { 5, 5, 7, 1, 1 },   /* 4 */
    { 7, 4, 7, 1, 7 },   /* 5 */
    { 7, 4, 7, 5, 7 },   /* 6 */
    { 7, 1, 1, 1, 1 },   /* 7 */
    { 7, 5, 7, 5, 7 },   /* 8 */
    { 7, 5, 7, 1, 7 },   /* 9 */
    { 0, 0, 0, 0, 2 },   /* .  index 10 */
    { 0, 2, 0, 2, 0 },   /* :  index 11 */
    { 0, 0, 7, 0, 0 },   /* -  index 12 */
    { 0, 0, 0, 0, 0 },   /* space, index 13 */
    { 7, 4, 7, 4, 4 },   /* F  index 14 */
    { 7, 5, 7, 4, 4 },   /* P  index 15 */
    { 7, 4, 7, 1, 7 },   /* S  index 16 */
    { 7, 5, 5, 5, 5 },   /* N  index 17 (no diagonal at this size) */
    { 5, 5, 5, 5, 7 },   /* U  index 18 */
    { 7, 4, 4, 4, 7 },   /* C  index 19 */
    { 5, 5, 7, 5, 5 },   /* H  index 20 */
    { 7, 2, 2, 2, 2 },   /* T  index 21 */
    { 7, 1, 2, 4, 7 },   /* Z  index 22 */
    { 7, 5, 5, 5, 7 },   /* O  index 23 */
    { 5, 5, 2, 5, 5 },   /* X  index 24 */
    /* THE REST OF THE ALPHABET, added when the score readout arrived: a
     * weapon called PISTOL cannot be spelled out of F P S N U C H T Z O X.
     * Same 3x5 cell, same one-glyph-per-line table. */
    { 7, 5, 7, 5, 5 },   /* A  index 25 */
    { 6, 5, 6, 5, 6 },   /* B  index 26 */
    { 6, 5, 5, 5, 6 },   /* D  index 27 */
    { 7, 4, 6, 4, 7 },   /* E  index 28 */
    { 7, 4, 5, 5, 7 },   /* G  index 29 */
    { 7, 2, 2, 2, 7 },   /* I  index 30 */
    { 5, 5, 6, 5, 5 },   /* K  index 31 */
    { 4, 4, 4, 4, 7 },   /* L  index 32 */
    { 5, 7, 7, 5, 5 },   /* M  index 33 */
    { 7, 5, 7, 6, 5 },   /* R  index 34 */
    { 5, 5, 5, 5, 2 },   /* V  index 35 */
    { 5, 5, 7, 7, 5 },   /* W  index 36 */
    { 5, 5, 2, 2, 2 }    /* Y  index 37 */
};

static unsigned char hud_ink = 255;
static unsigned char hud_shadow;

void gta_hud_init(const unsigned char *palette)
{
    int i, best_bright = -1, best_dark = -1;
    long bright = -1, dark = -1;

    for (i = 0; i < 256; i++) {
        long v = (long)palette[i * 3] + palette[i * 3 + 1] + palette[i * 3 + 2];
        if (bright < 0 || v > bright) { bright = v; best_bright = i; }
        if (dark < 0 || v < dark) { dark = v; best_dark = i; }
    }
    hud_ink = (unsigned char)(best_bright < 0 ? 255 : best_bright);
    hud_shadow = (unsigned char)(best_dark < 0 ? 0 : best_dark);
}

static int glyph_index(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    switch (c) {
    case '.': return 10;
    case ':': return 11;
    case '-': return 12;
    case ' ': return 13;
    case 'F': return 14;
    case 'P': return 15;
    case 'S': return 16;
    case 'N': return 17;
    case 'U': return 18;
    case 'C': return 19;
    case 'H': return 20;
    case 'T': return 21;
    case 'Z': return 22;
    case 'O': return 23;
    case 'X': return 24;
    case 'A': return 25;
    case 'B': return 26;
    case 'D': return 27;
    case 'E': return 28;
    case 'G': return 29;
    case 'I': return 30;
    case 'K': return 31;
    case 'L': return 32;
    case 'M': return 33;
    case 'R': return 34;
    case 'V': return 35;
    case 'W': return 36;
    case 'Y': return 37;
    default:  return 13;
    }
}

void gta_hud_text(unsigned char *dst, int pitch, int w, int h,
                  int x, int y, const char *s)
{
    int len = 0, bw, bh, by, bx;
    const char *p;

    for (p = s; *p; p++)
        len++;
    if (len <= 0)
        return;

    /* The plate: one pixel of margin all round, so the digits read over tarmac
     * and over pale concrete without changing colour. */
    bw = len * (GW + 1) + 1;
    bh = GH + 2;
    for (by = y - 1; by < y - 1 + bh; by++) {
        unsigned char *d;
        if (by < 0 || by >= h) continue;
        d = dst + (long)by * pitch;
        for (bx = x - 1; bx < x - 1 + bw; bx++)
            if (bx >= 0 && bx < w)
                d[bx] = hud_shadow;
    }

    for (p = s; *p; p++) {
        const unsigned char *g = glyph[glyph_index(*p)];
        int row;
        for (row = 0; row < GH; row++) {
            int py = y + row;
            unsigned char *d;
            int col;
            if (py < 0 || py >= h) continue;
            d = dst + (long)py * pitch;
            for (col = 0; col < GW; col++) {
                int px = x + col;
                if (px < 0 || px >= w) continue;
                if (g[row] & (1 << (GW - 1 - col)))
                    d[px] = hud_ink;
            }
        }
        x += GW + 1;
    }
}

void gta_hud_text_big(unsigned char *dst, int pitch, int w, int h,
                      int x, int y, const char *s, int scale)
{
    int len = 0, bw, bh, by, bx;
    const char *p;
    if (scale < 1) scale = 1;
    for (p = s; *p; p++) len++;
    if (len <= 0) return;
    bw = (len * (GW + 1) + 1) * scale;
    bh = (GH + 2) * scale;
    for (by = y - scale; by < y - scale + bh; by++) {
        unsigned char *d;
        if (by < 0 || by >= h) continue;
        d = dst + (long)by * pitch;
        for (bx = x - scale; bx < x - scale + bw; bx++)
            if (bx >= 0 && bx < w) d[bx] = hud_shadow;
    }
    for (p = s; *p; p++) {
        const unsigned char *g = glyph[glyph_index(*p)];
        int row, col, sy, sx;
        for (row = 0; row < GH; row++)
            for (col = 0; col < GW; col++) {
                if (!(g[row] & (1 << (GW - 1 - col)))) continue;
                for (sy = 0; sy < scale; sy++) {
                    int py = y + row * scale + sy;
                    unsigned char *d;
                    if (py < 0 || py >= h) continue;
                    d = dst + (long)py * pitch;
                    for (sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        if (px >= 0 && px < w) d[px] = hud_ink;
                    }
                }
            }
        x += (GW + 1) * scale;
    }
}

int gta_hud_width_big(const char *s, int scale)
{
    return gta_hud_width(s) * (scale < 1 ? 1 : scale);
}

/* The cop's head: a peaked cap over a face with two eyes. Bit 6 is the left
 * column. Seven wide, eight tall, and the plate is a pixel round it. */
static const unsigned char cop_glyph[8] = {
    0x1C,   /* ..XXX.. */
    0x3E,   /* .XXXXX. */
    0x7F,   /* XXXXXXX  the peak */
    0x3E,   /* .XXXXX. */
    0x2A,   /* .X.X.X.  the eyes are holes */
    0x3E,   /* .XXXXX. */
    0x1C,   /* ..XXX.. */
    0x1C    /* ..XXX..  the collar */
};

void gta_hud_cop(unsigned char *dst, int pitch, int w, int h, int x, int y)
{
    int by, bx, row, col;
    for (by = y - 1; by < y + 9; by++) {
        unsigned char *d;
        if (by < 0 || by >= h) continue;
        d = dst + (long)by * pitch;
        for (bx = x - 1; bx < x + 8; bx++)
            if (bx >= 0 && bx < w)
                d[bx] = hud_shadow;
    }
    for (row = 0; row < 8; row++) {
        int py = y + row;
        unsigned char *d;
        if (py < 0 || py >= h) continue;
        d = dst + (long)py * pitch;
        for (col = 0; col < 7; col++) {
            int px = x + col;
            if (px < 0 || px >= w) continue;
            if (cop_glyph[row] & (1 << (6 - col)))
                d[px] = hud_ink;
        }
    }
}

/* How wide gta_hud_text() will draw `s`, plate included. A caller that wants
 * the text against the RIGHT edge needs this: the score is right-aligned in
 * the original and a left-aligned one jumps sideways as the digits grow. */
int gta_hud_width(const char *s)
{
    int len = 0;
    const char *p;
    for (p = s; *p; p++) len++;
    return len * (GW + 1) + 1;
}

char *gta_hud_int(char *buf, long value)
{
    char tmp[12];
    int n = 0;

    if (value < 0) { *buf++ = '-'; value = -value; }
    do {
        tmp[n++] = (char)('0' + (int)(value % 10));
        value /= 10;
    } while (value && n < (int)sizeof tmp);
    while (n)
        *buf++ = tmp[--n];
    *buf = 0;
    return buf;
}

/* Returns the END of what it wrote, like gta_hud_int, so callers can append.
 *
 * It used to return the start of the buffer, which is a perfectly reasonable
 * thing for a formatter to do and was wrong here: the caller appended " FPS"
 * at the returned pointer and so overwrote its own number. The readout showed
 * a tidy " FPS" with nothing in front of it. */
char *gta_hud_tenths(char *buf, long value_x10)
{
    char *p;

    if (value_x10 < 0) value_x10 = 0;
    p = gta_hud_int(buf, value_x10 / 10);
    *p++ = '.';
    *p++ = (char)('0' + (int)(value_x10 % 10));
    *p = 0;
    return p;
}
