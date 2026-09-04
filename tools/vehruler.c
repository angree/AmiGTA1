/* THE RULER for the driven car's physics.
 *
 * A model is not "right" or "wrong" by eye alone - it has numbers, and this
 * prints them: how long a car takes to reach its top speed, how far it needs
 * to stop, how tight a circle it holds with the wheel down, how far the body
 * points away from where it is actually going. Run it before and after a
 * change to the model and the diff is the change, in metres and seconds
 * rather than adjectives.
 *
 * No map, no rendering, no Amiga headers: it drives `gta_veh_step` in a void.
 * That is the point - the physics is the only thing under test.
 *
 * UNITS. The world is 32 pixels to a city block and the simulation runs at
 * 50 Hz, so everything is also printed in blocks and seconds, which is what a
 * human can compare against the original.
 *
 *   wsl sh /mnt/i/GITHUB/Amiga_GTA/tools/bin/build_host.sh release
 *   build/host/vehruler build/data/style001.til 0
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../native/gta_tiles.h"
#include "../native/gta_car.h"
#include "../native/gta_vehphys.h"
#include "../native/gta_trig.h"
#include "../native/gta_style.h"

#define TICKS_PER_SEC 50
/* The physics steps at the original's 23.333 Hz (gta_vehphys.c), so a
 * velocity is per STEP, not per tick. */
#define STEPS_PER_SEC 23
#define PX_PER_BLOCK  32

/* 16.16 -> a printable whole-and-hundredths pair, without a float anywhere. */
#define WHOLE(f)  ((long)((f) >> 16))
#define FRAC2(f)  ((long)((((f) < 0 ? -(f) : (f)) & 0xFFFFL) * 100L >> 16))
#define SIGN(f)   (((f) < 0 && WHOLE(f) == 0) ? "-" : "")

static long isqrt_l(long v)
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

/* Speed as a scalar, 16.16 px/tick. */
static long speed_of(const gta_veh *v)
{
    long sx = v->vx >> 8, sy = v->vy >> 8;      /* Q8 to keep the square small */
    return isqrt_l(sx * sx + sy * sy) << 8;
}

/* px per physics step, 16.16 -> blocks per second x100.
 * blocks/s = px_per_step * 23.333 / 32 = px_per_step * 0.729167. */
static long bps100(long pxtick)
{
    return ((pxtick >> 8) * 729L) / 2560L;
}

static void print_speed(const char *label, long pxtick)
{
    long b = bps100(pxtick);
    printf("%s%s%ld.%02ld px/step  (%ld.%02ld blocks/s, %ld px/s)\n",
           label, SIGN(pxtick), WHOLE(pxtick), FRAC2(pxtick),
           b / 100, b % 100, ((pxtick >> 16) * 70L) / 3L);
}

/* Run the model until it stops gaining speed, and report the plateau. */
static long settle(gta_veh *v, int maxticks, int *ticks_out, long *dist_out)
{
    long best = 0, x0 = v->x, y0 = v->y;
    int t, stable = 0;
    for (t = 0; t < maxticks; t++) {
        long s;
        gta_veh_step(v, 1, 0, 0, 0, 0);
        s = speed_of(v);
        if (s > best + 256) { best = s; stable = 0; }
        else if (++stable > 25) { t++; break; }
    }
    if (ticks_out) *ticks_out = t;
    if (dist_out) {
        long dx = (v->x - x0) >> 16, dy = (v->y - y0) >> 16;
        *dist_out = isqrt_l(dx * dx + dy * dy);
    }
    return best;
}

/* The angle between where the body points and where it is actually going,
 * in whole degrees. This is the slip angle, and it is the single number that
 * says whether a model slides at all. */
static int slip_deg(const gta_veh *v)
{
    long fx, fy, sx, sy, dot, cross, sp;
    int a = gta_veh_angle(v);
    int deg;
    sp = speed_of(v);
    if (sp < 6553) return 0;                    /* below 0.1 px/tick: noise */
    fx = gta_sin(a); fy = -gta_cos(a);          /* Q14 forward */
    sx = v->vx >> 8; sy = v->vy >> 8;
    dot   = (fx * sx + fy * sy) >> 14;
    cross = (fx * sy - fy * sx) >> 14;
    if (dot < 0) { dot = -dot; }                /* reversing: fold it */
    if (cross < 0) cross = -cross;
    if (dot == 0) return 90;
    /* atan without a float: walk a coarse table of tan(deg) in Q14. */
    {
        static const long tan14[] = {           /* tan(1..44 deg) * 16384 */
              286,  572,  859, 1146, 1434, 1723, 2014, 2308, 2603, 2901,
             3203, 3508, 3817, 4131, 4450, 4774, 5104, 5441, 5786, 6138,
             6499, 6869, 7250, 7642, 8046, 8462, 8892, 9338, 9799,10276,
            10773,11289,11826,12387,12973,13585,14228,14903,15613,16362,
            17153,17990,18876,19816
        };
        long ratio = (cross << 14) / dot;
        for (deg = 0; deg < 44; deg++)
            if (ratio < tan14[deg]) break;
        return deg;
    }
}

/* ============ THE ORIGINAL'S SHAPE, AS A PROTOTYPE ==========================
 *
 * The original's AI car is not a velocity vector at all - it is TWO POINTS.
 *
 *     F = P + h*u(th) ;  R = P - h*u(th)          rebuilt every frame
 *     F += s*u(front_angle) ;  R += s*u(rear_angle)
 *     P' = midpoint(F, R)
 *     th' = atan2(F - R)
 *
 * with, while a turn is held, front = body + 3T and rear = body - 2T,
 * T = 11.25 degrees (0x20 of the original's 0x400 circle, 8 of our 256).
 * The rear wheel is steered OUT of the corner, which is what makes the tail
 * sweep. Nothing accumulates: the points are re-derived from position and
 * heading at the top of every frame, so the body length is rigid and there is
 * no drift.
 *
 * This runs it side by side with the port's own model, on the same ruler, so
 * the two can be compared in numbers rather than adjectives. It is a
 * PROTOTYPE inside a measuring tool - not the port's physics, and it borrows
 * the port's acceleration so that only the GEOMETRY differs.
 */
typedef struct {
    long x, y;              /* 16.16 world px */
    long ang16;             /* 16.16 of the 256-step circle */
    long s;                 /* 16.16 px per tick, a SCALAR */
    long vmax, vmin, acc, dec;
    int  h;                 /* half-length, world px */
} twin_veh;

#define TWIN_T 8            /* 11.25 degrees in our 256-step circle */
#define TWIN_ANG(tv) ((int)(((tv)->ang16 >> 16) & 255))

static void twin_init(twin_veh *tv, const gta_veh *ref, long x, long y, int a)
{
    tv->x = x; tv->y = y; tv->ang16 = (long)(a & 255) << 16; tv->s = 0;
    /* The prototype only exists to show the AI model's GEOMETRY, so it
     * borrows the driven car's engine and stops where a driven car stops. */
    tv->acc = ref->k_thrust;
    tv->dec = ref->k_reverse;
    tv->vmax = ref->k_thrust * 28;      /* roughly its own terminal speed */
    tv->vmin = -(tv->vmax / 3);
    tv->h = ref->len / 2;
    if (tv->h < 4) tv->h = 4;
}

static void twin_step(twin_veh *tv, int throttle, int brake, int steer)
{
    long fx, fy, rx, ry;
    long fa16, ra16;

    if (throttle && !brake) {
        tv->s += tv->acc;
        if (tv->s > tv->vmax) tv->s = tv->vmax;
    } else if (brake) {
        if (tv->s > 0) { tv->s -= tv->dec; if (tv->s < 0) tv->s = 0; }
        else { tv->s -= tv->acc >> 1; if (tv->s < tv->vmin) tv->s = tv->vmin; }
    } else {
        tv->s -= (tv->s >> 6) + (tv->s > 0 ? 900 : (tv->s < 0 ? -900 : 0));
        if (tv->s > 0 && tv->s < 1000) tv->s = 0;
        if (tv->s < 0 && tv->s > -1000) tv->s = 0;
    }

    /* The wheels. At zero speed the original freezes them; here that falls
     * out anyway, because both points then move by nothing. */
    fa16 = tv->ang16 + ((long)(3 * TWIN_T * steer) << 16);
    ra16 = tv->ang16 - ((long)(2 * TWIN_T * steer) << 16);

    /* Rebuild the two points from the body, THEN advance each along its own
     * heading. h*u is Q14 times whole pixels, so <<2 lands on 16.16. */
    fx = tv->x + (gta_sin16(tv->ang16) * tv->h << 2);
    fy = tv->y - (gta_cos16(tv->ang16) * tv->h << 2);
    rx = tv->x - (gta_sin16(tv->ang16) * tv->h << 2);
    ry = tv->y + (gta_cos16(tv->ang16) * tv->h << 2);

    fx += ((tv->s >> 7) * gta_sin16(fa16)) >> 7;
    fy -= ((tv->s >> 7) * gta_cos16(fa16)) >> 7;
    rx += ((tv->s >> 7) * gta_sin16(ra16)) >> 7;
    ry -= ((tv->s >> 7) * gta_cos16(ra16)) >> 7;

    tv->x = rx + ((fx - rx) >> 1);
    tv->y = ry + ((fy - ry) >> 1);
    tv->ang16 = gta_dir16(fx - rx, fy - ry);
}

/* The same steady-circle measurement, on the prototype. */
static void twin_circle(const gta_tiles *ti, int model, int pct, long top)
{
    gta_veh ref;
    twin_veh tv;
    long target, minx, maxx, miny, maxy, dia = 0;
    int t, prev, turned = 0, lap = 0;

    gta_veh_init(&ref, ti, model, 0, 0, 0);
    twin_init(&tv, &ref, 0, 0, 0);
    target = (top / 100) * pct;
    for (t = 0; t < 2000 && tv.s < target; t++)
        twin_step(&tv, 1, 0, 0);
    prev = TWIN_ANG(&tv);
    minx = maxx = tv.x; miny = maxy = tv.y;
    for (t = 0; t < 4000; t++) {
        int d;
        twin_step(&tv, tv.s < target, 0, 1);
        d = (TWIN_ANG(&tv) - prev) & 255; if (d > 128) d -= 256;
        turned += d; prev = TWIN_ANG(&tv);
        if (tv.x < minx) minx = tv.x;
        if (tv.x > maxx) maxx = tv.x;
        if (tv.y < miny) miny = tv.y;
        if (tv.y > maxy) maxy = tv.y;
        if (turned >= 256 || turned <= -256) { lap = t + 1; break; }
    }
    if (lap) dia = (((maxx - minx) >> 16) + ((maxy - miny) >> 16)) / 2;
    printf("  %4d  %6ld.%02ld  %10ld  %6ld.%02ld  %10d%s\n",
           pct, WHOLE(tv.s), FRAC2(tv.s), dia / 2,
           (dia / 2) / PX_PER_BLOCK, ((dia / 2) % PX_PER_BLOCK) * 100 / PX_PER_BLOCK,
           lap, lap ? "" : "  (no lap in 4000 ticks)");
}

static void hdr(const char *what)
{
    printf("\n--- %s ---\n", what);
}

/* One steady circle at `pct` of the model's top speed. The throttle is
 * bang-bang against the target so the lap is driven at a held speed rather
 * than a decaying one; the radius then means something. */
static void circle_test(const gta_tiles *ti, int model, int pct, long top)
{
    gta_veh v;
    long target, minx, maxx, miny, maxy, dia = 0;
    int t, prev, turned = 0, lap = 0, maxslip = 0;

    gta_veh_init(&v, ti, model, 0, 0, 0);
    target = (top / 100) * pct;
    for (t = 0; t < 2000; t++) {
        if (speed_of(&v) >= target) break;
        gta_veh_step(&v, 1, 0, 0, 0, 0);
    }
    prev = gta_veh_angle(&v);
    minx = maxx = v.x; miny = maxy = v.y;
    for (t = 0; t < 4000; t++) {
        int a, d, thr = speed_of(&v) < target;
        gta_veh_step(&v, thr, 0, 1, 0, 0);
        a = gta_veh_angle(&v);
        d = (a - prev) & 255; if (d > 128) d -= 256;
        turned += d; prev = a;
        if (v.x < minx) minx = v.x;
        if (v.x > maxx) maxx = v.x;
        if (v.y < miny) miny = v.y;
        if (v.y > maxy) maxy = v.y;
        if (t > 30) { int s = slip_deg(&v); if (s > maxslip) maxslip = s; }
        if (turned >= 256 || turned <= -256) { lap = t + 1; break; }
    }
    if (lap) dia = (((maxx - minx) >> 16) + ((maxy - miny) >> 16)) / 2;
    {
        /* TWO radii, because they disagree and the disagreement is the
         * finding. The bounding box measures the ground the car COVERS,
         * which a sliding car inflates by its own sideways travel; v/omega
         * measures the arc its heading actually describes. With 20 degrees
         * of slip the box reads a fifth high, so the honest number for
         * "how tight does it corner" is the second one. */
        long steps = (long)lap * 7 / 15;
        long rw = (lap && steps) ? (speed_of(&v) >> 8) * steps / 1608 : 0;
        printf("  %4d  %6ld.%02ld  %8ld  %8ld  %6ld.%02ld  %8d  %3d deg%s\n",
               pct, WHOLE(speed_of(&v)), FRAC2(speed_of(&v)),
               dia / 2, rw, rw / PX_PER_BLOCK,
               (rw % PX_PER_BLOCK) * 100 / PX_PER_BLOCK,
               lap, maxslip, lap ? "" : "  (no lap)");
    }
}

int main(int argc, char **argv)
{
    gta_tiles ti;
    const gta_car_info *ci;
    gta_veh v;
    int model, t, ticks;
    long top, dist, x0, y0;

    if (argc < 3) {
        fprintf(stderr, "usage: vehruler <style.til> <model> [model...]\n"
                        "       vehruler <style.til> all      "
                        "top speed of every vehicle, fastest first\n");
        return 2;
    }

    /* THE WHOLE FLEET, fastest first. The speeds are DRIVEN, not computed
     * from `gear1 / (adhesion_x * 2.5)` - the point is to check this port's
     * model against that arithmetic rather than to restate it. If a row
     * disagrees with the formula, the model is wrong somewhere. */
    if (strcmp(argv[2], "all") == 0) {
        gta_tiles ti2;
        int i, j, n;
        static long tops[128];
        static int order[128];

        if (gta_tiles_load(argv[1], &ti2) != 0) {
            fprintf(stderr, "vehruler: cannot load %s\n", argv[1]);
            return 1;
        }
        n = ti2.n_cars > 128 ? 128 : ti2.n_cars;
        for (i = 0; i < n; i++) {
            gta_veh vv;
            gta_veh_init(&vv, &ti2, i, 0, 0, 0);
            tops[i] = settle(&vv, 4000, 0, 0);
            order[i] = i;
        }
        for (i = 1; i < n; i++) {           /* insertion sort, descending */
            int k = order[i];
            for (j = i; j > 0 && tops[order[j - 1]] < tops[k]; j--)
                order[j] = order[j - 1];
            order[j] = k;
        }
        printf("%-3s %-4s %-11s %8s %8s %9s %7s %6s  %s\n",
               "rec", "id", "class", "px/step", "blocks/s", "km/h",
               "thrust", "mass", "w x l");
        for (i = 0; i < n; i++) {
            int m = order[i];
            const gta_car_info *c = &ti2.cars[m];
            long b = bps100(tops[m]);
            /* km/h at 6.25 cm a world pixel and 23.333 steps a second:
             * px/step * 23.333 * 0.125 m * 3.6 = px/step * 10.5 */
            long kmh = ((tops[m] >> 8) * 2688L) >> 16;
            printf("%3d %4d %-11s %5ld.%02ld %5ld.%02ld %8ld %4ld.%02ld "
                   "%3ld.%02ld  %dx%d\n",
                   m, c->model_id, gta_vehicle_class_name(c->vtype),
                   WHOLE(tops[m]), FRAC2(tops[m]), b / 100, b % 100, kmh,
                   WHOLE(c->thrust), FRAC2(c->thrust),
                   WHOLE(c->mass), FRAC2(c->mass),
                   gta_car_world_wid(c), gta_car_world_len(c));
        }
        gta_tiles_free(&ti2);
        return 0;
    }
    /* gta_dir16's self-test, first and always: the two-point model re-derives
     * its heading from this function every tick, so an error here is an error
     * in every arc the car drives. Feed it the vector for a known angle at
     * several radii and see what comes back. */
    {
        long worst = 0;
        int a, r;
        /* The lengths the two-point model actually hands it, in 16.16: a
         * motorcycle's 19-px baseline, a car's 31, a bus's 60, and one
         * deliberately short 2-px case to see where it starts to fray. */
        static const long radii[] = { 2L << 16, 19L << 16, 31L << 16,
                                      60L << 16, 200L << 16 };
        for (r = 0; r < 5; r++) {
            for (a = 0; a < 256; a++) {
                long vx =  ((radii[r] >> 7) * gta_sin(a)) >> 7;
                long vy = -((radii[r] >> 7) * gta_cos(a)) >> 7;
                long got = gta_dir16(vx, vy);
                long want = (long)a << 16;
                long err = got - want;
                if (err >  (128L << 16)) err -= 256L << 16;
                if (err < -(128L << 16)) err += 256L << 16;
                if (err < 0) err = -err;
                if (err > worst) worst = err;
            }
        }
        printf("gta_dir16 self-test: worst error %ld.%03ld of 256 steps "
               "(%ld.%02ld degrees)%s\n", worst >> 16,
               ((worst & 0xFFFFL) * 1000L) >> 16,
               (worst * 360 / 256) >> 16,
               ((((worst * 360 / 256)) & 0xFFFFL) * 100L) >> 16,
               worst > (2L << 16) ? "   *** TOO COARSE ***" : "");
    }

    if (gta_tiles_load(argv[1], &ti) != 0) {
        fprintf(stderr, "vehruler: cannot load %s\n", argv[1]);
        return 1;
    }

    for (argc = 2; argv[argc]; argc++) {
        model = atoi(argv[argc]);
        if (model < 0 || model >= ti.n_cars) {
            fprintf(stderr, "vehruler: model %d of %d\n", model, ti.n_cars);
            continue;
        }
        ci = &ti.cars[model];

        printf("\n========================================================\n");
        printf("model %d  %dx%d world px  table: max %d min %d thrust %ld.%02ld "
               "mass %ld.%02ld\n", model,
               gta_car_world_wid(ci), gta_car_world_len(ci),
               ci->max_speed, ci->min_speed,
               WHOLE(ci->thrust), FRAC2(ci->thrust),
               WHOLE(ci->mass), FRAC2(ci->mass));
        printf("           adhesion x %ld.%02ld y %ld.%02ld  brakes hand "
               "%ld.%02ld foot %ld.%02ld bias %ld.%02ld\n",
               WHOLE(ci->tyre_adhesion_x), FRAC2(ci->tyre_adhesion_x),
               WHOLE(ci->tyre_adhesion_y), FRAC2(ci->tyre_adhesion_y),
               WHOLE(ci->handbrake_friction), FRAC2(ci->handbrake_friction),
               WHOLE(ci->footbrake_friction), FRAC2(ci->footbrake_friction),
               WHOLE(ci->front_brake_bias), FRAC2(ci->front_brake_bias));
        printf("           slide back %ld.%02ld hand %ld.%02ld  moment %ld  "
               "turn_ratio %d  axles drive %d steer %d\n",
               WHOLE(ci->back_end_slide), FRAC2(ci->back_end_slide),
               WHOLE(ci->handbrake_slide), FRAC2(ci->handbrake_slide),
               ci->moment, ci->turn_ratio,
               ci->drive_wheel_offset, ci->steering_wheel_offset);

        /* 1. STANDING START. */
        hdr("standing start, full throttle");
        gta_veh_init(&v, &ti, model, 0, 0, 0);
        top = settle(&v, 1500, &ticks, &dist);
        print_speed("top speed   ", top);
        printf("reached in  %d ticks (%d.%02d s) over %ld px (%ld.%02ld blocks)\n",
               ticks, ticks / TICKS_PER_SEC, (ticks % TICKS_PER_SEC) * 2,
               dist, dist / PX_PER_BLOCK, (dist % PX_PER_BLOCK) * 100 / PX_PER_BLOCK);

        /* 2. BRAKING from the top. */
        hdr("braking from top speed");
        x0 = v.x; y0 = v.y;
        for (t = 0; t < 1000; t++) {
            gta_veh_step(&v, 0, 1, 0, 0, 0);
            if (speed_of(&v) < 13107) break;    /* under 0.2 px/tick */
        }
        dist = isqrt_l(((v.x - x0) >> 16) * ((v.x - x0) >> 16)
                     + ((v.y - y0) >> 16) * ((v.y - y0) >> 16));
        printf("stopped in  %d ticks (%d.%02d s) over %ld px (%ld.%02ld blocks)\n",
               t, t / TICKS_PER_SEC, (t % TICKS_PER_SEC) * 2,
               dist, dist / PX_PER_BLOCK, (dist % PX_PER_BLOCK) * 100 / PX_PER_BLOCK);

        /* 3. COASTING. */
        hdr("coasting from top speed");
        gta_veh_init(&v, &ti, model, 0, 0, 0);
        settle(&v, 1500, 0, 0);
        x0 = v.x; y0 = v.y;
        for (t = 0; t < 3000; t++) {
            gta_veh_step(&v, 0, 0, 0, 0, 0);
            if (speed_of(&v) < 13107) break;
        }
        dist = isqrt_l(((v.x - x0) >> 16) * ((v.x - x0) >> 16)
                     + ((v.y - y0) >> 16) * ((v.y - y0) >> 16));
        printf("rolled to a stop in %d ticks (%d.%02d s) over %ld px "
               "(%ld.%02ld blocks)\n", t, t / TICKS_PER_SEC,
               (t % TICKS_PER_SEC) * 2, dist,
               dist / PX_PER_BLOCK, (dist % PX_PER_BLOCK) * 100 / PX_PER_BLOCK);

        /* 3b. DOES IT ACTUALLY STOP?
         *
         * "bezwladne auto dalej sie lekko rusza mimo ze powinno stac. i w ten
         * sposob np. wjezdza pomalu w solid blok." Test 3 above stops
         * measuring at 0.2 px/tick, so a car that never reaches ZERO looks
         * like a car that stopped. The tyre force is proportional to the
         * velocity it is resisting, so in fixed point it rounds to nothing
         * long before the velocity does, and what is left creeps forever.
         *
         * Thirty seconds with every control released, from the moment the
         * coast test gave up. A car at rest moves 0 px. */
        {
            long rx0 = v.x, ry0 = v.y, crept;
            int still = 0;
            for (t = 0; t < 30 * TICKS_PER_SEC; t++) {
                gta_veh_step(&v, 0, 0, 0, 0, 0);
                if (v.vx == 0 && v.vy == 0 && v.omega == 0) { still = t + 1; break; }
            }
            crept = isqrt_l(((v.x - rx0) >> 16) * ((v.x - rx0) >> 16)
                          + ((v.y - ry0) >> 16) * ((v.y - ry0) >> 16));
            printf("then, hands off for 30 s: crept %ld px, "
                   "v (%ld,%ld) omega %ld - %s\n",
                   crept, v.vx, v.vy, v.omega,
                   still ? "AT REST" : "*** STILL MOVING ***");
        }

        /* 3c. THE SAME, FROM EVERY HEADING AND AFTER A TURN.
         *
         * Straight down the +x axis is the one case where one velocity
         * component is exactly zero to begin with, so it is the one case that
         * proves the least. A car that has been turned carries velocity in
         * both axes and a residual omega, and each of those decays through
         * its own rounding. */
        hdr("comes to rest? every 32nd heading, after a turn");
        {
            int a, worst_a = -1, moving = 0;
            long worst = 0;
            for (a = 0; a < 256; a += 8) {
                long rx0, ry0, crept;
                gta_veh_init(&v, &ti, model, 0, 0, a);
                for (t = 0; t < 200; t++) gta_veh_step(&v, 1, 0, 0, 0, 0);
                for (t = 0; t < 40; t++)  gta_veh_step(&v, 1, 0, 1, 0, 0);
                /* hands off, and let it roll to whatever it rolls to */
                for (t = 0; t < 3000; t++) {
                    gta_veh_step(&v, 0, 0, 0, 0, 0);
                    if (speed_of(&v) < 13107) break;
                }
                rx0 = v.x; ry0 = v.y;
                for (t = 0; t < 30 * TICKS_PER_SEC; t++)
                    gta_veh_step(&v, 0, 0, 0, 0, 0);
                crept = isqrt_l(((v.x - rx0) >> 16) * ((v.x - rx0) >> 16)
                              + ((v.y - ry0) >> 16) * ((v.y - ry0) >> 16));
                /* THE VERDICT IS THE FINAL STATE, NOT THE DISTANCE. A car
                 * that rolls two more pixels and then stops dead is a car
                 * that stopped; a car left with any velocity at all has not,
                 * and will still be moving next year. */
                if (v.vx || v.vy || v.omega) {
                    moving++;
                    printf("  heading %3d: STILL MOVING - v (%ld,%ld) "
                           "omega %ld after %ld px\n",
                           a, v.vx, v.vy, v.omega, crept);
                }
                if (crept > worst) { worst = crept; worst_a = a; }
            }
            printf("last roll before it settled: %ld px (heading %d); "
                   "%d of 32 headings never stopped%s\n",
                   worst, worst_a, moving,
                   moving ? "   *** IT NEVER STOPS ***" : "   - all at rest");
        }

        /* 3d. WHERE THE TYRES GIVE UP.
         *
         * The force each tyre makes is proportional to the velocity it is
         * resisting, so below some speed it rounds to nothing and the car
         * coasts forever. That speed is a property of this model's adhesion
         * coefficients, and it is the number any rest threshold has to clear.
         * Found by bisection: the largest velocity a no-input step leaves
         * completely unchanged. */
        hdr("the speed below which the tyres take nothing off");
        {
            long lo = 0, hi = 1L << 20;     /* 16 px a step, far above it */
            while (lo < hi) {
                long mid = lo + (hi - lo + 1) / 2, before, after;
                gta_veh_init(&v, &ti, model, 0, 0, 0);
                v.vx = mid; v.vy = 0; v.omega = 0;
                before = speed_of(&v);
                /* The model steps at 23.333 Hz, so several ticks may pass
                 * before one of them does any work at all. */
                for (t = 0; t < 3; t++) gta_veh_step(&v, 0, 0, 0, 0, 0);
                after = speed_of(&v);
                if (after >= before) lo = mid; else hi = mid - 1;
            }
            printf("stalls at or below %ld.%02ld px/step "
                   "(%ld.%02ld px a second)\n",
                   WHOLE(lo), FRAC2(lo),
                   WHOLE(lo * STEPS_PER_SEC), FRAC2(lo * STEPS_PER_SEC));
        }

        /* 4. THE STEADY CIRCLE at three speeds. Wheel hard over, throttle
         * modulated to hold the target speed, radius read off the bounding
         * box of one full lap - no trigonometry and no assumption about
         * where the centre is.
         *
         * THREE speeds because that is the question that separates the two
         * families of model. A two-point bicycle, which is what the original
         * is believed to be, fixes the radius by GEOMETRY and it comes out
         * the SAME at every speed. A force model with real tyres widens the
         * circle as speed rises, because the grip runs out. Whichever the
         * original turns out to be, this table says which one WE are. */
        hdr("full-lock circle, three speeds");
        printf("  %%top  speed(px/s)   box(px)   arc(px)  arc(blocks)   lap(tk)  slip\n");
        circle_test(&ti, model, 33, top);
        circle_test(&ti, model, 66, top);
        circle_test(&ti, model, 100, top);

        /* 4b. THE SAME CIRCLE, on the original's two-point geometry. */
        hdr("full-lock circle, TWO-POINT PROTOTYPE (the original's shape)");
        printf("  %%top  speed(px/t)  radius(px)  radius(blk)  lap(ticks)\n");
        twin_circle(&ti, model, 33, top);
        twin_circle(&ti, model, 66, top);
        twin_circle(&ti, model, 100, top);
        printf("  expected from the original: R = 0.94 * L = %d px\n",
               (gta_car_world_len(ci) * 94) / 100);

        /* 4c. THE ROAD SNAP. Point the car a few degrees off a cardinal,
         * hold the throttle, touch nothing else, and tell the model it is on
         * a road: the original drags it straight and then locks it. Run it
         * twice, once with the assist and once without, because the number
         * that matters is the difference. */
        hdr("road snap - a few degrees off north, hands off the wheel");
        {
            int road, a0;
            for (road = 0; road <= 1; road++) {
                for (a0 = 3; a0 <= 9; a0 += 3) {
                    int locked = -1;
                    gta_veh_init(&v, &ti, model, 0, 0, a0);
                    for (t = 0; t < 600; t++) {
                        gta_veh_step(&v, 1, 0, 0, 0, road);
                        if (locked < 0 && (v.ang16 & 0xFFFFL) == 0
                            && (gta_veh_angle(&v) & 63) == 0)
                            locked = t;
                    }
                    printf("  %-8s start %2d/256 (%2d deg) -> after 600 ticks "
                           "%3d/256; snapped exact at tick %s\n",
                           road ? "on road" : "off road", a0, a0 * 360 / 256,
                           gta_veh_angle(&v),
                           locked >= 0 ? "yes" : "never");
                }
            }
        }

        /* 4d. THE SKID FLAG and THE DAMAGE DERATE, both new and both cheap
         * to check: how many steps of a corner and of a handbrake turn the
         * game would be laying tyre marks for, and what a wrecked engine
         * does to the top speed. */
        hdr("skid flag, and what damage does to the engine");
        {
            int marks = 0, hb_marks = 0;
            gta_veh_init(&v, &ti, model, 0, 0, 0);
            settle(&v, 2000, 0, 0);
            for (t = 0; t < 300; t++) {
                gta_veh_step(&v, 1, 0, 1, 0, 0);
                if (v.sliding) marks++;
            }
            gta_veh_init(&v, &ti, model, 0, 0, 0);
            settle(&v, 2000, 0, 0);
            for (t = 0; t < 300; t++) {
                gta_veh_step(&v, 1, 0, 1, 1, 0);
                if (v.sliding) hb_marks++;
            }
            printf("tyre marks: %d of 300 ticks cornering, %d of 300 with the "
                   "handbrake\n", marks, hb_marks);
            {
                static const int dmg[] = { 0, 25, 50, 75, 100 };
                int k;
                printf("top speed by damage: ");
                for (k = 0; k < 5; k++) {
                    gta_veh_init(&v, &ti, model, 0, 0, 0);
                    v.damage = dmg[k];
                    top = settle(&v, 3000, 0, 0);
                    printf("%d%%->%ld.%02ld  ", dmg[k], WHOLE(top), FRAC2(top));
                }
                printf("px/step\n");
            }
        }

        /* 4e. REVERSE, held. The original caps only the tank's FORWARD
         * speed and nothing at all backwards, so a car held in reverse
         * accelerates until its own rolling resistance stops it. That is
         * worth a number rather than a shrug, because it is the one place
         * this model can surprise a player. */
        hdr("reverse, held from a standing start");
        {
            long back = 0;
            int stable = 0;
            gta_veh_init(&v, &ti, model, 0, 0, 0);
            for (t = 0; t < 4000; t++) {
                long s;
                gta_veh_step(&v, 0, 1, 0, 0, 0);
                s = speed_of(&v);
                if (s > back + 256) { back = s; stable = 0; }
                else if (++stable > 25) break;
            }
            print_speed("backwards   ", back);
            printf("reached in  %d ticks (%d.%02d s)\n", t,
                   t / TICKS_PER_SEC, (t % TICKS_PER_SEC) * 2);
        }

        /* 5. THE HANDBRAKE TURN - the signature move of the original. */
        hdr("handbrake turn");
        gta_veh_init(&v, &ti, model, 0, 0, 0);
        settle(&v, 1500, 0, 0);
        {
            int maxslip = 0, turned = 0, prev = gta_veh_angle(&v);
            for (t = 0; t < 200; t++) {
                int a, d, s;
                /* Throttle HELD, because that is how the move is actually
                 * done: you keep the power on and yank the handbrake. Off
                 * the throttle the handbrake is just a very good brake and
                 * the car stops before it has finished rotating. */
                gta_veh_step(&v, 1, 0, 1, 1, 0);
                a = gta_veh_angle(&v);
                d = (a - prev) & 255; if (d > 128) d -= 256;
                turned += d; prev = a;
                s = slip_deg(&v);
                if (s > maxslip) maxslip = s;
                if (speed_of(&v) < 13107) break;
            }
            printf("held %d ticks, rotated %d/256 (%d degrees)\n",
                   t, turned, turned * 360 / 256);
            printf("slip angle  %d degrees at most\n", maxslip);
            print_speed("speed left  ", speed_of(&v));
        }
    }

    gta_tiles_free(&ti);
    return 0;
}
