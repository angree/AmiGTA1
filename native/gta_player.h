/* The player on foot.
 *
 * Portable C89, no floats, no Amiga headers - the same rules as the renderer,
 * and for the same reason: the host tools step this code a thousand ticks in a
 * second, and the emulator is the last place a movement bug should be found.
 *
 * WHAT THIS IS FOR
 * ----------------
 * It is the smallest thing that turns a map viewer into a game, and it forces
 * the three subsystems everything else stands on: collision against the map,
 * sprite drawing, and animation. Cars, pedestrians and the police all reuse
 * exactly these three.
 *
 * COLLISION IS READ OFF THE MAP, NOT GUESSED
 * ------------------------------------------
 * GTA's map carries a 3-bit `ground type` on every block, and - this is the
 * part worth writing down - it is carried on the block the player would
 * OCCUPY, not on the block underneath. Dumping nyc.cmp shows it plainly:
 *
 *   column (62,60), a plain road:  z=1 LID=75 type_map=0000  (the road surface)
 *                                  z=2 no faces, type_map=0021 -> ground ROAD
 *   column (66,62), a building:    z=2,3,4 walls, type_map=0050 -> ground BUILDING
 *
 * So "can I stand in this block?" is one lookup and one shift, and the answer
 * comes from the level designer rather than from an inference about walls. A
 * face-by-face wall test was the obvious alternative and is strictly worse: a
 * building's four walls are all on its own block, so crossing INTO it from a
 * road would have to test the far block's faces anyway, and fences - which are
 * flat plates with no ground type of their own - would need a second rule.
 *
 * Ground types, from Carnage3D's eGroundType (MIT) and confirmed against the
 * columns above: 0 air, 1 water, 2 road, 3 pavement, 4 field, 5 building.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_PLAYER_H
#define GTA_PLAYER_H

#include "gta_map.h"
#include "gta_tiles.h"

/* The 3-bit field at type_map bits 4..6. */
#define GTA_GROUND_AIR       0
#define GTA_GROUND_WATER     1
#define GTA_GROUND_ROAD      2
#define GTA_GROUND_PAVEMENT  3
#define GTA_GROUND_FIELD     4
#define GTA_GROUND_BUILDING  5

/* Where the player is put when a column has nothing walkable in it at all -
 * the street layer. It is 2 for the same reason the renderer's reference grid
 * level is 2: a plain road's lid sits on layer 1, so the block a person
 * standing on that road occupies is layer 2, and the buildings beside it start
 * their walls on layer 2 as well. Both facts come out of nyc.cmp (see the
 * columns quoted above) rather than out of a preference. */
#define GTA_GREF_LAYER 2

#define gta_block_walkable(b) (gta_block_ground_type(b) >= GTA_GROUND_ROAD && \
                               gta_block_ground_type(b) <= GTA_GROUND_FIELD)

/* Frame numbers WITHIN the ped sprite category, from Carnage3D's
 * ped_animations.json (MIT). They are indices into the 295 ped sprites, so the
 * absolute sprite index is gta_tiles_sprite_base(t, 7) + frame. */
#define GTA_PED_WALK_FIRST   0
#define GTA_PED_WALK_FRAMES  8
#define GTA_PED_RUN_FIRST    8
#define GTA_PED_RUN_FRAMES   8
#define GTA_PED_STAND        98
/* The rest of the sheet, same source. Getting in and out of a car needs most
 * of these and nothing had them before. `ENTER_CAR` is not a run of frames -
 * the original holds 26 for three beats, 25 for two, then walks 29..33 - so it
 * is spelled out as a sequence in gta_player.c. */
#define GTA_PED_EXITCAR_FIRST   16
#define GTA_PED_EXITCAR_FRAMES   8
#define GTA_PED_SIT_IN_CAR      97
#define GTA_PED_ENTER_BIKE_FIRST 80
#define GTA_PED_ENTER_BIKE_FRAMES 4
#define GTA_PED_SIT_ON_BIKE     84
#define GTA_PED_EXIT_BIKE_FIRST 85
#define GTA_PED_EXIT_BIKE_FRAMES 4
/* LYING ON THE ROAD IS FRAME 42, not 98. 98 is `standing_still`; a body drawn
 * with it is a person standing up, turned sideways, which is what this port
 * did to everyone it ran over. */
#define GTA_PED_LIES_ON_FLOOR   42
#define GTA_PED_FALL_FIRST      38
#define GTA_PED_FALL_FRAMES      3

typedef enum {
    GTA_ANIM_STAND = 0,
    GTA_ANIM_WALK,
    GTA_ANIM_RUN,
    GTA_ANIM_ENTER_CAR,
    GTA_ANIM_EXIT_CAR
} gta_player_anim;

/* GETTING IN IS NOT A RUN OF FRAMES.
 *
 * The original holds frame 26 for three beats, 25 for two, then walks 29..33 -
 * reach for the handle, pull, drop in. Carnage3D's ped_animations.json spells
 * the same sequence out and this is it, one entry per animation step.
 * Getting OUT is an ordinary run, 16..23. */
#define GTA_PED_ENTER_STEPS 10
extern const unsigned char gta_ped_enter_seq[GTA_PED_ENTER_STEPS];
#define GTA_ENTER_TICKS  4      /* ticks per step: 40 ticks, 0.8 s in all */
#define GTA_EXIT_TICKS   4

typedef struct {
    /* Position in the renderer's own units: 16.16 world pixels at the
     * reference scale, 32 to a block. Sharing units with the camera is what
     * lets the camera simply be told the player's position. */
    long x, y;

    int layer;              /* map layer the player occupies */
    int angle;              /* 0..255, 0 = north, clockwise (gta_trig.h) */

    gta_player_anim anim;
    int frame;              /* index within the animation */
    int frame_tick;         /* ticks spent on this frame */

    int ped_base;           /* first ped sprite in the tile set */
    int ped_count;

    /* Diagnostics, because "he will not walk through that gap" needs a reason
     * and not a guess. Set by the last gta_player_update(). */
    int blocked_x, blocked_y;
    int ground;             /* ground type under the player right now */
} gta_player;

/* Place the player in the middle of block (bx, by), on whatever walkable layer
 * that column has, facing south (angle 128 - down the screen, which is the way
 * the sprite art faces). Returns 0 if the column has no walkable layer at all,
 * in which case the player is still placed and the caller can move him. */
int gta_player_init(gta_player *p, const gta_map *m, const gta_tiles *t,
                    int bx, int by);

/* One tick.
 *
 *   turn     -1 left, 0 straight, +1 right
 *   forward  -1 back, 0 still,    +1 forward
 *   walk     non-zero to WALK; zero, the default, RUNS
 *
 * The default is a run and that is the original's behaviour, not a preference:
 * GTA 1 has no walk key at all, the player is always jogging, and a port that
 * defaults to a walking pace feels broken however correct the physics are.
 * Walking is kept on shift because it is useful for lining up a doorway.
 *
 * A TICK IS NOT A FRAME. The caller runs this at a fixed rate (gta_main.c's
 * SIM_HZ, 50) out of accumulated real time, so the player covers the same
 * ground per second whatever the renderer manages. It used to be one call per
 * rendered frame, which meant he moved faster downtown than over the water and
 * would have run in slow motion on a slower machine. The speeds in
 * gta_player.c are per tick and must be rescaled if SIM_HZ changes. */
void gta_player_update(gta_player *p, const gta_map *m,
                       int turn, int forward, int walk);

/* The absolute sprite index for the player's current pose. */
int gta_player_sprite(const gta_player *p);

/* The grid level the player's feet are on: the plane on top of the layer
 * below him, which is his own layer index. */
#define gta_player_grid(p)  ((p)->layer)

/* WHICH WAY THE ART FACES.
 *
 * GTA's sprites are not drawn pointing along the engine's zero heading, so a
 * heading has to be turned into a rotation before it is drawn. The offset is
 * 128 - half a turn - and it comes from two independent facts:
 *
 *   1. The SHOULDER LINE settles the axis on its own. A ped's standing frame
 *      is 14 wide by 10 tall and the walking frame 14 by 18: the widest part
 *      of a person seen from above is across the shoulders, and it is across
 *      the sprite's X. So the art faces north or south and cannot face east or
 *      west. Buses agree - 51x120, longer than they are wide, along Y.
 *
 *   2. Which of the two is Carnage3D's, whose GameObject.cpp defaults every
 *      object to eSpriteOrientation_S and applies -SPRITE_ZERO_ANGLE, with the
 *      comment "all sprites in game are rotated at 90 degrees". Its heading
 *      zero is east; ours is north; the two conventions differ by exactly the
 *      90 degrees it subtracts, leaving south.
 *
 * Point 1 is a measurement. Point 2 WAS a reading of somebody else's
 * coordinate convention, and therefore a half-turn risk - a pedestrian who
 * moonwalks. **It is now settled, 2026-08-21, three ways that agree:**
 *
 *   a. THE ART ITSELF. Ped frame 89 is shoot_pistol_while_standing, and a
 *      pistol is held in front of the body: in the raw contact sheet the grey
 *      barrel points DOWN the image. Frame 50 (punching, head at the bottom)
 *      and frame 98 (standing still) agree. Frame 42, lies_on_floor, has its
 *      head at the TOP - which is not a contradiction but the same fact seen
 *      from behind: a man facing south who is shot falls BACKWARDS, so his
 *      head ends up north.
 *
 *   b. THE ARITHMETIC. gta_render_sprite's inverse matrix is
 *      [[cs, sn], [-sn, cs]], so the forward rotation is [[cs, -sn], [sn, cs]]
 *      and it sends art-down (0,+1) to screen (-sin a, cos a). The port's
 *      heading convention gives direction (sin A, -cos A), and those are equal
 *      exactly when A = a + 128. Facing = draw angle + half a turn.
 *
 *   c. THE PICTURE. `gtadump spritetest ... 709` places frame 89 at sixteen
 *      compass directions and draws each at the draw angle equal to its own
 *      direction. Every single gun points at the CENTRE of the frame - i.e.
 *      each faces a+128 - which is (b) as a photograph.
 *
 * So draw = heading + 128 is right, and p->angle starts at 128 because south
 * is the heading at which the art is unrotated. Recorded in the notes with
 * the commands. Do not re-derive this; every car and pedestrian uses it. */
#define GTA_SPRITE_ART_SOUTH 128
#define gta_player_draw_angle(p)  (((p)->angle + GTA_SPRITE_ART_SOUTH) & 255)

#endif /* GTA_PLAYER_H */
