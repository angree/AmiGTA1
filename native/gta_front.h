/* THE FRONT END - the title screen and the menu, out of the game's own art.
 *
 * WHERE THE ART IS AND HOW IT WAS DECODED. `gtadata` holds a set of files
 * nothing else in this port reads:
 *
 *     f_logo0.rat .. f_logo7.rat    107520 bytes each
 *     f_upper.rat                   107520
 *     f_lower0.rat, f_lower1.rat    199680
 *     f_pal.raw                     768
 *     f_play1..8, f_playn, f_bmg, f_dma
 *
 * and beside every `.rat` a `.raw` of exactly THREE TIMES its size. Nothing
 * in the file says what shape any of them is. Three facts settle it:
 *
 *   1. 768 bytes is a 256-entry RGB palette, so `.rat` is one byte a pixel
 *      into it and `.raw` is the same picture in 24-bit RGB - six bits a
 *      component, as VGA stores it.
 *   2. 107520 = 640 x 168 and 199680 = 640 x 312, and 168 + 312 = 480. The
 *      front end is a 640x480 screen in an UPPER strip and a LOWER one.
 *   3. Rendering f_logo0 at 640x168 shows the words "grand theft auto"
 *      (out/fe_logo0.png). That is the proof, and it is the only kind this
 *      port accepts for a format: a picture somebody looked at.
 *
 * So: eight frames of an animated logo over the top third, a rusty lit
 * background under it, and the menu drawn on top in f_mtext.fon / f_key.fon.
 *
 * THE AMIGA SCREEN IS 320x200, so `gtabake -front` squashes the whole thing
 * at build time - 2:1 across, 480 rows into 200 by nearest row, which puts
 * the logo strip in the top 70 and the background in the other 130. Nearest
 * and not averaged, because the source is PALETTE INDICES and the average of
 * two indices is a colour neither of them was.
 *
 * THE FRONT END HAS ITS OWN PALETTE. It is not the game's, so the screen's
 * is set to f_pal while the menu is up and back to the style file's when the
 * game starts.
 *
 * The baked file:
 *
 *     u8  magic[4]    'G','T','A','F'
 *     u8  version     1
 *     u8  frames      GTA_FRONT_FRAMES
 *     u8  w_hi, w_lo  GTA_FRONT_W
 *     u8  up_h        GTA_FRONT_UP
 *     u8  low_h       GTA_FRONT_LOW
 *     u8  pad[6]
 *     u8  palette[768]                 already scaled from 6 bits to 8
 *     u8  logo[frames][w * up_h]
 *     u8  background[w * low_h]
 *
 * Portable C89, no floats, no Amiga headers - the host tools bake it and
 * the game reads it with the same code. Licence: MIT (ours).
 */
/* The include guard is not GTA_FRONT_H: that name is the picture's HEIGHT
 * three lines down, and having both made every translation unit that reads
 * this file warn about a redefinition. */
#ifndef GTA_FRONT_H_INCLUDED
#define GTA_FRONT_H_INCLUDED

#define GTA_FRONT_W       320
#define GTA_FRONT_H       200
#define GTA_FRONT_UP       70           /* 168 of the source's 480 rows */
#define GTA_FRONT_LOW     130           /* and the other 312 */
#define GTA_FRONT_FRAMES    8
#define GTA_FRONT_HDR      16

/* HOW MANY PALETTE ENTRIES THE SYSTEM KEEPS. Intuition draws the screen
 * title bar and the mouse pointer out of the same 256 entries the picture
 * lives in - the bar from the three pens amigagfx_set_bar_pens() names, the
 * pointer from 17..19 - so the first twenty are set aside and the artwork is
 * quantised into 20..255. Without it the bar came out yellow on black and
 * the pointer took whatever the artwork had put at 17. */
#define GTA_FRONT_RESERVED 20

typedef struct {
    unsigned char pal[768];
    unsigned char *logo;                /* frames * W * UP */
    unsigned char *back;                /* W * LOW */
    int frames, w, up_h, low_h;
    int ok;
} gta_front;

/* Read the baked file. Returns 0 and leaves `ok` set; a missing file is not
 * an error anywhere else in this port and is not one here either - the game
 * simply starts without a title screen. */
int  gta_front_load(gta_front *f, const char *path);
void gta_front_free(gta_front *f);

/* Draw frame `frame` of the logo and the background into an 8-bit buffer
 * that is at least GTA_FRONT_W x GTA_FRONT_H. */
void gta_front_draw(const gta_front *f, unsigned char *dst, int pitch,
                    int x0, int frame);

#endif /* GTA_FRONT_H */
