/* The GTA vehicle DEFINITION - one record per model, straight out of the
 * style file's car table.
 *
 * Portable C89, no floats, no Amiga headers: the same rules as the renderer.
 * It lives in its own header because BOTH sides need it - `gta_style.c` reads
 * it out of a .GRY on the host, and `gta_tiles.c` reads it back out of the
 * baked .til on the Amiga, which never sees a .GRY at all.
 *
 * This is the DEFINITION of a model, not an instance of one. A car driving
 * around the city is a `gta_car`, further down, and points at one of these.
 *
 * Licence: MIT (ours). Attribution for the field order: Carnage3D, MIT.
 */
#ifndef GTA_CAR_H
#define GTA_CAR_H

/* THE CAR TABLE - 6972 bytes in style001.gry that were skipped until traffic
 * needed them. One variable-length record per vehicle: 174 fixed bytes and
 * then 8 bytes per door.
 *
 * THE GIFT IN THIS SECTION: the physics constants are stored as 32-bit
 * FIXED POINT, 16.16, exactly the format this port uses everywhere. Carnage3D
 * reads them with `READ_FIXEDF32`, which is `int32 / 65536.0f` - it converts
 * away from the representation we want. We keep the raw int32 and there is no
 * conversion, no float, and no loss. Mass, thrust, tyre adhesion and the brake
 * frictions come out of the file ready to multiply.
 *
 * WHERE THE LAYOUT COMES FROM: Carnage3D's StyleData::ReadVehicles (MIT), read
 * for the field ORDER only. It reads the 24-bit .G24 files and this port reads
 * 8-bit .GRY, and those two disagree about the SPRITE record - which is why
 * `gta_style_load` checks that the walk consumes car_size to the byte instead
 * of trusting the layout. If a future style file does not land exactly, the
 * layout is wrong and the loader says so rather than reading garbage.
 *
 * ONE DELIBERATE DIFFERENCE FROM Carnage3D. It reads the twelve HLS remaps
 * (12 x 3 x i16 = 72 bytes) and SKIPS the twelve bytes after them; for a
 * .GRY those twelve bytes are the 8-bit palette remaps, which is precisely
 * what an AGA port wants and what the HLS triples are useless for. So this
 * does the opposite: skips the 72 and keeps the 12.
 */
/* Model ids worth naming. The full list is Carnage3D's eVehicleModel; only the
 * ones this port has to treat specially live here. */
#define GTA_MODEL_HELICOPTER 88

/* THE TABLE'S DIMENSIONS ARE IN SOURCE PIXELS, 64 TO A BLOCK. THE WORLD IS 32.
 *
 * gta_render.h says it plainly - "a sprite lives at SOURCE scale, 64 pixels to
 * a block, while a tile is baked at 32" - so a 120-pixel bus is drawn just
 * under two blocks long. The traffic code took the file's number as world
 * pixels and thought the same bus was FOUR blocks long, and every symptom
 * followed from that: buses that could not fit anywhere and parked themselves
 * in dead ends, following gaps of three blocks between saloons, and corners
 * the drive test called off-road because a vehicle twice its real length
 * cannot sweep them.
 *
 * These two are therefore the only lengths anything outside the file reader
 * should use. The struct keeps the file's own numbers, because that is what
 * the struct is for and what `gtadump carinfo` prints. */
#define gta_car_world_len(ci)  ((ci)->length >> 1)
#define gta_car_world_wid(ci)  ((ci)->width  >> 1)

#define GTA_CAR_REMAPS 12
#define GTA_CAR_DOORS  4

/* vtype, as stored. The gaps are real - the file uses 0..4, 8, 9, 13, 14. */
typedef enum {
    GTA_VEH_BUS        = 0,
    GTA_VEH_JUGG_FRONT = 1,
    GTA_VEH_JUGG_BACK  = 2,
    GTA_VEH_BIKE       = 3,
    GTA_VEH_CAR        = 4,
    GTA_VEH_TRAIN      = 8,
    GTA_VEH_TRAM       = 9,
    GTA_VEH_BOAT       = 13,
    GTA_VEH_TANK       = 14
} gta_vehicle_class;

typedef struct {
    short rpy, rpx;                 /* hinge, relative to the car's centre */
    short object;
    short delta;                    /* which sprite delta opens this door */
} gta_car_door;

typedef struct gta_car_info {
    /* Dimensions in world pixels, in the file's own order.
     *
     * BEWARE THE NAMES. Carnage3D reads these three into variables called
     * width / height / depth and then builds its vector as {width, depth,
     * height} - so its second field is the one on the GROUND and its third is
     * the vertical, the opposite of what its names suggest. Copying those
     * names into this struct would have made every car collide as though it
     * were turned sideways.
     *
     * The file settles it. The second field equals the vehicle's SPRITE
     * HEIGHT, to the pixel, right across the table: car 62 against a 50x64
     * sprite, bus 120 against 51x120, train 125 against 40x125. And the third
     * is 10 for every saloon and 24 for every bus, which is a car being 10
     * pixels tall and a bus 24. So:
     *
     *     length  runs along the sprite's long axis - the way it points
     *     vert    is how tall it stands, and no ground collision uses it
     *
     * The road footprint is therefore width x length. */
    short width, length, vert;

    short sprite_num;               /* index WITHIN this class's category */
    short weight;
    short max_speed, min_speed;
    short accel, braking;
    short grip, handling;

    unsigned char remap8[GTA_CAR_REMAPS];   /* the 8-bit palette remaps */

    unsigned char vtype;            /* gta_vehicle_class */
    unsigned char model_id;
    signed char   turning;
    signed char   damagable;

    short value[4];
    signed char cx, cy;             /* centre of mass, pixels */
    long moment;                    /* moment of inertia */

    /* 16.16 fixed point, straight out of the file. */
    long mass, thrust;
    long tyre_adhesion_x, tyre_adhesion_y;
    long handbrake_friction, footbrake_friction, front_brake_bias;

    short turn_ratio, drive_wheel_offset, steering_wheel_offset;

    long back_end_slide, handbrake_slide;   /* 16.16 */

    unsigned char convertible;      /* bit 0 convertible, bit 1 extra anim */
    unsigned char engine, radio, horn, sound_function, fast_change;

    short n_doors;
    gta_car_door doors[GTA_CAR_DOORS];

    /* Resolved by the loader: sprite_num counts within the class's own
     * category, so this is the absolute index into st->sprites and is the
     * only one a renderer can use. -1 if the class is unknown. */
    int sprite_index;
} gta_car_info;

/* Serialise one definition to / from the baked .til's fixed-size record.
 * `rec` is GTA_TIL_CARREC bytes (gta_tiles.h). Both live in gta_car.c so the
 * two field orders sit next to each other and can be checked by eye. */
void gta_car_pack(const gta_car_info *c, unsigned char *rec);
void gta_car_unpack(gta_car_info *c, const unsigned char *rec);

/* WHICH PANEL A HIT LANDED ON. The impact point in world pixels against a car
 * at (cx,cy) facing `face` gives one of the six body panels - front, middle
 * or back, left or right - as a delta index (gta_tiles.h, GTA_DELTA_DMG_*).
 * The original dents the panel that was struck rather than the whole car. */
int gta_car_panel_delta(const gta_car_info *ci, long cx, long cy, int face,
                        long hx, long hy);

#endif /* GTA_CAR_H */
