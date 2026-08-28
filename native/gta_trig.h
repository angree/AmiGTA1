/* The only trigonometry in the port: a 256-entry cosine table, Q14.
 *
 * WHY A TABLE AND NOT libm
 * ------------------------
 * The target is a plain 68020 with no FPU, and `float * float` must never
 * reach the ROM at all - Kickstart 3.1's mathieeesingbas.library has broken
 * multiply and divide entries on FPU-less machines and answers with a Line-F
 * exception (Guru #8000000B). That is written up in CLAUDE.md as defect 5. So
 * there is no cos() here, no float anywhere, and the table is a constant in
 * the executable rather than something built at startup.
 *
 * 256 DIRECTIONS, not 360 degrees: a full turn is 256 units, so wrapping is
 * `& 255` instead of a modulo, and the sine is the same table read 64 entries
 * along. That is the whole reason for the choice.
 *
 * Q14 (16384 = 1.0) rather than Q16 is also deliberate. Every use multiplies
 * this by a 16.16 scale factor, and Q16 x 16.16 overflows a signed 32-bit int
 * for the scales this renderer actually uses; Q14 leaves two bits of headroom,
 * and 1/16384 of a turn's worth of error is a fortieth of a pixel across the
 * largest sprite in the game.
 *
 * ANGLE CONVENTION, used everywhere in the port:
 *
 *     0   = north (up the screen, -y)     128 = south
 *     64  = east  (+x)                    192 = west
 *
 * so the angle increases CLOCKWISE as seen on screen. Screen y grows downward,
 * which is why the direction vector is (sin, -cos) and not (cos, sin).
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_TRIG_H
#define GTA_TRIG_H

#define GTA_ANGLE_STEPS 256
#define GTA_TRIG_ONE    16384       /* Q14 */

extern const short gta_cos_q14[GTA_ANGLE_STEPS];

#define gta_cos(a)  (gta_cos_q14[(a) & 255])
#define gta_sin(a)  (gta_cos_q14[((a) - 64) & 255])

/* The inverse: a screen-axis vector (x right, y DOWN) back into an angle,
 * returned in 16.16 of the same 256-step circle, 0 .. 0xFFFFFF. Fractional
 * on purpose - the driven car re-derives its heading from a short baseline
 * every tick, and whole steps would quantise that into a wobble. See the
 * comment over the implementation in gta_trig.c. */
long gta_dir16(long vx, long vy);

/* The table read at a FRACTIONAL angle - argument in 16.16 of the same
 * 256-step circle, result Q14 like the table itself. The driven car needs
 * these: with whole-step wheels its turn radius grows as it slows down.
 * See the comment over the implementation. */
long gta_cos16(long a16);
long gta_sin16(long a16);

#endif /* GTA_TRIG_H */
