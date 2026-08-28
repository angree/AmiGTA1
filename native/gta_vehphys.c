/* The driven vehicle's physics - the original's rigid body, in fixed point.
 * gta_vehphys.h explains what the model is and where it came from.
 *
 * EVERYTHING HERE IS 16.16 AND THE PORT'S Q14 TRIG. The original does this
 * in x87 floats; floats must never reach this machine's ROM (CLAUDE.md defect
 * 5), and they are not needed - the handling table is already 16.16 on disk,
 * positions are 16.16, and the only transcendentals are three sin/cos pairs a
 * tick, which the port's table already serves.
 *
 * THE ONE CONVERSION, applied in gta_veh_init and nowhere else: LENGTH. The
 * DOS game measures in source-art pixels, 64 to a block; this port's world is
 * 32 to a block (gta_car.h). Every length, offset and velocity therefore
 * halves, and the moment of inertia - mass times length squared - quarters.
 *
 * TIME IS NOT CONVERTED. The model runs at the original's own 23.333 Hz, on
 * an accumulator inside this module, for the reason set out over TIME_NUM
 * below: its yaw damping is a property of the discrete step and rescaling it
 * makes a different car.
 *
 * Licence: MIT (ours).
 */
#include "gta_vehphys.h"
#include "gta_nav.h"
#include "gta_trig.h"
#include "gta_car.h"

/* ---- the two scale conversions ------------------------------------------
 *
 * TIME. The DOS game's simulation rate is 23.333 Hz and this is READ, not
 * guessed: the main loop waits until a tick counter reaches 3, and that
 * counter is incremented by an interrupt handler installed at 70 Hz (PIT
 * divisor 1193182/70).
 * Three ticks of 70 Hz is 42.86 ms, so one game frame is 23.333 Hz and every
 * per-tick constant in the handling table is per 1/23.333 s. Against this
 * port's 50 Hz that is 7/15.
 *
 * (The vertical-retrace wait in the blit path is a CEILING of 70 Hz on top of
 * this, not the rate itself - which is why one of the four passes over the
 * binary reported "no limiter, 70 Hz". The limiter is in the main loop, not
 * the blitter.)
 *
 * LENGTH halves - 64 source pixels to a block against this port's 32 - and it
 * appears inline as a >>1 or a /2 where it is used. */
#define TIME_NUM   7            /* the original's 23.333 Hz ... */
#define TIME_DEN   15           /* ... over our 50 */

/* AND THE MODEL IS NOT RESCALED TO OUR RATE - IT IS RUN AT THE ORIGINAL'S.
 *
 * This looks like extra work and it is the opposite. The yaw damping in this
 * model is `sum(arm^2 * lateral_grip) / moment`, which for a standard saloon
 * is (30^2*0.94 + 20^2*1.41)/1360 = 1.037 PER STEP - just over one. A step
 * therefore not only kills the car's rotation, it slightly reverses it: the
 * car snaps onto whatever angle the front tyre asks for, within one step,
 * with no wobble and no carry. That snap is what GTA 1 steering feels like.
 *
 * That number is a property of the DISCRETE step, not of a differential
 * equation being approximated. Run the same constants twice as often and the
 * damping per step halves to 0.52: the car keeps rotating after the wheel is
 * straight, and it feels floaty and oversteery - a different car. Run it half
 * as often and it is over 2, and the rotation oscillates.
 *
 * So the physics steps at 23.333 Hz on its own accumulator and the port's
 * 50 Hz tick just drives it. Seven fifteenths of a step per tick: a step
 * every second or third tick, and dt stays exactly 1 where the original had
 * it. Only LENGTHS are converted, because those are a property of the world,
 * not of the clock. */

/* ---- the original's own constants, converted ---------------------------- */

/* One of the table's speed units in 16.16 world px per PHYSICS STEP: one
 * source pixel is half a world pixel.
 *
 * THE PLAYER'S CAR IS NOT SPEED-CAPPED, and this was worth an hour to settle
 * because two of the four readings of the original said it was. There IS a
 * `if (thrust > 0 && |speed| > max_speed) thrust = 0` in the original, but a
 * branch above it jumps over it for every model except 0x25, the tank. So
 * ordinary cars never meet it.
 *
 * A car's top speed is therefore purely the balance of engine against rolling
 * resistance: `gear1 / (adhesion_x * (1 + 1.5))`, which for a saloon is 42.9
 * source px a step = 21.4 world px a step = 500 world px a second = 15.6
 * blocks a second. `max_speed` in the table steers the AI and is what a
 * parked car is given at spawn; it does not restrain the player. Two cars
 * with the same engine and the same tyres are therefore equally fast whatever
 * the table says - and models 0 and 4 really are identical in every physical
 * field, which is a thing to remember before "fixing" them. */
#define VEH_SPEED_UNIT   32768L         /* half a world px, in 16.16 */
#define VEH_TANK_MODEL   0x25

/* The steering lock, +-0.873 rad (50.02 degrees) from the body's own heading
 * (the original tests it through cos < 0.642). In this port's 256-step circle
 * that is 35.57 units. NOT scaled by time - it is an
 * angle, not a rate. */
#define VEH_STEER_LOCK   2331278L       /* 35.57 in 16.16 */

/* Releasing the wheel unwinds it at 0.139 rad (7.96 degrees) a tick, but only
 * once it is more than 0.139 rad off centre - the original tests sin(d)
 * against the same number it then subtracts, so the dead-band and the rate
 * are the same angle. Both are per step, and the step is the original's. */
#define VEH_CENTRE_BAND  371150L        /* 5.66 units in 16.16 */
#define VEH_CENTRE_RATE  371150L        /* the same angle, once a step */


/* The road-snap assist, all four of its numbers. The original works in
 * 1024ths of a turn and tests `rotation & 0xff` - the position within the
 * current quadrant - so these are the same tests in 256ths, 16.16.
 *   BAND   0x20 of 1024 = 11.25 degrees = 8 of our units
 *   EXACT  the original's `== 0`, which at its resolution is a quarter of
 *          one of our steps
 *   NUDGE  0.0278 rad = 1.593 degrees = 1.133 of our units, per step
 *   SPEED  the original's `speed > 6`, six source px a step = three of ours */
#define VEH_SNAP_BAND    524288L        /* 8 units in 16.16 */
#define VEH_SNAP_EXACT   16384L         /* a quarter of a unit */
#define VEH_SNAP_NUDGE   74244L         /* 1.133 units in 16.16 */
#define VEH_SNAP_SPEED   196608L        /* 3 world px a step */


/* The three numbers that are the feel of the game, hard-coded in the original
 * exactly like this - the table's `back end slide value` and `handbrake slide
 * value` are read from the file and then never used.
 *
 * REAR_GRIP multiplies BOTH of the rear tyre's coefficients, always. The
 * front's are used raw. So the back axle carries half again the grip of the
 * front, and the car understeers gently - that is GTA 1's forgiving feel.
 *
 * HAND_SLIDE REPLACES it on the rear's lateral coefficient while the
 * handbrake is down and the footbrake is not: 1.5 becomes 0.6, a 2.5x
 * collapse of rear grip against an unchanged front. That is the handbrake
 * turn, and there is nothing else to it. */
#define VEH_REAR_GRIP    98304L         /* 1.5 in Q16 */
#define VEH_HAND_SLIDE   39322L         /* 0.6 in Q16 */
/* Model 9, the bus, gets 3.0 for both - huge grip and no handbrake slide. */
#define VEH_BUS_GRIP     196608L        /* 3.0 in Q16 */
#define VEH_BUS_MODEL    9

/* The skid threshold, 20.0 of the original's force units. Theirs is a real
 * force - mass times acceleration -
 * and ours is already divided by mass and halved once for the length scale,
 * so the equivalent test on our number is `lateral > 20 / (2 * mass)`. That
 * depends on the vehicle, so gta_veh_init works it out once into skid_level;
 * for a mass-10 saloon it comes to 1.0.*/
#define VEH_SKID_NUM     655360L        /* 10.0 in 16.16, to be divided by mass */

/* THERE IS NO FOOT BRAKE IN GTA 1, and this is not a simplification - it is
 * what the original does. Its driving model contains a complete footbrake
 * model (the front axle gets `footbrake_friction * front_brake_bias`, the
 * rear gets the remainder), and `footbrake friction` and `front brake bias`
 * are in the car table for every vehicle. But the byte that turns it on is
 * never
 * written: the flag has three references in the whole original - two that
 * clear it and the one that reads it. The branch is dead, the two table
 * fields are dead data.
 *
 * What the player calls braking is the REVERSE GEAR, engaged instantly with
 * no stop-first rule: the engine simply pushes backwards at half force, and
 * the car decelerates through zero and keeps going. That is where GTA 1's
 * famously long, mushy stopping distance comes from - about five blocks from
 * speed - and reproducing it means NOT adding a brake the original lacks. */
#define VEH_REVERSE_NUM  1
#define VEH_REVERSE_DEN  2
/* ... except a motorcycle, which reverses at 0.15 instead of 0.5. */
#define VEH_REV_BIKE_NUM 15
#define VEH_REV_BIKE_DEN 100
#define VEH_CLASS_BIKE   3

/* ---- fixed-point helpers ------------------------------------------------
 *
 * Each one exists because the naive expression overflows a signed 32-bit long
 * on the machine that matters. The shifts are chosen to keep the SMALL factor
 * intact: a tyre coefficient is a few thousand in Q16, and >>8 would round it
 * to noise, so it is always the large factor that gets shifted. */

/* k (Q16, under 65536) times v (16.16, under ~2.5M) -> 16.16.
 * The VELOCITY is the one that gets shifted, because it is the large one. */
#define KMUL(k, v)      ((((k) * ((v) >> 8))) >> 8)

/* Q16 times Q16, BOTH SMALL -> Q16. KMUL is wrong for this and it cost a
 * measurement to notice: scaling a tyre coefficient of 917 by the rear grip
 * multiplier 1.5 through KMUL shifts the 917 down to 3 first and returns
 * 1152 instead of 1375 - 16% of the car's rear grip, quietly missing, which
 * showed up as a top speed 10% too high and a handbrake that spun the car
 * nearly twice as far as the original's. Shift the MULTIPLIER instead: it is
 * a small whole-ish number and loses nothing in four bits. */
#define CMUL(a, b)      (((((a) >> 4) * (b))) >> 12)

/* r (16.16, under ~1.5M) times a (16.16, under ~500k) -> 16.16 */
#define RMUL(r, a)      ((((r) >> 8) * ((a) >> 4)) >> 4)

/* the angular acceleration: k_inertia (Q16, under ~130k) times a torque */
#define IMUL(k, t)      (((((k) >> 4) * ((t) >> 8))) >> 4)

/* a Q14 unit vector component times a 16.16 length -> 16.16 */
#define UMUL(u, len)    ((((u) * ((len) >> 8))) >> 6)

/* project a 16.16 vector onto a Q14 unit vector -> 16.16 */
#define VDOT(vx, vy, ux, uy)  (((((vx) >> 7) * (ux)) + (((vy) >> 7) * (uy))) >> 7)

/* The shortest way round the circle, in 16.16 of 256 steps. */
static long ang_diff(long a, long b)
{
    long d = (a - b) & 0xFFFFFFL;
    if (d > (128L << 16)) d -= (256L << 16);
    return d;
}

static long isqrt16(long v)
{
    long r = 0, b = 1L << 30;
    if (v <= 0) return 0;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

static long speed_of(const gta_veh *v)
{
    long sx = v->vx >> 8, sy = v->vy >> 8;
    return isqrt16(sx * sx + sy * sy) << 8;
}

void gta_veh_init(gta_veh *v, const gta_tiles *t, int model,
                  long x, long y, int angle)
{
    const gta_car_info *ci = &t->cars[model];
    long m = ci->mass > 0 ? ci->mass : (10L << 16);
    long m8 = m >> 8;                   /* mass in Q8, for the divisions */
    long mom = ci->moment > 0 ? ci->moment : 1360L;
    long cy;

    if (m8 < 1) m8 = 1;

    v->model = model;
    v->model_id = ci->model_id;
    v->remap = -1;
    v->len = gta_car_world_len(ci);
    v->wid = gta_car_world_wid(ci);
    if (v->len < 8) v->len = 8;

    v->ang16 = (long)(angle & 255) << 16;
    v->omega = 0;
    v->steer16 = v->ang16;              /* wheels straight ahead */
    v->vx = v->vy = 0;
    v->damage = 0;
    v->mass = m;

    /* The centre of mass sits `cy` source pixels along the body from the
     * geometric centre - always backwards, a few pixels, on every stock
     * vehicle. The caller gave us the centre it wants the car drawn at, so
     * the body's own position starts offset from it. */
    cy = ((long)ci->cy << 16) / 2;      /* source px -> world 16.16 */
    v->com_y = cy;
    v->ox = x; v->oy = y;
    v->x = x + UMUL(gta_sin16(v->ang16), cy);
    v->y = y - UMUL(gta_cos16(v->ang16), cy);

    /* The speed cap. The original clamps the ENGINE, not the velocity: above
     * max_speed the thrust simply stops, and drag brings the car back. */
    v->vmax = (long)ci->max_speed * VEH_SPEED_UNIT;
    v->vmin = (long)ci->min_speed * VEH_SPEED_UNIT;     /* negative already */

    /* THE TYRE COEFFICIENTS, pre-divided by mass so that a force becomes an
     * acceleration with one multiply. Each is a per-tick rate, so each takes
     * the time scale once. `(c * 3 / 5) << 8 / (m >> 8)` keeps the full
     * fractional mass - dividing by a whole-number mass would be 1.7% out on
     * the heavier saloons. */
#define COEF(c)  ((((c)) << 8) / m8)
    v->k_adh_x = COEF(ci->tyre_adhesion_x);
    v->k_adh_y = COEF(ci->tyre_adhesion_y);
    v->k_hand  = COEF(ci->handbrake_friction);
#undef COEF

    /* Engine acceleration: a length per step squared, so it takes the length
     * scale once. A saloon's thrust 15 over mass 10 becomes 0.75 world px per
     * step squared, which against its own rolling resistance settles at 21.4
     * px a step. */
    v->k_thrust = (((ci->thrust / 2) << 8) / m8);

    /* Reverse is the same engine backwards at half force - a seventh for a
     * motorcycle, which is why bikes are hopeless at backing up. */
    v->k_reverse = ci->vtype == VEH_CLASS_BIKE
                   ? (v->k_thrust * VEH_REV_BIKE_NUM) / VEH_REV_BIKE_DEN
                   : (v->k_thrust * VEH_REVERSE_NUM) / VEH_REVERSE_DEN;

    /* The lever arms, measured from the CENTRE OF MASS, halved into world
     * pixels. A standard saloon: front wheel +25, rear -25, com -5, so the
     * front tyre pulls at 15 world px ahead of the mass and the rear at 10
     * behind it. That asymmetry is why the car pivots where it does. */
    v->r_front = (((long)ci->steering_wheel_offset - ci->cy) << 16) / 2;
    v->r_rear  = (((long)ci->drive_wheel_offset   - ci->cy) << 16) / 2;

    /* alpha = torque / I, with torque already divided by mass, so what is
     * wanted is mass/I. The moment converts as mass x length^2 (quarter) and
     * radians become 256ths of a turn (x 256/2pi), which together are one
     * constant: I_ours = moment / 163. */
    v->k_inertia = (m / mom) * 163L;
    if (v->k_inertia < 16) v->k_inertia = (m * 163L) / mom;
    if (v->k_inertia < 1) v->k_inertia = 1;

    /* `turn ratio` is DEGREES OF STEERING PER STEP - 12 for a car, 10 for a
     * three-wheeler, 8 for the superbike, 40 for the tank. 12 degrees is 8.53
     * of our 256 units, so full lock arrives in four steps. */
    v->turn_rate = (long)ci->turn_ratio * 65536L * 256L / 360L;
    v->skid_level = VEH_SKID_NUM / (m8 >> 8 ? m8 >> 8 : 1);
    v->skid = 0;
    v->sliding = 0;
    v->accum = 0;
}

void gta_veh_step(gta_veh *v, int throttle, int brake, int steer,
                  int handbrake, int on_road)
{
    long sb, cb;                        /* the body's own axis, Q14 */
    long sw, cw;                        /* the steered wheel's axis, Q14 */
    long ax = 0, ay = 0, torque = 0;    /* accumulated, already per unit mass */
    long skid = 0;                      /* the two axles' lateral forces, signed */
    long thrust = 0;
    long spd, d;

    /* ---- the original's clock ------------------------------------------
     *
     * The caller ticks at 50 Hz and the model wants 23.333, so most ticks do
     * nothing at all and every second or third one runs a full step. The
     * controls are read inside the step, which is also what the original
     * does - it samples the keyboard once per physics frame, not more. */
    v->accum += TIME_NUM;
    if (v->accum < TIME_DEN) return;
    v->accum -= TIME_DEN;

    /* ---- the driver ----------------------------------------------------
     *
     * The original reads four independent controls (accelerate, footbrake,
     * handbrake and a forward/reverse gear). This port has two arrow keys, so
     * DOWN means whichever of footbrake and reverse makes sense: brake while
     * there is forward speed, reverse once there is not. Everything else is
     * the original's. */
    sb = gta_sin16(v->ang16);
    cb = gta_cos16(v->ang16);
    /* A BROKEN ENGINE PULLS LESS. The original leaves the first 25 points of
     * damage free and then takes the power down linearly to a quarter at 100
     * (four sites in the original, all of them the same expression:
     * `(100 - (damage - 25)) * 0.01 * gear1`). It applies to the
     * PLAYER's car only, which is the only one this module drives. */
    {
        long thr = v->k_thrust, rev = v->k_reverse;
        if (v->damage > 25) {
            int d = v->damage > 100 ? 100 : v->damage;
            thr = (thr * (125 - d)) / 100;
            rev = (rev * (125 - d)) / 100;
        }
        if (throttle && !brake) thrust = thr;
        if (brake) thrust = -rev;
    }
    /* Wanted twice below - by the tank's engine cut and by the road snap -
     * so it is worked out once, here. One square root per physics step. */
    spd = speed_of(v);

    /* The tank, and only the tank, has its engine cut at its top speed. Every
     * other vehicle is limited by its own rolling resistance alone. */
    if (v->model_id == VEH_TANK_MODEL && thrust > 0 && spd > v->vmax)
        thrust = 0;

    /* The wheel winds on while the key is held, and stops at the lock. */
    if (steer > 0)      v->steer16 += v->turn_rate;
    else if (steer < 0) v->steer16 -= v->turn_rate;
    v->steer16 &= 0xFFFFFFL;

    d = ang_diff(v->steer16, v->ang16);
    if (d > VEH_STEER_LOCK)
        v->steer16 = (v->ang16 + VEH_STEER_LOCK) & 0xFFFFFFL;
    else if (d < -VEH_STEER_LOCK)
        v->steer16 = (v->ang16 - VEH_STEER_LOCK) & 0xFFFFFFL;

    sw = gta_sin16(v->steer16);
    cw = gta_cos16(v->steer16);

    /* ---- the two tyres --------------------------------------------------
     *
     * Each one resists the velocity OF ITS OWN CONTACT POINT, which is the
     * body's velocity plus the sweep of that point under this tick's
     * rotation. The original computes that sweep as a finite difference
     * across the whole predicted turn - position + velocity, rotated by
     * heading + omega, minus position rotated by heading - rather than the
     * linearised omega x r. That is what keeps it stable when the car is
     * spinning, so it is what this does too. */
    {
        long s2 = gta_sin16(v->ang16 + v->omega);
        long c2 = gta_cos16(v->ang16 + v->omega);
        int wheel;

        for (wheel = 0; wheel < 2; wheel++) {
            long r      = wheel ? v->r_rear : v->r_front;
            long axis_s = wheel ? sb : sw;      /* rear resolves on the BODY */
            long axis_c = wheel ? cb : cw;      /* front on the STEERED wheel */
            long kx, ky;
            long wvx, wvy, vlong, vlat, along, alat, fwx, fwy, rwx, rwy;

            /* the coefficients for this tyre, this tick */
            if (wheel == 0) {
                /* FRONT: the table's values, raw. Nothing ever scales them,
                 * and with no foot brake nothing ever adds to them either. */
                kx = v->k_adh_x;
                ky = v->k_adh_y;
            } else {
                /* REAR: both coefficients scaled by the grip multiplier, and
                 * the handbrake swapping the lateral one for the slide value
                 * while adding its friction to the longitudinal. */
                long grip  = v->model_id == VEH_BUS_MODEL
                             ? VEH_BUS_GRIP : VEH_REAR_GRIP;
                long slide = v->model_id == VEH_BUS_MODEL
                             ? VEH_BUS_GRIP : VEH_HAND_SLIDE;
                kx = v->k_adh_x;
                if (handbrake) kx += v->k_hand;
                kx = CMUL(grip, kx);
                ky = CMUL(handbrake ? slide : grip, v->k_adh_y);
            }

            /* the contact point's velocity */
            wvx = v->vx + UMUL(s2 - sb, r);
            wvy = v->vy - UMUL(c2 - cb, r);

            /* resolved onto this tyre's own axes */
            vlong = VDOT(wvx, wvy, axis_s, -axis_c);
            vlat  = VDOT(wvx, wvy, axis_c,  axis_s);

            along = -KMUL(kx, vlong);
            alat  = -KMUL(ky, vlat);
            if (wheel) along += thrust;         /* rear-wheel drive */

            /* THE GAME'S OWN DEFINITION OF SLIDING, accumulated here because
             * this is where the number exists. The original sums the two
             * axles' LATERAL forces - signed, so a pure spin partly cancels
             * and only a car actually travelling sideways trips it - takes
             * the magnitude, and lays tyre marks above 20.0
             * in the original. Our forces are already divided by mass, so
             * the threshold is scaled by mass to match. */
            skid += alat;

            /* back into the world */
            fwx = ((along >> 7) * axis_s + (alat >> 7) * axis_c) >> 7;
            fwy = ((alat  >> 7) * axis_s - (along >> 7) * axis_c) >> 7;

            ax += fwx;
            ay += fwy;

            /* torque about the centre of mass: r is along the body */
            rwx =  UMUL(sb, r);
            rwy = -UMUL(cb, r);
            torque += RMUL(rwx, fwy) - RMUL(rwy, fwx);
        }
    }

    /* The slide measure, and the original's own cheat on it. Above the
     * threshold the DOS game lays tyre marks; but the handbrake REDUCES the
     * rear's lateral force, so a handbrake turn often would not trip it - so
     * the code writes the threshold in by hand whenever the handbrake is down
     * above walking pace with the power on (`000c7d50:828`). Our forces carry
     * a division by mass the original's do not, so the comparison is done
     * against a mass-scaled threshold rather than a bare 20. */
    v->skid = skid < 0 ? -skid : skid;
    v->sliding = v->skid > v->skid_level;
    if (handbrake && throttle && spd > (2 * VEH_SPEED_UNIT))
        v->sliding = 1;

    /* ---- integrate: semi-implicit Euler, dt = 1 ------------------------- */
    v->vx += ax;
    v->vy += ay;
    v->omega += IMUL(v->k_inertia, torque);
    v->x += v->vx;
    v->y += v->vy;
    v->ang16 = (v->ang16 + v->omega) & 0xFFFFFFL;

    /* the geometric centre, which is what the rest of the port draws and
     * collides with */
    v->ox = v->x - UMUL(gta_sin16(v->ang16), v->com_y);
    v->oy = v->y + UMUL(gta_cos16(v->ang16), v->com_y);

    /* ---- the wheel comes back to centre when it is let go --------------- */
    if (!steer) {
        d = ang_diff(v->steer16, v->ang16);
        if (d > VEH_CENTRE_BAND)
            v->steer16 = (v->steer16 - VEH_CENTRE_RATE) & 0xFFFFFFL;
        else if (d < -VEH_CENTRE_BAND)
            v->steer16 = (v->steer16 + VEH_CENTRE_RATE) & 0xFFFFFFL;
    }

    /* ---- AND THE ROAD PULLS IT STRAIGHT ---------------------------------
     *
     * This is the assist that makes GTA 1 track so calmly down a street, and
     * it is easy to mistake for good physics. Hands off the wheel, going
     * forwards, above a walking pace, on a road block: the car is magnetised
     * to the nearest cardinal at 1.59 degrees a step, and once it lands
     * within a step of one the heading is FORCED exact and the rotation
     * zeroed outright.
     *
     * The window is 11.25 degrees either side (0x20 of the original's 1024,
     * eight of our 256), so it only ever tidies up a car that is already
     * nearly straight - it cannot fight a deliberate turn, and it does
     * nothing at all off-road or in reverse. the driving model, the block from
     * the speed test at asm 0xc8c87 through the quadrant headings at
     * 0xc8dd9. */
    if (on_road && !steer && !brake && spd > VEH_SNAP_SPEED) {
        long quad = v->ang16 & ((64L << 16) - 1);   /* how far past a cardinal */
        if (quad < VEH_SNAP_EXACT || quad > (64L << 16) - VEH_SNAP_EXACT) {
            /* landed: force it, and stop rotating */
            v->ang16 = (v->ang16 + (32L << 16)) & ~((64L << 16) - 1);
            v->ang16 &= 0xFFFFFFL;
            v->omega = 0;
            v->steer16 = v->ang16;
        } else if (quad < VEH_SNAP_BAND) {
            v->steer16 = (v->steer16 - VEH_SNAP_NUDGE) & 0xFFFFFFL;
        } else if (quad > (64L << 16) - VEH_SNAP_BAND) {
            v->steer16 = (v->steer16 + VEH_SNAP_NUDGE) & 0xFFFFFFL;
        }
    }
}

/* ==========================================================================
 * THE BODY AGAINST THE WORLD
 *
 * WHAT THIS REPLACES: one point, half a body length ahead of the centre. That
 * test is blind to everything a long vehicle does. Reversing into a wall puts
 * the nose at the far end from the impact, so nothing is detected at all. A
 * corner clipped with a rear quarter is not detected, because the nose is
 * already past it. And a bus is 60 px long against a 32 px block, so the two
 * ends can straddle a wall block entirely without either being in it.
 *
 * Measured with `gtadump drivecar`, one saloon driven at full throttle for 500
 * ticks: 203 ticks with some part of the body inside a wall, and at the worst
 * moment ALL TEN outline points were - the whole car inside the building.
 *
 * TEN POINTS, NOT FOUR. Four corners leave a 60-px flank unsampled; the thirds
 * of each long side close that, and the middle of each end catches a wall met
 * square on.
 *
 * AND IT IS RESOLVED PER AXIS, which is the difference between a wall you
 * scrape along and a wall you stick to. Rejecting the whole step because one
 * corner caught also cancels the part of the movement that was parallel to the
 * wall, so the car stops dead against a kerb it was only brushing. Trying X
 * and Y separately keeps whichever half is legal - the classic slide - and it
 * costs one extra outline test.
 *
 * The heading is undone last and only if the body is still inside something
 * once the position is legal: you may not TURN into a wall either, and that is
 * a rotation, which no amount of translation can undo.
 * ========================================================================== */

/* along, across; thousandths of the half-extents */
static const short veh_wall_samp[10][2] = {
    {-1000, -1000}, {-1000, 0}, {-1000, 1000},
    { -333, -1000}, { -333, 1000},
    {  333, -1000}, {  333, 1000},
    { 1000, -1000}, { 1000, 0}, { 1000, 1000}
};

/* Is any of the outline standing on ground a car may not be on? Ground type 2
 * is road and 3 is pavement (gta_nav.h); 5 is water and anything under 2 is a
 * building or the edge of the world - the same test the nose used. */
static int veh_body_blocked(const gta_nav *nav, int layer, long cx, long cy,
                            int ang, int hl, int hw)
{
    long fx = gta_sin(ang), fy = -gta_cos(ang);
    long rx = gta_cos(ang), ry = gta_sin(ang);
    int i;

    for (i = 0; i < 10; i++) {
        long al = (long)hl * veh_wall_samp[i][0] / 1000;
        long si = (long)hw * veh_wall_samp[i][1] / 1000;
        long px = cx + (fx * al + rx * si) * 4;
        long py = cy + (fy * al + ry * si) * 4;
        int g = gta_nav_ground(gta_nav_at_m(nav, (int)(px >> 21),
                                            (int)(py >> 21), layer));
        if (g < 2 || g == 5)
            return 1;
    }
    return 0;
}


/* PUSH A WEDGED BODY BACK OUT - the port's answer to the corner push-out.
 *
 * Walks the same ten outline points. For each one standing on ground a car may
 * not be on, it adds the direction from that point back towards the centre;
 * the sum is the way out. Applied as whole pixels of position, because this
 * port's wall resolve is positional rather than force-based.
 *
 * Returns 1 if it moved the car. The step is two pixels when the car is
 * stopped and one while it is moving, which is the original's own choice of
 * magnitudes - a car that is already travelling needs less help.
 *
 * Without this a car that ends up inside geometry by ANY route the wall test
 * does not gate - a shove from another car, a rotation that was legal for the
 * centre but not a corner, a spawn against a kerb - is stuck for ever: the
 * resolve refuses both axes and the heading every tick and never moves it. */
static int veh_unstick(gta_veh *v, const gta_nav *nav, int layer)
{
    long fx = gta_sin(gta_veh_angle(v)), fy = -gta_cos(gta_veh_angle(v));
    long rx = gta_cos(gta_veh_angle(v)), ry = gta_sin(gta_veh_angle(v));
    int hl = v->len / 2, hw = v->wid / 2;
    long sx = 0, sy = 0;
    int i, n = 0, step;

    for (i = 0; i < 10; i++) {
        long al = (long)hl * veh_wall_samp[i][0] / 1000;
        long si = (long)hw * veh_wall_samp[i][1] / 1000;
        long px = v->ox + (fx * al + rx * si) * 4;
        long py = v->oy + (fy * al + ry * si) * 4;
        int g = gta_nav_ground(gta_nav_at_m(nav, (int)(px >> 21),
                                            (int)(py >> 21), layer));
        if (g < 2 || g == 5) {
            /* Away from the offending point, i.e. towards the centre. */
            sx -= (fx * al + rx * si);
            sy -= (fy * al + ry * si);
            n++;
        }
    }
    if (!n)
        return 0;

    /* Normalise crudely - the direction is what matters, not the length. */
    {
        long m = (sx < 0 ? -sx : sx) + (sy < 0 ? -sy : sy);
        if (m < 1) {
            /* Dead centre of a solid block: no direction to prefer. Back the
             * way the car came, which is the only guess with any information
             * in it. */
            sx = -fx; sy = -fy; m = (fx < 0 ? -fx : fx) + (fy < 0 ? -fy : fy);
            if (m < 1) return 0;
        }
        step = (v->vx == 0 && v->vy == 0) ? 2 : 1;
        v->x  += (sx * step * 65536L) / m;
        v->y  += (sy * step * 65536L) / m;
        v->ox += (sx * step * 65536L) / m;
        v->oy += (sy * step * 65536L) / m;
    }
    /* It is not driving anywhere while it is inside a wall. */
    v->vx = v->vy = 0;
    v->omega = 0;
    return 1;
}

int gta_veh_wall(gta_veh *v, const void *navp, int layer,
                 long x0, long y0, long ox0, long oy0, long ang0)
{
    const gta_nav *nav = (const gta_nav *)navp;
    long dx = v->x - x0, dy = v->y - y0;
    /* The move the world is about to refuse - kept because it is the only
     * thing that says which way the car was going when it got wedged. */
    long odx = dx, ody = dy;
    int ang = gta_veh_angle(v);
    int hl = v->len / 2, hw = v->wid / 2;
    int hit = 0;

    if (!nav)
        return 0;

    /* X first, then Y on top of whatever X survived. */
    /* THE BLOCKED COMPONENT IS ZEROED, NOT BOUNCED. Reflecting a quarter of
     * it made a car held against a wall under power oscillate - in, out, in -
     * so it never settled and never left. The original zeroes the speed on a
     * wall hit and pushes out separately. Zeroing only the blocked axis leaves
     * the parallel one alone, which is exactly the slide. */
    if (dx && veh_body_blocked(nav, layer, ox0 + dx, oy0, ang, hl, hw)) {
        dx = 0;
        v->vx = 0;
        hit = 1;
    }
    if (dy && veh_body_blocked(nav, layer, ox0 + dx, oy0 + dy, ang, hl, hw)) {
        dy = 0;
        v->vy = 0;
        hit = 1;
    }

    /* Both axes refused and the car did not move: it may still have TURNED
     * into something. Put the heading back where it was. */
    if (veh_body_blocked(nav, layer, ox0 + dx, oy0 + dy, ang, hl, hw)) {
        int a0 = (int)((ang0 >> 16) & 255);
        if (!veh_body_blocked(nav, layer, ox0 + dx, oy0 + dy, a0, hl, hw)) {
            v->ang16 = ang0;
            v->omega = 0;
            hit = 1;
        }
    }

    if (!hit) {
        /* ONLY A CAR THAT ACTUALLY MOVED IS NOT STUCK. Clearing the counter on
         * every collision-free tick cleared it every OTHER tick, because the
         * physics runs at 23.3 Hz inside a 50 Hz loop and the steps in between
         * move nothing and collide with nothing - so the count never reached
         * its threshold and the car sat against the wall for ever. */
        if (dx || dy)
            v->wall_stuck = 0;
        /* Not colliding this tick - but it may already BE inside something
         * from an earlier one, and if it is, nothing above would ever notice.
         * This is the only place that can tell. */
        if (veh_body_blocked(nav, layer, v->ox, v->oy,
                             gta_veh_angle(v), hl, hw))
            veh_unstick(v, nav, layer);
        return 0;
    }

    /* WEDGED: everything was refused and the car has not moved. A car nose-in
     * to a wall has to TURN to get out, and turning swings a corner further
     * in, so the turn is refused too - it can be there for ever. After
     * GTA_WALL_STUCK ticks of that, ease it back the way it came, a pixel at
     * a time. The original does the equivalent with a unit force at each
     * corner standing in a solid block (the corner push-out). */
    if (dx == 0 && dy == 0) {
        if (++v->wall_stuck > GTA_WALL_STUCK) {
            /* BACK THE WAY IT CAME. Pushing along the nose is wrong for a car
             * that REVERSED into something - it drives that one further in.
             * The refused move is the direction with the information in it. */
            long m = (odx < 0 ? -odx : odx) + (ody < 0 ? -ody : ody);
            long bx, by;

            if (m < 1) {
                int a = gta_veh_angle(v);
                bx = -(long)gta_sin(a) * 4;
                by =  (long)gta_cos(a) * 4;
            } else {
                bx = -(odx * 65536L) / m;         /* one pixel, that way */
                by = -(ody * 65536L) / m;
            }
            if (!veh_body_blocked(nav, layer, ox0 + bx, oy0 + by, ang,
                                  hl, hw)) {
                dx = bx;
                dy = by;
            }
            v->wall_stuck = 0;
        }
    } else {
        v->wall_stuck = 0;
    }

    v->x  = x0  + dx;  v->y  = y0  + dy;
    v->ox = ox0 + dx;  v->oy = oy0 + dy;

    /* AND IF IT IS STILL WEDGED after refusing both axes and the heading,
     * push it out. Without this the three refusals repeat every tick and the
     * car never moves again - "it sticks to buildings and jams". */
    if (veh_body_blocked(nav, layer, v->ox, v->oy, gta_veh_angle(v), hl, hw))
        veh_unstick(v, nav, layer);
    {
        long iv = (v->vx < 0 ? -v->vx : v->vx)
                + (v->vy < 0 ? -v->vy : v->vy);
        return (int)(iv >> 16);
    }
}
