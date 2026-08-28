/* Packing a vehicle definition into the baked .til, and back out again.
 *
 * BOTH DIRECTIONS LIVE IN ONE FILE ON PURPOSE. The writer runs on the host in
 * gtabake and the reader runs on the Amiga in gta_tiles.c, so a field written
 * in one order and read in another would produce a table of plausible nonsense
 * with nothing to catch it - a car with a bus's mass and a boat's sprite. Kept
 * side by side, the two lists are checked by reading them next to each other,
 * and `gta_car_pack` / `gta_car_unpack` are a round trip the tools can assert
 * on. That is the same reasoning that made the .GRY walk check the section
 * length to the byte.
 *
 * Big-endian, because the Amiga reads this file every time the game starts and
 * the host writes it once, in a build tool. See gta_tiles.h.
 *
 * Licence: MIT (ours).
 */
#include <string.h>

#include "gta_car.h"
#include "gta_tiles.h"

/* GTA_TIL_CARREC is a number in a header and the pack below is a list of
 * assignments; nothing but this makes them agree. A negative array size is a
 * compile error on both compilers, so the file will not build if a field is
 * added without the constant being changed. */
typedef char gta_car_rec_size_check[
    (22 + GTA_CAR_REMAPS + 4 + 8 + 2 + 32 + 6 + 8 + 6 + 2
     + GTA_CAR_DOORS * 8 + 4) == GTA_TIL_CARREC ? 1 : -1];

static void put_be16(unsigned char *p, int v)
{
    p[0] = (unsigned char)((v >> 8) & 0xFF);
    p[1] = (unsigned char)(v & 0xFF);
}

static void put_be32(unsigned char *p, long v)
{
    p[0] = (unsigned char)((v >> 24) & 0xFF);
    p[1] = (unsigned char)((v >> 16) & 0xFF);
    p[2] = (unsigned char)((v >> 8) & 0xFF);
    p[3] = (unsigned char)(v & 0xFF);
}

static short get_be16(const unsigned char *p)
{
    int v = (p[0] << 8) | p[1];
    return (short)(v < 0x8000 ? v : v - 0x10000);
}

static long get_be32(const unsigned char *p)
{
    unsigned long v = ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16)
                    | ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
    return (v < 0x80000000UL) ? (long)v : -(long)(0xFFFFFFFFUL - v) - 1L;
}

/* The two lists below must stay in the same order as each other. They are
 * deliberately written out longhand rather than driven from a table of field
 * offsets: a table would need offsetof and a type tag per field, which is more
 * machinery than 30 lines of assignments and hides the one thing that has to
 * be checked by eye. */
void gta_car_pack(const gta_car_info *c, unsigned char *rec)
{
    int i;
    unsigned char *p = rec;

    memset(rec, 0, GTA_TIL_CARREC);

    put_be16(p, c->width);      p += 2;
    put_be16(p, c->length);     p += 2;
    put_be16(p, c->vert);       p += 2;
    put_be16(p, c->sprite_num); p += 2;
    put_be16(p, c->weight);     p += 2;
    put_be16(p, c->max_speed);  p += 2;
    put_be16(p, c->min_speed);  p += 2;
    put_be16(p, c->accel);      p += 2;
    put_be16(p, c->braking);    p += 2;
    put_be16(p, c->grip);       p += 2;
    put_be16(p, c->handling);   p += 2;

    memcpy(p, c->remap8, GTA_CAR_REMAPS); p += GTA_CAR_REMAPS;

    *p++ = c->vtype;
    *p++ = c->model_id;
    *p++ = (unsigned char)c->turning;
    *p++ = (unsigned char)c->damagable;

    for (i = 0; i < 4; i++) { put_be16(p, c->value[i]); p += 2; }

    *p++ = (unsigned char)c->cx;
    *p++ = (unsigned char)c->cy;

    put_be32(p, c->moment);             p += 4;
    put_be32(p, c->mass);               p += 4;
    put_be32(p, c->thrust);             p += 4;
    put_be32(p, c->tyre_adhesion_x);    p += 4;
    put_be32(p, c->tyre_adhesion_y);    p += 4;
    put_be32(p, c->handbrake_friction); p += 4;
    put_be32(p, c->footbrake_friction); p += 4;
    put_be32(p, c->front_brake_bias);   p += 4;

    put_be16(p, c->turn_ratio);            p += 2;
    put_be16(p, c->drive_wheel_offset);    p += 2;
    put_be16(p, c->steering_wheel_offset); p += 2;

    put_be32(p, c->back_end_slide);  p += 4;
    put_be32(p, c->handbrake_slide); p += 4;

    *p++ = c->convertible;
    *p++ = c->engine;
    *p++ = c->radio;
    *p++ = c->horn;
    *p++ = c->sound_function;
    *p++ = c->fast_change;

    put_be16(p, c->n_doors); p += 2;
    /* All four door slots are always written, so the record is indexable. */
    for (i = 0; i < GTA_CAR_DOORS; i++) {
        put_be16(p, c->doors[i].rpy);    p += 2;
        put_be16(p, c->doors[i].rpx);    p += 2;
        put_be16(p, c->doors[i].object); p += 2;
        put_be16(p, c->doors[i].delta);  p += 2;
    }

    put_be32(p, (long)c->sprite_index); p += 4;
}

void gta_car_unpack(gta_car_info *c, const unsigned char *rec)
{
    int i;
    const unsigned char *p = rec;

    memset(c, 0, sizeof *c);

    c->width      = get_be16(p); p += 2;
    c->length     = get_be16(p); p += 2;
    c->vert       = get_be16(p); p += 2;
    c->sprite_num = get_be16(p); p += 2;
    c->weight     = get_be16(p); p += 2;
    c->max_speed  = get_be16(p); p += 2;
    c->min_speed  = get_be16(p); p += 2;
    c->accel      = get_be16(p); p += 2;
    c->braking    = get_be16(p); p += 2;
    c->grip       = get_be16(p); p += 2;
    c->handling   = get_be16(p); p += 2;

    memcpy(c->remap8, p, GTA_CAR_REMAPS); p += GTA_CAR_REMAPS;

    c->vtype     = *p++;
    c->model_id  = *p++;
    c->turning   = (signed char)(*p < 0x80U ? (int)*p : (int)*p - 256); p++;
    c->damagable = (signed char)(*p < 0x80U ? (int)*p : (int)*p - 256); p++;

    for (i = 0; i < 4; i++) { c->value[i] = get_be16(p); p += 2; }

    c->cx = (signed char)(*p < 0x80U ? (int)*p : (int)*p - 256); p++;
    c->cy = (signed char)(*p < 0x80U ? (int)*p : (int)*p - 256); p++;

    c->moment             = get_be32(p); p += 4;
    c->mass               = get_be32(p); p += 4;
    c->thrust             = get_be32(p); p += 4;
    c->tyre_adhesion_x    = get_be32(p); p += 4;
    c->tyre_adhesion_y    = get_be32(p); p += 4;
    c->handbrake_friction = get_be32(p); p += 4;
    c->footbrake_friction = get_be32(p); p += 4;
    c->front_brake_bias   = get_be32(p); p += 4;

    c->turn_ratio            = get_be16(p); p += 2;
    c->drive_wheel_offset    = get_be16(p); p += 2;
    c->steering_wheel_offset = get_be16(p); p += 2;

    c->back_end_slide  = get_be32(p); p += 4;
    c->handbrake_slide = get_be32(p); p += 4;

    c->convertible    = *p++;
    c->engine         = *p++;
    c->radio          = *p++;
    c->horn           = *p++;
    c->sound_function = *p++;
    c->fast_change    = *p++;

    c->n_doors = get_be16(p); p += 2;
    if (c->n_doors < 0 || c->n_doors > GTA_CAR_DOORS)
        c->n_doors = 0;
    for (i = 0; i < GTA_CAR_DOORS; i++) {
        c->doors[i].rpy    = get_be16(p); p += 2;
        c->doors[i].rpx    = get_be16(p); p += 2;
        c->doors[i].object = get_be16(p); p += 2;
        c->doors[i].delta  = get_be16(p); p += 2;
    }

    c->sprite_index = (int)get_be32(p); p += 4;
}
