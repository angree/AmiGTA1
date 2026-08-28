/* The DRIVEN vehicle - the physics a player's hands will feel.
 *
 * THIS IS THE ORIGINAL'S OWN MODEL, in fixed point. Two independent passes
 * over the original's driving code agreed field for field and
 * constant for constant; the working is in the notes.
 *
 * WHAT THE ORIGINAL ACTUALLY DOES, and why the port's first attempt felt
 * nothing like it:
 *
 *   GTA 1 HAS TWO MOVEMENT MODELS. `car+0x138` selects between them. The
 *   cheap one is a two-point integer bicycle - a front point and a rear
 *   point, each moved along its own heading, the body angle
 *   re-derived from the line between them. Every AI traffic car drives with
 *   it. **The player's car never does.** Getting into a car sets `car+4 = 1`
 *   and `car+0x138 = 1`, and from then on the car runs the float model:
 *
 *   A RIGID BODY WITH TWO TYRES. One mass, one moment of inertia, a centre of
 *   mass, and exactly two contact points - the steering wheel at
 *   `steering_wheel_offset` and the drive wheel at `drive_wheel_offset`, both
 *   on the centreline. Each tyre resists the velocity OF ITS OWN CONTACT
 *   POINT with a force proportional to that velocity: longitudinally by
 *   `tyre_adhesion_x`, laterally by `tyre_adhesion_y`. The forces are summed
 *   into a force and a torque about the centre of mass, divided by mass and
 *   inertia, and integrated with a semi-implicit Euler step at dt = 1.
 *
 *   THE CAR TURNS BECAUSE THE FRONT TYRE PULLS SIDEWAYS AT A LEVER ARM.
 *   There is no yaw rate, no turn radius, no steering-to-rotation formula
 *   anywhere in the original. That is what the port had, and why a corner
 *   felt like a turntable rather than a car.
 *
 *   THE THREE NUMBERS THAT ARE THE FEEL OF GTA 1:
 *     - the REAR tyre's coefficients are multiplied by 1.5 and the front's
 *       are not, so the back axle always has half again the grip of the front
 *       - that is the game's gentle, forgiving understeer;
 *     - the handbrake replaces that 1.5 on the rear's LATERAL coefficient
 *       with 0.6, a 2.5x collapse of rear grip against an unchanged front -
 *       that, and nothing else, is the handbrake turn;
 *     - the steering wheel is an ABSOLUTE world heading that winds on at
 *       `turn_ratio` DEGREES PER STEP while the key is held, stops at a 50
 *       degree lock, and unwinds at about 8 degrees a step when released.
 *       Because it is absolute, the car rotating under it unwinds the
 *       steering for free, which is what makes a corner settle instead of
 *       tightening.
 *
 * UNITS. The DOS game works in 1/64 of a map block (one source-art pixel) and
 * this port works in 1/32 (see gta_car.h), so every LENGTH halves and the
 * moment of inertia quarters. That conversion happens once, in gta_veh_init.
 *
 * THE CLOCK IS NOT CONVERTED. The original steps at 23.333 Hz and so does
 * this, on an accumulator inside gta_veh_step - the caller still calls it
 * every 50 Hz tick and most of those return immediately. The reason is in
 * gta_vehphys.c over TIME_NUM: the model's yaw damping is 1.037 per step,
 * deliberately just over one, and that is a property of the discrete step
 * rather than of any differential equation. Rescale it and the car stops
 * snapping to its steering and starts floating.
 *
 * The port's 256-step circle replaces the original's radians, and every angle
 * is kept in 16.16 of it because the model integrates small angular
 * accelerations and whole steps would quantise them away.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_VEHPHYS_H
#define GTA_VEHPHYS_H

#include "gta_tiles.h"

typedef struct {
    /* state - a rigid body */
    long x, y;              /* CENTRE OF MASS, world 16.16 px */
    long vx, vy;            /* world 16.16 px per PHYSICS STEP */
    long ang16;             /* heading, 16.16 of the 256-step circle */
    long omega;             /* 16.16 angle units per step */
    long steer16;           /* the steered wheel's ABSOLUTE heading, 16.16 */
    int  accum;             /* 50 Hz ticks banked towards the next 23.333 Hz step */
    /* Consecutive ticks in which the world refused BOTH axes - i.e. the car is
     * pressed into something and going nowhere. Past GTA_WALL_STUCK it gets
     * eased back out; see gta_veh_wall(). */
    int  wall_stuck;

    /* what the rest of the port reads: the car's geometric centre, which is
     * the centre of mass shifted back along the body by the com offset. The
     * original keeps both for the same reason - the art is drawn about the
     * centre, the physics acts about the mass. */
    long ox, oy;            /* world 16.16 px */

    /* the model, resolved once by gta_veh_init */
    int  model;             /* index into tiles->cars */
    /* The paint it was wearing when the player got in, so putting it back in
     * the street puts back the same car. An index into the model's own
     * remap8 table, or -1 for the sprite's own colours. */
    int  remap;
    int  model_id;          /* the table's own id - what the original's
                             * special cases are keyed on, NOT the index */
    int  len, wid;          /* world px */
    long vmax, vmin;        /* 16.16 px/step - only the tank is capped */
    long mass;              /* raw 16.16 table mass, for collisions */
    int  damage;
    long skid;              /* |sum of the axles' lateral forces|, this step */
    int  sliding;           /* the game's own "lay tyre marks" test */
    long skid_level;        /* what `skid` has to beat, scaled by this mass */

    /* tyre coefficients already divided by mass, so a force becomes an
     * acceleration with one multiply. Q16, per step. */
    long k_adh_x, k_adh_y;  /* rolling and lateral grip */
    long k_hand;            /* the handbrake's extra longitudinal friction */
    long k_reverse;         /* reverse acceleration, 16.16 px per step^2 */
    long k_thrust;          /* engine acceleration, 16.16 px per step^2 */

    long r_front, r_rear;   /* lever arms from the COM, 16.16 world px */
    long com_y;             /* centre of mass along the body, 16.16 world px */
    long k_inertia;         /* mass / moment, Q16 - turns a torque into alpha */
    long turn_rate;         /* steering wind-on, 16.16 angle units per step */
} gta_veh;

/* Place a vehicle of `model` at world (x,y) 16.16, heading `angle` 0..255. */
void gta_veh_init(gta_veh *v, const gta_tiles *t, int model,
                  long x, long y, int angle);

/* Call this every 50 Hz tick. It banks the tick and runs a physics step on
 * every second or third one, at the original's 23.333 Hz.
 * throttle/brake/handbrake are 0 or 1, steer -1/0/+1 -
 * digital, because the Amiga's arrow keys are. No world collision here - the
 * caller owns the map and decides what a wall does.
 *
 * `on_road` is the one thing the model cannot work out for itself: whether
 * the block under the car is a road (nav ground type 2). When it is, and the
 * player is going forwards above a walking pace with their hands off the
 * wheel, the original magnetises the car onto the nearest cardinal - see the
 * road-snap block at the end of gta_veh_step. Pass 0 and the assist simply
 * never fires, which is what the original does off-road. */
void gta_veh_step(gta_veh *v, int throttle, int brake, int steer,
                  int handbrake, int on_road);

/* The heading as the rest of the port speaks it. */
#define gta_veh_angle(v)  ((int)(((v)->ang16 >> 16) & 255))

/* How long a car may sit against a wall going nowhere under power before it is
 * eased back out. Half a second at 50 Hz: long enough that an ordinary
 * nose-to-wall stop is undisturbed, short enough that "it sticks to buildings
 * and jams" is not.
 *
 * The original does the equivalent with a unit force applied at each corner
 * standing in a solid block, a corner push-out of magnitude 1 while the car
 * is moving and 2 while it is stopped. Because that
 * force is at a corner it also rotates the car out, which is why GTA 1 cars
 * squirm free of walls instead of sitting in them. */
#define GTA_WALL_STUCK   25

/* THE BODY AGAINST THE WORLD - walls, water, anything a car may not stand on.
 *
 * Call it straight after gta_veh_step with the position and heading the car
 * had BEFORE that step. It returns the impact speed in whole world pixels per
 * tick (0 if nothing was hit), which is what the caller charges as bodywork.
 *
 * `nav` is a `const gta_nav *`, declared void here so this header does not
 * have to drag gta_nav.h in behind it. */
int gta_veh_wall(gta_veh *v, const void *nav, int layer,
                 long x0, long y0, long ox0, long oy0, long ang0);

#endif /* GTA_VEHPHYS_H */
