/* The cosine table. Generated, not hand-typed:
 *
 *   python -c "import math;print([round(math.cos(2*math.pi*i/256)*16384) for i in range(256)])"
 *
 * See gta_trig.h for why it is Q14, why a turn is 256 units, and what angle 0
 * means.
 *
 * Licence: MIT (ours).
 */
#include "gta_trig.h"

const short gta_cos_q14[GTA_ANGLE_STEPS] = {
     16384,  16379,  16364,  16340,  16305,  16261,  16207,  16143,
     16069,  15986,  15893,  15791,  15679,  15557,  15426,  15286,
     15137,  14978,  14811,  14635,  14449,  14256,  14053,  13842,
     13623,  13395,  13160,  12916,  12665,  12406,  12140,  11866,
     11585,  11297,  11003,  10702,  10394,  10080,   9760,   9434,
      9102,   8765,   8423,   8076,   7723,   7366,   7005,   6639,
      6270,   5897,   5520,   5139,   4756,   4370,   3981,   3590,
      3196,   2801,   2404,   2006,   1606,   1205,    804,    402,
         0,   -402,   -804,  -1205,  -1606,  -2006,  -2404,  -2801,
     -3196,  -3590,  -3981,  -4370,  -4756,  -5139,  -5520,  -5897,
     -6270,  -6639,  -7005,  -7366,  -7723,  -8076,  -8423,  -8765,
     -9102,  -9434,  -9760, -10080, -10394, -10702, -11003, -11297,
    -11585, -11866, -12140, -12406, -12665, -12916, -13160, -13395,
    -13623, -13842, -14053, -14256, -14449, -14635, -14811, -14978,
    -15137, -15286, -15426, -15557, -15679, -15791, -15893, -15986,
    -16069, -16143, -16207, -16261, -16305, -16340, -16364, -16379,
    -16384, -16379, -16364, -16340, -16305, -16261, -16207, -16143,
    -16069, -15986, -15893, -15791, -15679, -15557, -15426, -15286,
    -15137, -14978, -14811, -14635, -14449, -14256, -14053, -13842,
    -13623, -13395, -13160, -12916, -12665, -12406, -12140, -11866,
    -11585, -11297, -11003, -10702, -10394, -10080,  -9760,  -9434,
     -9102,  -8765,  -8423,  -8076,  -7723,  -7366,  -7005,  -6639,
     -6270,  -5897,  -5520,  -5139,  -4756,  -4370,  -3981,  -3590,
     -3196,  -2801,  -2404,  -2006,  -1606,  -1205,   -804,   -402,
         0,    402,    804,   1205,   1606,   2006,   2404,   2801,
      3196,   3590,   3981,   4370,   4756,   5139,   5520,   5897,
      6270,   6639,   7005,   7366,   7723,   8076,   8423,   8765,
      9102,   9434,   9760,  10080,  10394,  10702,  11003,  11297,
     11585,  11866,  12140,  12406,  12665,  12916,  13160,  13395,
     13623,  13842,  14053,  14256,  14449,  14635,  14811,  14978,
     15137,  15286,  15426,  15557,  15679,  15791,  15893,  15986,
     16069,  16143,  16207,  16261,  16305,  16340,  16364,  16379,
};

/* THE OTHER DIRECTION: a vector back into an angle.
 *
 * The port needed this the moment the driven car stopped being "a heading plus
 * a speed". The original's cars are two points - a front and a rear - each
 * moved along its own heading, with the body angle re-derived from the line
 * between them every frame. That re-derivation is an atan2, and this is
 * ours.
 *
 * RESOLUTION IS THE POINT, so it returns 16.16 rather than a whole step. The
 * original works in 1024 steps; a 256-step answer would quantise every frame's
 * heading to 1.4 degrees, and a car that re-derives its angle from a short
 * baseline every tick turns that into a visibly wobbling, radius-drifting arc.
 * Interpolating inside the table costs one divide and beats 1024 steps.
 *
 * Convention is the port's own (gta_trig.h): 0 is north, 64 east, and the
 * direction vector is (sin, -cos). Pass the vector in screen axes - x right,
 * y DOWN - and the answer comes back in the same units gta_veh keeps its
 * heading in.
 */
static const long gta_atan16[65] = {
           0,    41718,    83416,   125073,   166669,   208185,   249600,   290894,
      332050,   373047,   413869,   454496,   494912,   535100,   575043,   614727,
      654136,   693257,   732076,   770579,   808756,   846595,   884085,   921217,
      957981,   994370,  1030375,  1065990,  1101209,  1136026,  1170436,  1204436,
     1238021,  1271189,  1303938,  1336265,  1368170,  1399652,  1430711,  1461346,
     1491559,  1521350,  1550722,  1579676,  1608214,  1636338,  1664052,  1691359,
     1718262,  1744764,  1770869,  1796582,  1821906,  1846846,  1871405,  1895590,
     1919403,  1942851,  1965938,  1988668,  2011047,  2033080,  2054772,  2076127,
     2097152
};

long gta_dir16(long vx, long vy)
{
    long x = vx, y = -vy;               /* y now points north, like the table */
    long q, rem, a;
    int neg_x = 0, neg_y = 0;

    if (x < 0) { x = -x; neg_x = 1; }
    if (y < 0) { y = -y; neg_y = 1; }
    if (x == 0 && y == 0) return 0;

    /* Keep both inside 15 bits so the interpolation below cannot overflow a
     * 32-bit long: x<<6 stays under 2^21 and delta*rem under 2^31. */
    while (x > 0x7FFFL || y > 0x7FFFL) { x >>= 1; y >>= 1; }

    if (x <= y) {
        q = (x << 6) / y;
        rem = (x << 6) - q * y;
        a = gta_atan16[q];
        if (q < 64) a += ((gta_atan16[q + 1] - gta_atan16[q]) * rem) / y;
    } else {
        q = (y << 6) / x;
        rem = (y << 6) - q * x;
        a = gta_atan16[q];
        if (q < 64) a += ((gta_atan16[q + 1] - gta_atan16[q]) * rem) / x;
        a = (64L << 16) - a;            /* mirror about the 45 degree line */
    }

    /* a is now 0..64 in 16.16, measured from north towards east. */
    if (!neg_x && !neg_y) return a;                     /* north-east */
    if (!neg_x &&  neg_y) return (128L << 16) - a;      /* south-east */
    if ( neg_x &&  neg_y) return (128L << 16) + a;      /* south-west */
    return ((256L << 16) - a) & 0xFFFFFFL;              /* north-west */
}

/* THE TABLE READ AT A FRACTIONAL ANGLE.
 *
 * The two-point car keeps its heading in 16.16 because it re-derives it from
 * a short baseline every tick (gta_dir16 above). Reading the plain table at
 * `angle >> 16` throws that precision away again at the point where it
 * matters most - the wheels - and the symptom is not subtle: the steady turn
 * radius stops being constant and grows as the car slows down, because a
 * slower car's per-tick heading change falls below one whole step and gets
 * rounded away. Measured on the ruler, 256-step wheels gave 29 px at full
 * speed and 35 px at a third of it; the original, with its 1024-step circle,
 * holds one radius at every speed.
 *
 * Linear interpolation between two entries is enough - the table's largest
 * step is 402/16384, so the worst interpolation error is a quarter of one
 * part in a thousand, far below what the ruler can see.
 */
long gta_cos16(long a16)
{
    int i = (int)((a16 >> 16) & 255);
    long f = a16 & 0xFFFFL;
    long c0 = gta_cos_q14[i];
    long c1 = gta_cos_q14[(i + 1) & 255];
    return c0 + (((c1 - c0) * f) >> 16);
}

long gta_sin16(long a16)
{
    return gta_cos16(a16 - (64L << 16));
}
