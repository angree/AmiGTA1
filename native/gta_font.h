/* THE ORIGINAL'S FONTS - the .FON files: pager1 (the pager line, 12 px),
 * score1 (the digits, 12x11), missmul1 (the multiplier, 7 px), street1
 * (the district names), big1/big2 (the title sizes), cuttext, f_m*.
 *
 * The format (Carnage3D's Font.cpp, checked against every file in the
 * GTADATA): one byte of character count, one of height, then per
 * character one byte of width and width x height palette indices, then a
 * 768-byte palette of the font's own. The first character is '!' (0x21).
 *
 * The font's palette is not the game's: every glyph pixel is remapped ONCE
 * at load to the nearest entry of the game palette the style file carries,
 * so drawing is a plain index copy. Index 0 in a glyph is transparent.
 *
 * Portable C89, no floats, no Amiga headers. Licence: MIT (ours).
 */
#ifndef GTA_FONT_H
#define GTA_FONT_H

typedef struct {
    int n_chars, height;
    unsigned char *widths;      /* n_chars */
    unsigned long *offsets;     /* n_chars, into pixels */
    unsigned char *pixels;      /* remapped to the game palette, 0 = clear */
    unsigned char *blank;       /* n_chars: 1 = the glyph has no pixel at all */
    int space;                  /* the advance of a space, px */
    /* THE DARKEST ENTRY OF THE GAME'S PALETTE, found once at load. It is
     * what a drop shadow is drawn in - see gta_font_draw_shadow. */
    int shadow;
} gta_font;

/* Load `path`; `palette` is the game's 768-byte RGB palette the glyphs are
 * remapped into. Returns 0 on success; the struct is safe to free either
 * way. */
int  gta_font_load(gta_font *f, const char *path, const unsigned char *palette);
void gta_font_free(gta_font *f);

/* Draw `s` at (x,y) into an 8-bit buffer of pitch/w/h, clipped. Returns
 * the x after the last glyph. Characters without a glyph advance a space. */
int  gta_font_draw(const gta_font *f, unsigned char *dst, int pitch, int w, int h,
                   int x, int y, const char *s);

/* THE SAME, CLIPPED to a box - a pager line is scrolled through a hole
 * sixty pixels wide and the rest of it must not be drawn. (It was called
 * without ever being declared: every translation unit that used it did so
 * through an implicit declaration, which the 68k ABI happened to get
 * right.) */
int  gta_font_draw_clip(const gta_font *f, unsigned char *dst, int pitch,
                        int w, int h, int x, int y, const char *s,
                        int cx0, int cy0, int cx1, int cy1);

/* THE SAME, WITH A DROP SHADOW. The string is drawn first one pixel down
 * and to the right in the palette's darkest colour, then again in its own -
 * which is what makes twelve-point green legible over grey pavement and over
 * the pager's own grey device. */
int  gta_font_draw_shadow(const gta_font *f, unsigned char *dst, int pitch,
                          int w, int h, int x, int y, const char *s);
int  gta_font_draw_clip_shadow(const gta_font *f, unsigned char *dst,
                               int pitch, int w, int h, int x, int y,
                               const char *s,
                               int cx0, int cy0, int cx1, int cy1);

/* The width `s` would take. */
int  gta_font_width(const gta_font *f, const char *s);

#endif /* GTA_FONT_H */
