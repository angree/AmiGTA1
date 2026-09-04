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

/* The width `s` would take. */
int  gta_font_width(const gta_font *f, const char *s);

#endif /* GTA_FONT_H */
