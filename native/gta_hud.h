/* A three-by-five pixel readout drawn straight into the chunky buffer.
 *
 * It exists so the person at the keyboard can see the frame rate while they
 * drive the camera around, instead of waiting for a benchmark to finish and
 * reading a log. Two people watching the number catch things one does not.
 *
 * Portable C89, no Amiga headers, like the rest of the renderer - so the host
 * tools can draw it too.
 *
 * THE FONT IS OURS. A 3x5 digit set is ten glyphs of fifteen bits; writing it
 * is quicker than finding a CC0 one and it leaves nothing to track in
 * LICENSING.md. GTA's own .FON files would be game data we cannot ship, and
 * the reader for them is not written yet.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_HUD_H
#define GTA_HUD_H

/* Pick the ink and shadow colours out of a 768-byte RGB palette: the brightest
 * entry and the darkest. Doing it by search rather than by hard-coded index
 * means the readout stays legible whichever style file is loaded. */
void gta_hud_init(const unsigned char *palette);

/* Draw `s` at (x, y) into an 8-bit buffer, on a one-pixel dark plate so it
 * reads over tarmac and over concrete alike. Understands 0-9, '.', ':', '-',
 * ' ' and the capitals used by the labels; anything else is drawn blank. */
void gta_hud_text(unsigned char *dst, int pitch, int w, int h,
                  int x, int y, const char *s);

/* Format `value_x10` (a number in tenths, so 312 prints as "31.2") into `buf`
 * and return the END of it, so a caller can append. No sprintf - it is broken
 * on this libc (CLAUDE.md) - and no
 * floats, which must never reach the ROM on this target. */
char *gta_hud_tenths(char *buf, long value_x10);

/* Append a plain integer to a buffer position; returns the new end. */
char *gta_hud_int(char *buf, long value);

/* The width in pixels gta_hud_text() will take for `s`, so a caller can put
 * it against the right-hand edge. */
int gta_hud_width(const char *s);

#endif /* GTA_HUD_H */
