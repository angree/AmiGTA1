/* Vehicles in the world - the fleet, as opposed to the definitions.
 *
 * `gta_car_info` (gta_car.h) is a MODEL: the Bedford's mass, the bus's doors.
 * `gta_car` here is one of them standing in a particular street at a
 * particular angle. Same split as a sprite record against a drawn sprite.
 *
 * Portable C89, no floats, no Amiga headers - the same rules as the renderer
 * and gta_player.c, and for the same reason: the host tools step this a
 * thousand ticks in a second and the emulator is the last place a placement
 * bug should be found.
 *
 * THE CARS DRIVE, AND THEY DRIVE THE ORIGINAL'S WAY. Since 2026-08-22 this is
 * a reading of GTA 1's own AI driver rather than a model of our own: a car is
 * given a DESTINATION and a route of blocks to it, it STEERS towards the next
 * one at a fixed number of angle units a tick, and its speed comes from how
 * many blocks of road are clear ahead. The road direction it is on is derived
 * from its heading, not the other way round. The notes carry the write-up and
 * the behaviour each rule came from; the constants below carry the arithmetic
 * that turns the original's units into ours.
 *
 * WHICH WAY A CAR FACES is the same constant as the player's, and that is not
 * an assumption. Car art cannot be read directly - a top-down saloon is very
 * nearly symmetric front to back - so it was read through the man sitting in
 * it: ped frame 97 is `sitting_in_car`, it is drawn unrotated with the driver
 * reaching DOWN the screen to his steering wheel, and a driver faces his car's
 * front. So an unrotated car faces south, exactly like an unrotated pedestrian
 * (the notes, "the sprite art faces SOUTH").
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_TRAFFIC_H
#define GTA_TRAFFIC_H

#include "gta_car.h"
#include "gta_map.h"
#include "gta_nav.h"
#include "gta_route.h"
#include "gta_tiles.h"
#include "gta_render.h"

/* The renderer takes GTA_MAX_SPRITES per frame and walks the list once per
 * layer, so the fleet is deliberately smaller than that budget - the player
 * and, later, pedestrians need room in the same list. */
#define GTA_MAX_CARS 20

/* THE FLEET FOLLOWS THE CAMERA.
 *
 * A car further than DESPAWN blocks from the camera is removed, and the fleet
 * is topped back up in the band SPAWN_LO..SPAWN_HI. Those numbers are set by
 * the screen rather than by taste: 320x200 at the default zoom is about +-4.7
 * blocks across and +-2.9 down, so a band starting at 7 is always out of
 * sight - a car appearing from nothing in front of the player is worse than no
 * car - and a despawn at 14 is far enough that one cannot be dropped while it
 * is still visible even zoomed right out.
 *
 * SPAWN_TICKS throttles the top-up. The band scan walks a few hundred map
 * blocks and the tick runs at 50 Hz, so doing it every tick would cost more
 * than driving the whole fleet does. Twice a second is far quicker than cars
 * can leave the area. */
#define GTA_TRAFFIC_DESPAWN     14
#define GTA_TRAFFIC_SPAWN_LO     7
#define GTA_TRAFFIC_SPAWN_HI    11
#define GTA_TRAFFIC_SPAWN_TICKS 25

/* AND THOSE THREE ARE THE NUMBERS FOR THE DEFAULT ZOOM ONLY.
 *
 * They were written as "always out of sight" and they are not: the camera
 * zooms continuously from 64 pixels a block down to 8, and at 8 the screen is
 * forty blocks across - so a despawn at 14 blocks removes cars in plain view
 * and a spawn ring at 7 pops them into existence in the middle of the picture.
 * That is what "cars disappear here and there" was.
 *
 * The fleet therefore follows the VIEW rather than a constant.
 * gta_traffic_set_view_blocks() takes the half-width of the screen in blocks
 * and everything is derived from it, so the defaults above are what the
 * formulas produce at the default zoom (half-width 5): despawn 14, ring 7..11.
 * The margin is generous on purpose - the 2.5D projection pushes higher layers
 * outward, so a block that is off the edge by the flat arithmetic can still
 * have its roof on screen. */
#define gta_traffic_despawn_blocks(v) ((v) + 9)
#define gta_traffic_ring_lo(v)        ((v) + 2)
#define gta_traffic_ring_hi(v)        ((v) + 6)

/* FOLLOWING AND TURNING, all in 16.16 world pixels or per tick at 50 Hz.
 *
 * LOOKAHEAD is how far down its own lane a car bothers to look - three blocks,
 * which at 2.5 blocks a second is well over a second of warning.
 *
 * FOLLOW_GAP is where it starts easing off, MIN_GAP is where it stops, and
 * between the two the wanted speed is scaled linearly by how much of the gap is
 * left. Both are bumper to bumper, so a bus gets the room a bus needs without
 * anything here knowing about lengths.
 *
 * THE GAP HAS TO EXCEED THE STOPPING DISTANCE or a car arrives inside the one
 * in front however hard it brakes afterwards. The first numbers - a 6-pixel
 * minimum and a gentle brake - gave a stopping distance of about ten pixels
 * from full speed, so cars kept touching: the overlap test reported "55 px
 * apart, need 61" and similar near misses on a third of the runs. Easing over
 * a 32-pixel span rather than braking at a threshold fixes it properly, and
 * the span being a power of two means the scaling is a multiply and a shift
 * with no division anywhere.
 *
 * ACCEL and BRAKE are per tick. Braking is harder than accelerating - true of
 * cars, and it is also what stops a queue concertina-ing. */
#define GTA_TRAFFIC_LOOKAHEAD  (128L << 16)

/* HOW FAR AHEAD, IN TICKS, THE RECTANGLE IN FRONT OF A CAR LOOKS.
 *
 * The rectangle is swept along the RELATIVE velocity, so what it really
 * measures is time to contact, and a horizon in ticks is the honest unit for
 * that. A second is about right: a cruising car covers two blocks in it, which
 * is further than it needs to stop, and it is short enough that a vehicle
 * crossing a junction ahead has cleared before it counts.
 *
 * Measured with no horizon at all (the full 128-pixel lookahead, swept along
 * this car's own heading and ignoring the other one's): every car in the city
 * braked for the cross traffic at every junction and the fleet went from 96%
 * moving to 30%. */
#define GTA_TRAFFIC_HORIZON   12
/* AND THESE TWO ARE NOW THE ORIGINAL'S SPACING. Its ladder stops a car when
 * fewer than three blocks are clear ahead and eases it off below six, so a
 * queue in GTA 1 stands about three blocks apart. Ours were 80 and 16 pixels -
 * two and a half blocks and half a block - which under the new steering let
 * cars touch: the overlap test caught six of them in a 900-tick run, all a few
 * pixels short. One block to stop and three to start easing is both the
 * original's number and enough room. The span between them stays a power of
 * two so the easing is still a multiply and a shift. */
#define GTA_TRAFFIC_FOLLOW_GAP  (56L << 16)
#define GTA_TRAFFIC_MIN_GAP     (24L << 16)
#define GTA_TRAFFIC_GAP_SPAN    32          /* FOLLOW_GAP - MIN_GAP, in px */
#define GTA_TRAFFIC_GAP_SHIFT   5           /* and it is 1 << this, so no divide */
#define GTA_TRAFFIC_ACCEL       3000L
#define GTA_TRAFFIC_BRAKE      22000L

/* ...EXCEPT THAT EVERY CAR NOW USES ITS OWN, out of the car table.
 *
 * The two constants above were one number for the whole city. The original's
 * table has `accel` and `braking` per model - 10 to 25 and 10 to 150 in
 * style001 - and they are the game's own numbers, so traffic uses them rather
 * than a rate somebody picked. The units below turn a table entry into 16.16
 * world pixels per tick per tick, and they are chosen so that the COMMON car
 * comes out exactly where the tuned single rate had it: a saloon has accel 25
 * and braking 25, so 25 * 120 = 3000 and 25 * 880 = 22000. Nothing about the
 * traffic that was measured changes; a bus (accel 10) now takes two and a half
 * times as long to get going, and the sports car with braking 150 stops like
 * it should.
 *
 * The old constants stay as the fallback for a model with a zero entry. */
#define GTA_TRAFFIC_ACCEL_UNIT   120L
#define GTA_TRAFFIC_BRAKE_UNIT   880L

/* STEERING, AND THE ORIGINAL'S NUMBERS FOR IT.
 *
 * The original turns the wheel by 0x20 of its 0x400-unit circle every frame at
 * about 30 Hz, and clears the turn once the car has come round 0x100 (ninety
 * degrees). Ours is a 256-unit circle at 50 Hz, so the same TIME through a
 * corner is 0x20/0x400 * 256 * 30/50 = 4.8 units a tick: 5, which comes round
 * in 13 ticks - a quarter of a second, the same as the original's eight
 * frames.
 *
 * TURN_START is how close to the middle of a block the car has to be before it
 * begins the turn, which is what puts the arc across the junction rather than
 * after it. The original derives its own from the vehicle - half a block for
 * one driver type, the car's own dimension for another - so this is half a
 * block and a car's length is added on top of it. */
#define GTA_TURN_RATE      5
#define GTA_TURN_QUARTER  64            /* 90 degrees, in our 256ths */

/* A QUARTER ARC OF UNIT RADIUS, in 16.16: pi/2 << 16 = 102944.
 *
 * arc_len = radius_in_whole_pixels * GTA_ARC_QLEN, which for the largest
 * radius this port issues (29) is 2985376 - a fifth of the 32-bit range, so
 * the arithmetic below is safe on the Amiga where a long is four bytes. */
#define GTA_ARC_QLEN  102944L

/* Turning arc_s/arc_len into an angle without overflowing 32 bits.
 *
 *      phi_units = 64 * arc_s / arc_len = arc_s * 40.743 / radius
 *
 * (arc_s / radius) is at most GTA_ARC_QLEN, and 102944 * 5215 >> 7 is 4194164,
 * which is 63.998 of the 64 units wanted - two thousandths of an angle unit at
 * the end of the arc, and the landing is snapped exactly anyway. */
#define GTA_ARC_K      5215L
#define GTA_ARC_KSHIFT 7
#define GTA_TURN_START   (16L << 16)    /* half a block, 16.16 world px */

/* THE CORNER IS AN ARC OF A FIXED RADIUS, AND THAT IS THE POINT.
 *
 * A fixed turn RATE - which is what GTA_TURN_RATE was used for - does not
 * describe a corner. The radius it produces is speed over rate, so a vehicle
 * crawling through a junction cuts a five-pixel corner and one at cruise cuts
 * a thirteen-pixel one, out of the same block, and neither of them lands on
 * the lane it was aiming at. The car then arrives beside its lane and the lane
 * keeper drags it sideways to correct - "3 to 5 pixels outside the junction it
 * suddenly slides sideways", reported many times and finally reported clearly
 * enough to act on.
 *
 * So the RADIUS is the constant and the rate is derived from it every tick:
 *
 *      rate = speed * 256 / (2 * pi * radius)
 *
 * which makes the path the same arc whatever the speed does during it - the
 * car can brake half way round a corner and still come out on the line.
 *
 * TWENTY-NINE PIXELS, AND THAT NUMBER COMES OUT OF THE ORIGINAL'S PHYSICS
 * rather than out of ours. It was 8 - chosen to keep the corner shape the old
 * fixed rate happened to produce - until the original was read for it
 * (the notes, "THE ORIGINAL DOES NOT TURN THE WHEEL BY 0x20").
 *
 * The original has no rotation increment at all. Its car model carries a FRONT
 * point and a REAR point, moves each by `speed` along its own heading, and
 * takes the body angle from atan2(front - rear). It sets the front to
 * body + 3T and the rear to body - 2T with T = 0x20 of 0x400, i.e. +33.75
 * and -22.5 degrees, flat and instantly. That is a bicycle model, and its
 * radius is
 *
 *      R = L * (cos a + cos b) / (2 * (sin a + sin b)) = 0.935 * L
 *
 * with L the car's length. `+0x8c` is the half-length, and the vehicle records
 * in style001.gry put a standard car at L = 62 SOURCE px - so R = 58 source,
 * which at our half scale is 29 WORLD px. The 90-degree arc then consumes
 * almost exactly one 64-px block, which is what makes the original's corners
 * look gradual where ours snapped.
 *
 * THE DESIGN WAS ALREADY RIGHT AND ONLY THE NUMBER WAS WRONG: the original's
 * radius is speed-independent too, exactly as the constant-radius arc below
 * assumes. Ours was three and a half times too tight.
 *
 * MEASURED, 8 -> 29 over 96 sites: corner duration 8.9 -> 19.8 ticks
 * (0.17 -> 0.39 s), flow 87% -> 88%, turns on the line 32% -> 32%. Over the
 * four gate sites at 12000 ticks and four seeds: `BOX DEADLOCK` 33 -> 27
 * car-ticks, cars abandoned 20 -> 14, flow 84.1% -> 84.5%.
 *
 * ANYTHING FROM 20 UPWARDS BEHAVES IDENTICALLY, because the trigger cannot
 * fire earlier than the approach to the junction allows; 29 is kept because it
 * is the derived value rather than the smallest one that works. AND DO NOT
 * TAKE 16 ON THE STRENGTH OF THE SWEEP: it looks better there (24 car-ticks of
 * deadlock against 36) and is a disaster on the gate - 1113 car-ticks against
 * 27, with (204,108) alone contributing 873. Fourth time the 3000-tick sweep
 * and the 12000-tick gate have disagreed, and congestion is the gate's
 * question.
 *
 * STILL TWICE TOO FAST. The original takes about 20 frames for 90 degrees at
 * its turn speed 5, which at ~30 Hz is 0.7 s against our 0.39. The rest is in
 * the speed carried through the corner, not in the radius - see the "2 or 5"
 * rule in the AI driver.
 *
 * The trigger follows from it directly and is no longer a guess: a quarter
 * circle displaces the car by exactly the radius along its old heading, so the
 * turn must START exactly one radius before the centre line of the lane being
 * joined. See lane_target_at(). */
#define GTA_TURN_RADIUS   29

/* HOW FAR ALONG THE NEW LANE THE TURN READS ITS LANE TARGET, in blocks. The
 * block immediately past a crossing has the crossing on one side, so its kerb
 * test gives a different answer from the street a block further on - and the
 * lane keeper uses whatever block the car is actually in. Aiming at the first
 * answer therefore guarantees a second correction as soon as the car reaches
 * the second. See the trigger in drive_one. */
/* How far the aim walk will look for the straight road past a crossing. The
 * widest junction complexes in Liberty City run five blocks of + arrows. */
#define GTA_TURN_AIM_SCAN 5

/* The tightest arc a car will describe, for the case where it is already past
 * the ideal start point. Below this a corner stops looking like driving. */
#define GTA_TURN_RADIUS_MIN 3

/* AND THE MEASUREMENT THAT CHOSE IT. The old fixed rate with its
 * block-comparison trigger was kept behind a switch long enough to A/B it on
 * `drivesweep`'s lane-correction column, then deleted. 96 sites, 3000 ticks:
 *
 *                       flow   lane fix   OF WHICH AT A CORNER
 *      fixed rate        88%     72815          67299
 *      radius 8          87%     54629          45207
 *      radius 12         86%     50226          41584
 *      radius 16         86%     50741          43214
 *
 * **92% of every sideways correction in the city happens within forty ticks of
 * a corner**, which is the reported fault measured rather than described, and
 * it says the lane keeper was never the problem. Radius 8 is the one kept: it
 * takes a third off that number for one point of flow, where 12 takes 38% for
 * two points and more overlaps.
 *
 * What it does NOT fix is the overlap count, which rises from 95 to 122 - the
 * cars now commit to the new lane decisively instead of drifting into it, and
 * that exposes the missing give-way when merging. Concentrated at the same
 * corners as ever: (132,36) and (60,156). See START HERE item 1. */

/* How long after a corner a sideways correction still counts as that corner's
 * fault. At a walking pace through a junction, forty ticks is most of a block
 * - comfortably past the "3 to 5 pixels outside the junction" where the slide
 * was reported. */
#define GTA_AFTER_TURN   40

/* 256 / (2 * pi) = 40.7437, as a fraction that keeps a 32-bit multiply in
 * range on the 68020: 163/4 is 40.75, which is 0.02% high and cannot be told
 * apart from exact at one pixel a tick. */
#define GTA_TURN_K_NUM   163
#define GTA_TURN_K_SHIFT   2

/* WHEN A TURN IS ISSUED - and it is a CLOSED LOOP, which is the thing this
 * port had wrong and the reason cars came out of junctions in the wrong lane.
 *
 * The AI driver does not decide "I am half a block from the corner, turn
 * now". Every frame it takes the point one lookahead in front of the car,
 * works out which block that point is in, and compares it with the block of
 * the route node it is driving to - on the CROSS axis only:
 *
 *      case N: if (node.x < ahead.x) turn(-0x20); if (ahead.x < node.x) turn(+0x20);
 *      case S: if (node.x < ahead.x) turn(+0x20); if (ahead.x < node.x) turn(-0x20);
 *      ...the same on y for W and E...
 *
 * and re-issues that request, zeroing the turn accumulator, for as long as the
 * two disagree. The turn is only cleared once 0x100 (90 degrees) has
 * accumulated SINCE THE LAST ISSUE and the direction field has changed.
 *
 * So the car steers until its own lookahead point is in the lane it is aiming
 * at, and a corner that comes out short or wide is corrected on the next
 * frame rather than left. An open-loop "turn exactly 90 degrees starting half
 * a block out" - which is what was here - lands wherever the speed happened to
 * put it, and then something else has to drag the car sideways onto the lane.
 * That drag is what was photographed at a junction with an ambulance nose to
 * nose with a tanker.
 *
 * The lookahead is the original's: half a block for the simple driver type,
 * the vehicle's own half-length for the route-following ones, so a bus starts
 * its turn earlier than a Mini. Half a block is 0x20 there and 16 here. */
#define GTA_TURN_LOOKAHEAD 16           /* world pixels, the plain saloon */
#define GTA_TURN_LOOKAHEAD_MAX 28

/* KEEPING TO THE LANE, and the two numbers here are the original's.
 *
 * The car's position WITHIN its block - `pixel % 0x40` in a 64-pixel world,
 * `% 32` in ours - is compared against a target offset (`car+0x107`) with a
 * dead band either side (`car+0xbf`), and while it is outside that band the
 * whole car is translated sideways by the band's width a frame, without being
 * turned. Once it is back inside, the correction stops until it drifts out
 * again.
 *
 * THE TARGET IS THE BLOCK CENTRE AND NOTHING ELSE. `0x1f` of `0x40` is the
 * value written everywhere in the original for ordinary traffic; the only
 * other values in the whole game are 7 and 0x37, and those belong to
 * the pull-over, which is a car pulling over to the kerb - a manoeuvre, in
 * its own state (`car+0x11f == 1`), not how traffic drives.
 *
 * That is the fix for the junctions, and it is a DELETION. This port aimed at
 * the block centre plus a nudge away from the kerb, sampled from the blocks
 * either side - and at a junction there is road on all four sides, so the
 * nudge vanished for those blocks and came back afterwards. The car was
 * dragged four pixels sideways on the way in and four back on the way out,
 * which is the "driving diagonally to line up" that was reported, and which
 * put cars from two different lanes onto the same line in the middle of a
 * crossing. The original never looks at the neighbouring blocks at all, so it
 * cannot have the bug: a lane's position is a constant offset in a block.
 *
 * The dead band is what stops the correction being applied every single tick -
 * a permanent slow drag sideways - and the original's is 4 of its 64 pixels,
 * which is 2 of our 32. */
#define GTA_LANE_TARGET  16             /* world px into the block: the centre */
#define GTA_LANE_BAND     2             /* dead band either side of the line */
#define GTA_LANE_STEP     1             /* the slide per tick, in px - see the
                                         * keeper for why it is not the band */

/* --- THE STEERING CONTROLLER (gta_car.line_*, steer_along_line) ----------
 *
 * HOW FAR AHEAD IT AIMS, in world px: the pursuit point sits this far along
 * the line from the car's own projection onto it. Short and the car
 * over-steers and weaves; long and it cuts corners into the line lazily.
 * Scaled with speed - a car covers L in L/v ticks whatever it is doing - and
 * clamped to a sane band: 24 px is three quarters of a block, 64 px is two.
 * GTA_PURSUIT_K is px of lookahead per whole px/tick of speed. */
#define GTA_PURSUIT_K    24
#define GTA_PURSUIT_MIN  24
#define GTA_PURSUIT_MAX  64

/* THE TURN RATE, AND IT IS PHYSICS RATHER THAN A KNOB. A car at speed v with
 * turning radius R turns at v/R radians per tick; in this port's 256-step
 * circle that is v * (256 / 2pi) / R = v * 40.74 / R steps. The constant is
 * 40.74 * 64 = 2608 so that `(speed >> 6) * GTA_STEER_W` lands in 16.16
 * steps per tick without overflowing a 32-bit multiply on the 68020.
 *
 * The floor lets a car that is barely rolling still creep its heading round -
 * without it a car stopped against something could never steer out. The
 * ceiling is the same clamp a knocked car's spin uses, so a steered car can
 * never out-turn a rammed one. */
#define GTA_STEER_W      2608L
#define GTA_STEER_W_MIN  (1L << 14)     /* a quarter step a tick */
#define GTA_STEER_W_MAX  (3L << 16)     /* 3 steps a tick, 4.2 degrees */

/* HOW WRONG THE HEADING HAS TO BE before the controller holds the car down to
 * cornering speed, in whole steps of 256. 16 is 22 degrees: a car easing onto
 * its lane is well inside it and keeps its speed; a car coming back from a
 * shove, or turning round, is outside it and slows down first. */
#define GTA_STEER_SLOW   16

/* HOW LONG A CAR STAYS LOOSE AFTER BEING HIT, and how fast it settles.
 *
 * PROVISIONAL - these three are the shape of the thing, not the original's
 * numbers; those are being established separately. What is NOT provisional is
 * that the state has to exist: without it a hit car cannot move
 * sideways or turn at all, whatever the impulse says.
 *
 * The damping is per tick, in Q8: 246/256 leaves about 55% of the speed after
 * fifteen ticks, so a shunted car coasts to a stop in about a second rather
 * than stopping dead or sliding for ever. */
/* How hard a hit has to be before it knocks a car loose rather than merely
 * shoving it. Two world pixels a tick of closing speed - a car being leant on
 * at parking speed stays on its lane, a real collision does not.
 * PROVISIONAL, like the three below. */
#define GTA_KNOCK_HARD    (2L << 16)
/* HOW MUCH OF ITS OWN VELOCITY A CAR KEEPS THROUGH A COLLISION - and this one
 * is NOT a guess. The original's car-vs-car branch multiplies both velocity
 * components of the car that was hit by 0.5 exactly. Q8, so 128. */
#define GTA_KNOCK_KEEP    128
/* THE ORIGINAL'S CAR-VS-CAR CONSTANTS - the game's own numbers, not values
 * tuned by hand until the result looked right.
 *
 *   0.023809524 = 1/42   the push divisor
 *   20.0                 the striker's mass is clamped to this
 *   0x1398c4  0.5                  a light car keeps half its own velocity
 *   0x41700000 immediate  15.0     "light" means mass below this
 *
 * These four are NOT guesses. GTA_HIT_SPIN below is the only number in the
 * collision that still is: the original divides the torque by an inertia read
 * from the car model (+0x78), a field this port's car table does not carry, so
 * the rod value mass*len*len/12 stands in for it and this scales the result. */
#define GTA_HIT_SCALE        42
#define GTA_HIT_MASS_CAP     20
#define GTA_HIT_LIGHT        15
/* The original's gate on the flat shove: `(|striker speed| >> 1) > 6`, i.e.
 * more than twelve of its own speed units. */
#define GTA_HIT_SHOVE_MIN    12
/* Radians (what the original's torque/inertia produces) into 16.16 of a
 * 256-step circle: 256/(2*pi) = 40.743, i.e. 2670177 in 16.16. Stored as
 * 2670177 >> 6 and shifted back at the point of use, so the multiply cannot
 * overflow 32 bits. NOT a free parameter - it is a unit conversion. */
#define GTA_HIT_SPIN         41721
#define GTA_KNOCK_SPIN_MAX   (3L << 16)  /* 3 of 256 a tick = 4.2 deg */

/* ONE RESPONSE PER CONTACT - the original's own rule. Its collision handler
 * latches the car on the way in and refuses to respond a second time until
 * the physics step clears the latch again. This port paid the impulse, the
 * spin, the shove and the aggressor's halving EVERY TICK the
 * boxes still overlapped: `hitcar` measured ONE bus-into-saloon contact as
 * "cars knocked loose 13" and the victim's velocity was thirteen impulses
 * deep. While the latch is up the pair still separates positionally - bodies
 * may not rest inside each other - but pays nothing else. Six ticks: long
 * enough that a 50-percent-a-tick separation has undone the overlap, short
 * enough that a genuine second collision still lands. */
#define GTA_HIT_LATCH        6

/* HOW MUCH OF THE OVERLAP ONE TICK UNDOES, and how much may stand.
 *
 * The canonical stability recipe (Box2D / Catto GDC06; Gaul's tutorial; see
 * the project notes): positional correction is a PERCENTAGE of
 * the penetration beyond a small SLOP, never the whole of it. Undoing 100
 * percent in one tick - which is what this port shipped - overshoots, the
 * AI closes the gap again next tick, and the pair visibly jumps apart and
 * back: "teleportuje w te i we wte". Half per tick clears a real overlap in
 * two or three ticks and cannot oscillate; the one-pixel slop lets a queue
 * rest bumper to bumper without being ground apart every tick.
 *
 * GTA_SEP_SLOP is in the Q14 pixels box_mtv() reports depth in. The percent
 * is applied as a shift at the two call sites: depth << 2 was 100 percent
 * (the << 2 is the Q14-to-16.16 conversion), << 1 is 50. */
#define GTA_SEP_SLOP      (1L << 14)     /* 1 px, Q14 */
/* Above this depth the correction stops being 50 percent and closes the
 * whole excess: the band [slop..deep] is where contact is allowed to live,
 * so a driven-in body settles at ~4 px instead of sinking to 13, and a
 * shallow touch is still handled gently enough not to jump. */
#define GTA_SEP_DEEP      (2L << 14)     /* 2 px, Q14 */

/* THE TIMER IS THE BACKSTOP, NOT THE EXIT. A knocked car is meant to leave
 * the loose state when it has STOPPED SLIDING (GTA_KNOCK_REST below); the
 * timer only catches a car that somehow never slows. At 25 this was inverted:
 * kvx damps by 246/256 a tick, so a 4 px/tick shunt takes ~77 ticks to reach
 * the rest threshold, and the 25-tick timer cut nearly every knock off at
 * ~1.9 px/tick - the car clicked back onto the rails in mid-slide and the AI
 * darted it straight back into lane. That was the developer's "auta za
 * szybko probuja wrocic na swoj tor". 25 was chosen when spin ran away
 * ("baczki"); komega has had its own damping and clamp since, so the reason
 * for 25 is gone and 75 restores rest-speed as the normal exit. */
#define GTA_KNOCK_TICKS   75
/* GTA_RECOVER_TICKS and GTA_RECOVER_STEP were here: a timer and a rate that
 * walked the drawn heading back to the compass after a knock, while the old
 * lane keeper slid the body sideways at a quarter rate. Both are gone with
 * the steering controller (PROGRESS.md 130) - a shunted car keeps the
 * heading the collision gave it and turns back onto its lane line at v/R,
 * which is one mechanism instead of two and is an arc instead of a diagonal.
 * Named here because the numbers are quoted in older entries. */
#define GTA_KNOCK_DAMP    246           /* Q8, per tick, on the ROLLING share
                                         * of kvx/kvy - along the car's axis */
/* ...AND ON THE LATERAL SHARE, across the axis, where the tyres are: 200/256
 * loses a fifth a tick, so a car dragged sideways is still within three
 * ticks. That is the "duzy drag (bo opony hamuja)" a pushed car needs: the
 * player leaning on a flank moves it little, leaning on a nose rolls it. */
#define GTA_KNOCK_SIDE_DAMP 200
#define GTA_KNOCK_SPIN    235           /* Q8, per tick, on komega - the
                                         * original does not damp angular
                                         * velocity at all in the rigid body;
                                         * the tyres do it when the car lands
                                         * back on the rails. 253/256 lets a
                                         * slew carry, which is what a corner
                                         * hit is supposed to look like. */
/* Below this the car is not really moving and may as well be back on rails. */
#define GTA_KNOCK_REST    13107L        /* 0.2 px/tick */

/* SPEED, IN THE ORIGINAL'S UNITS.
 *
 * Its traffic runs on a small integer: cruise 6, changed by one a frame, and
 * forced to 2 or 5 through a turn. One of those units is one world pixel a
 * frame on a 64-pixel block at 30 Hz, which on our 32-pixel blocks at 50 Hz is
 * 0.3 of a pixel a tick, or 19660 in 16.16. Everything below is therefore the
 * original's own number times this.
 *
 * A car still cannot exceed its model's own top speed from the car table, so a
 * bus is slower than a saloon - the original's cruise is one number for all
 * traffic, and keeping the table's is both closer to what the file says and
 * the difference the player can see. */
#define GTA_SPEED_UNIT      19660L      /* one of the original's speed units */

/* WHAT COUNTS AS A CRASH, rather than as leaning on a bumper. A closing
 * speed of a whole world pixel a step is a knock worth denting; below that
 * the cars still push each other apart, they just do not bill each other.
 * And having been billed, a car is left alone for half a second - the
 * original gets the same effect from a one-shot impulse latch it applies
 * and clears, which this port has no equivalent of. */
#define GTA_RAM_HARD        65536L      /* 1.0 px a step, closing */
#define GTA_RAM_COOL        25          /* ticks, half a second at 50 Hz */
#define GTA_SPEED_CRUISE    6           /* what its traffic drives at */
#define GTA_SPEED_TURN_TIGHT 2          /* through the tighter turn */
#define GTA_SPEED_TURN_WIDE  5          /* through the straighter one */
/* THE SPEED A COMMITTED CORNER IS FINISHED AT.
 *
 * The arc is a path, so a car that stops half way round stops ON the path -
 * diagonally across a junction, where `angle` is a rounded guess at a
 * direction it is not travelling in and every other rule in this file reads
 * `angle`. Measured: with nothing enforcing this, one site went from 74% of
 * the fleet moving to 22% and the crossing at (60,44) locked solid for 28494
 * car-ticks, because the first car to stop mid-corner blocked the box and
 * everything behind it queued.
 *
 * The bumper gap still overrides it - driving into the car in front is worse
 * than any of this - which is why the floor is conditional below. */
#define GTA_SPEED_ARC_MIN    2

/* HOW MUCH THE SWEEP TEST GROWS THE BOXES BY, in world pixels.
 *
 * The other car is not going to stand still while this one goes round the
 * corner, and the corner takes 30 to 50 ticks. Two pixels is about one tick of
 * a cruising car and is enough to stop the two sprites touching without
 * refusing every turn in a busy street. */
#define GTA_SWEEP_SLACK      2

/* THE ROAD A TURN NEEDS IN FRONT OF WHERE IT LANDS, in world pixels.
 *
 * A car may not commit to a corner that drops it on somebody's bumper. Half a
 * block is about one car: enough that the vehicle joins a moving lane rather
 * than the back of a stationary one, and not so much that a busy street
 * refuses every turn. */
#define GTA_TURN_CLEAR      16

/* HOW FAR A CAR MUST TRAVEL AFTER A CORNER BEFORE IT MAY TAKE ANOTHER, in
 * world pixels.
 *
 * `allow_turn` on its own - back to 1 only on a block that is not part of a
 * junction - is right for an ordinary crossroads and wrong for the wide ones,
 * where a car can be inside the box for four blocks and a second, perfectly
 * legal corner is refused. Measured after the spawn fix put cars back in the
 * inner lanes: 531 to 629 turns missed a site.
 *
 * A lane change taken as right-then-left happens inside a block and a bit -
 * that is what makes it a lane change rather than a corner - so a block and a
 * half of travel separates the two cases without knowing anything about the
 * shape of the junction. */
#define GTA_TURN_AGAIN      48

/* WHERE THE "NO LEFT TURN FROM THE RIGHT-HAND LANE" RULE COMES FROM.
 *
 * NOT from the map's direction bits. `gtadump dirmap` settles that: a block on
 * a straight run of road carries exactly ONE of them, the way the lane runs -
 *
 *      44 <<<<<<++++<<<<<<<<<+      < = W only
 *      46 ------++++---------+      - = E only,  + = the junction
 *
 * - and the junction blocks carry several. There is nothing in `type_map` that
 * says "from this lane you may go straight or right but not left". The arrows
 * the player reads are TILE ART; the data has no opinion.
 *
 * What the data DOES give is which blocks are lanes of the same carriageway:
 * a neighbour to the side that is road and carries the same direction bit. So
 * the rule is geometric and it is the one a driver actually obeys: you turn
 * left from the leftmost lane and right from the kerb lane, and if there is
 * another lane of your own carriageway on the side you are turning towards,
 * that turn is not yours to take. */
#define GTA_LANE_TURN_RULE  1

/* HOW LONG A CAR WAITS FOR ITS ARC TO CLEAR BEFORE TAKING IT ANYWAY.
 *
 * Refusing a blocked corner outright deadlocks: the car stops at the line, and
 * if what is in the way is itself waiting for something, neither ever moves -
 * measured at 26% of the fleet moving where 97% had been. Half a second of
 * patience and then going regardless breaks every one of those, at the cost of
 * an occasional graze, which is the right way round. */
#define GTA_SWEEP_PATIENCE  25

/* HOW LONG A CAR MAY SIT HALF WAY ROUND A CORNER BEFORE IT CREEPS OUT ANYWAY.
 *
 * The bumper gap outranks the arc floor, which is right - driving into the car
 * in front is worse than anything - but in a jam EVERY car has something
 * inside its gap, so the first vehicle to stop mid-arc stops diagonally across
 * the crossing and the whole district locks behind it. Measured at (204,108):
 * 379 ticks per corner and 10% of the fleet moving.
 *
 * A second of that is a queue. Ten seconds of it is a wedge, and a wedge only
 * comes apart if somebody moves. So after this long the car finishes its arc
 * regardless, at the crawl of GTA_SPEED_ARC_MIN. It may graze what is in front
 * of it; a grazed sprite for a few ticks is a great deal better than a city
 * that never moves again. */
#define GTA_ARC_CREEP       1

/* AND THE LAST RESORT, six seconds in.
 *
 * Creeping only while the way is physically clear keeps the overlap count
 * where it belongs, and it cannot break a wedge where two cars really are
 * touching - measured at one site going to 20% of the fleet moving where the
 * old code managed 36%. A car that has been stationary in the middle of a
 * crossing for this long is not queueing; it is the thing everybody else is
 * queueing for. It finishes its arc regardless, and the graze that costs is
 * cheaper than the district. */
#define GTA_ARC_UNWEDGE   300

/* HOW MUCH ROAD HAS TO BE CLEAR AHEAD, in blocks, and what happens when it is
 * not. Straight out of the tail of the original's AI driver:
 *
 *      clear < 6 and moving well   -> ease off
 *      clear < 4                   -> ease off hard
 *      clear < 3                   -> stop
 *      clear > 3 and slow          -> speed up
 */
#define GTA_CLEAR_EASE   6
#define GTA_CLEAR_HARD   4
#define GTA_CLEAR_STOP   3

/* How far away a car's next destination is, in blocks. Far enough that it
 * drives somewhere rather than round the corner, near enough that the route
 * stays inside the search window and inside GTA_ROUTE_MAX nodes. */
#define GTA_ROUTE_TARGET_LO 10
#define GTA_ROUTE_TARGET_HI 20

/* HOW MANY NODES BEFORE THE END A CAR ASKS FOR ITS NEXT ROUTE.
 *
 * One breadth-first search runs per tick for the WHOLE fleet - the original has
 * the same rule and the same reason - so with twenty cars a request can wait
 * twenty ticks, and at a block every twenty ticks that is a whole block of
 * driving with no route at all. A car that runs out just before a junction is
 * therefore handed its new route just AFTER it, and `gtadump turntrace` shows
 * exactly that: a vehicle told to turn when it is already fourteen pixels past
 * the point where the turn should have started. The arc is then perfect and
 * still lands off the lane, because it began in the wrong place.
 *
 * Asking early costs no extra searches - the same car asks the same number of
 * times, just sooner - and four nodes is about eighty ticks of margin against a
 * worst-case twenty-tick wait. */
#define GTA_ROUTE_REFILL 0

/* TRAFFIC LIGHTS.
 *
 * The map marks them: `type_map_ext` bits 0..2 hold the traffic hint and 1 is
 * a traffic light. `gtadump lights` shows what that looks like on the ground -
 * the hint runs round the OUTSIDE of a junction, on the kerb blocks where the
 * post stands and on the road blocks beside them, which are the stop lines:
 *
 *      2 ,,,,1,,,,,,1,,,,,,        18 and 25 are the two kerbs
 *      3 ----1------1------        row 3 is a westbound lane
 *      6 ----1+++++-1------        the + blocks are the junction box
 *      7 ,,,,11111111,,,,,,
 *
 * A car stops on a hinted block only when the block AHEAD of it is a junction,
 * which is what makes the near side of the crossing a stop line and the far
 * side just a piece of road - both carry the hint.
 *
 * THE CYCLE IS OURS, not the original's: the phase length and the amber gap
 * were picked to look right and are not read out of the game. What IS the
 * original's is where the lights are and which blocks they stop. Neighbouring
 * junctions are offset by half a cycle so the city does not blink in unison -
 * the offset comes from the block coordinates, so it costs nothing to store
 * and is the same on the host and on the Amiga. */
#define GTA_LIGHT_PHASE  250      /* ticks of green for one axis, 5 s at 50 Hz */
#define GTA_LIGHT_AMBER   30      /* all-red at the end of a phase, 0.6 s */

/* HOW FAR OFF A BLOCK'S CENTRE THE OUTER LANE SITS.
 *
 * A lane is a block, but the OUTERMOST lane of a road is not the whole block:
 * measured off the artwork, the solid edge line is painted 14 source pixels
 * into the block, so the drivable part of that lane is 50 pixels of a 64-pixel
 * block and its centre is 7 source pixels - 3.5 world - further in than the
 * block's own centre.
 *
 * That is a small number and it shows anyway, because a car is 50 source
 * pixels wide in a 64-pixel lane: there is barely three pixels of paint either
 * side of it, so any error at all puts a wheel on the line. Hence the nudge,
 * applied only where the block beside it is not road. */
#define GTA_TRAFFIC_KERB_NUDGE  ( 4L << 16)

/* The same number as a lane target offset in whole world pixels, which is what
 * gta_car.lane_target is measured in. 14 source pixels of edge line leaves 50
 * drivable of 64, so the lane centre is 7 source - 3.5 world - off the block
 * centre; 4 is that rounded to the pixel grid the lane keeper steps on. */
#define GTA_LANE_KERB     4

/* HOW FAR ALONG THE STREET A JUNCTION BLOCK MAY LOOK for the lane offset it
 * cannot work out for itself - see lane_target_at().
 *
 * Liberty City's widest crossings are two blocks deep, and a step along the
 * direction of travel keeps the same cross axis, so three is one more than is
 * ever needed and still stops well before it could wander into the next
 * junction and answer with the wrong street's geometry. */
#define GTA_LANE_STREET_SCAN 4

/* HOW WIDE THE FOLLOW WINDOW IS, in 16.16 world pixels - see gap_ahead().
 *
 * Narrower than a lane on purpose: a wider one makes every car brake for
 * traffic in the NEXT lane wherever two lanes run side by side, which is most
 * of downtown, and that has been measured twice and costs far more than the
 * pile-ups it prevents. The second constant is the exception, for a car with a
 * turn under way - it is off the lane line by definition and is the one thing
 * twelve pixels genuinely cannot see. */
#define GTA_FOLLOW_SIDE      (12L << 16)

/* HOW MANY BLOCKS MUST BE REACHABLE from a block before a car is parked on it.
 * See can_get_away(): the map has pockets a car can drive into and never out
 * of - the layer-3 flyover at rows 93-104 is twelve blocks of one-way lane
 * that never join and whose ramps carry no direction bits - and a vehicle put
 * in one stops for ever and queues everything behind it. Twenty-four is twice
 * the size of the largest pocket found, so it separates a trap from a street
 * without the test needing to know what a viaduct is. */
#define GTA_REACH_MIN 24

/* HOW LONG A CAR WAITS TO TURN BEFORE IT PUSHES IN.
 *
 * Two cars can each be waiting for the block the other is about to leave and
 * neither ever moves. After this many ticks the car drops the merging
 * clearance and asks only whether the block is physically occupied, which is
 * what a driver does at a junction that will not clear.
 *
 * AND IT ONLY WORKS TOGETHER WITH THE CROSS-TRAFFIC FIX in gap_ahead(). Added
 * on its own it changed nothing at all - the same 0 to 9 cars stalled - so it
 * was very nearly deleted as unproven. Removing it AFTER the cross-traffic fix
 * put the stalls back up to 6 of 20 where they had been 0 or 1. Neither is
 * sufficient; both together are. A fix tested in the presence of another bug
 * can look like it does nothing. */
#define GTA_TRAFFIC_PATIENCE   150

/* HOW MANY CROSSINGS MAY BE HELD AT ONCE, and for how long a claim survives
 * without being renewed. See the claim table in gta_traffic below.
 *
 * The TTL only matters when the holder disappears - despawned off screen,
 * compacted out of the fleet - because a car renews its claim on every tick it
 * spends on a junction block. Three seconds is longer than any crossing in
 * Liberty City takes to drive and short enough that a lost claim cannot hold a
 * junction for a noticeable time. The original uses 60 of its frames. */
/* ONE SLOT PER CAR, so the table can never fill.
 *
 * It was EIGHT, for twenty cars, and `junction_claim()` FAILS OPEN when there
 * is no slot left - it returns "yes, go" rather than refusing. That is a safe
 * default only while the table is bigger than the demand for it. The moment
 * turning cars started booking their crossing on the approach block, demand
 * went past eight and the arbitration quietly switched itself off: shared-box
 * car-ticks went UP, from 8101 to 8767 at (64,64) and to 21152 at (50,44),
 * from a change that can only ever reduce sharing. A hundred and eighty bytes
 * removes the whole failure mode. */
/* Room for every car to hold its whole path through a crossing - up to four
 * junction blocks plus the LANDING block beyond - see claim_route(). A table
 * that runs out FAILS OPEN, which silently switches the whole reservation
 * off, so it is sized for the worst case with headroom. */
#define GTA_CLAIM_MAX  (GTA_MAX_CARS * 12)
#define GTA_PATH_MAX   4

/* THE JUNCTION OCCUPANCY MATRIX.
 *
 * The developer's design, and it is the right one:
 *
 *   "jak jest skrzyzowanie 4 lane z kazdego kierunku [...] to takie
 *    skrzyzowanie ma 16 pol. zrob taka matryce i albo kazdy kwadrat w tej
 *    matrycy ma 0 albo 1 i ma zapisany numer pojazdu ktory robi mu 1, i jak
 *    ten pojazd wyjedzie ze skrzyzowania to zwalnia sie automatycznie,
 *    wszystko po czym przejedzie i opusci ten kwadrat tez robi sie 0"
 *
 * One entry per junction block a vehicle's BODY is standing on, holding that
 * vehicle's serial. Rebuilt from scratch every tick, so a cell frees itself
 * the moment the car is off it and the vehicle behind can follow through
 * immediately - no timers, no release call, nothing to leak.
 *
 * BY THE BODY, NOT THE CENTRE, which is the half the port kept getting wrong:
 * a tanker is sixty world pixels long against a thirty-two pixel block, so it
 * stands on two or three of them and the ones that are not its middle were
 * reading as empty. "niektore auta sa dluzsze, moze nie bierzesz tego pod
 * uwage" - it was not.
 *
 * Nine cells is a vehicle's own block plus the ring around it, which is more
 * than the longest bus can reach. */
#define GTA_OCC_PER_CAR  9
#define GTA_OCC_MAX      (GTA_MAX_CARS * GTA_OCC_PER_CAR)

/* THE CORNER IS BOOKED BEFORE IT IS TAKEN, AND LOOKED AT BEFORE THAT.
 *
 *   "zakret danego pojazdu zajmij dopiero jak sprawdzi czy jest wolne miejsce
 *    tam gdzie chce zajac. inaczej sie zatrzymuje czekajac na zwolnienie (z
 *    lekkim zapasem dystansu zeby mogl plynnie zakrecic, wiec musi zwolnic
 *    wczesniej ciut)"
 *
 * GTA_TURN_LOOK is that margin, in world pixels: the car starts asking whether
 * its corner is free this far BEFORE the point it would commit at, so that a
 * refusal is a car easing off rather than a car stamping on the brakes on the
 * white line.
 *
 * GTA_LONG_CAR is the length above which a vehicle is given the two extra
 * cells on the OUTSIDE of its corner - "dlugie pojazdy potrzebuja tez wolnego
 * miejsca na zewnetrznym pasie zeby miec miejsce na skret". A bus sweeps
 * ground a saloon never touches and has to own it before it starts. */
#define GTA_TURN_LOOK    24
#define GTA_LONG_CAR     44
#define GTA_ARC_CELLS    12

/* NOT a lifetime. A booked square is released when the owner's body has
 * covered it and left it (the sweep in gta_traffic_tick) - the developer's
 * rule, no timer under a standing car. This countdown only reaps a booking
 * whose owner never reached it at all (rerouted, reversed away): ~10 s. */
#define GTA_CLAIM_TTL  500

/* HOW LONG THREE CARS MUST BE STUCK TOGETHER IN ONE CROSSING before it counts
 * as a deadlock rather than a queue - see gta_traffic.stat_boxlock.
 *
 * 50 ticks is one second at the 50 Hz tick. A queue clears in well under that;
 * a cycle never clears at all. Counting every tick of overlap instead made the
 * instrument report 27 -> 1542 for a change that HALVED the cars which never
 * got out, which is the opposite of what it exists to say. */
#define GTA_BOXLOCK_HOLD 50

/* HOW CLOSE TO A JUNCTION A CAR STOPS WHEN SOMEBODY IS CROSSING IT.
 *
 * The distance to the far edge of the block the car is in, so a car this far
 * out is about to enter the crossing. It has to be short - a car that starts
 * refusing a block and a half back is the reservation that was deleted for
 * making the whole city stand still - and long enough that the brake can
 * actually stop the vehicle before its nose is in the box. Twelve pixels is
 * about a third of a block. See box_busy(). */
/* HOW FAR BEFORE A CROSSING THE ENTRY QUESTION IS ASKED.
 *
 * It was TWELVE PIXELS, and that is the whole of "cars enter a few pixels and
 * stop". `edge` is measured from the car's MIDDLE, so a sixty-pixel bus asked
 * whether it could go when its nose was already eighteen pixels inside the
 * junction - far too late either to stop or to reserve anything.
 *
 * The question has to be asked while there is still room to stop: half the
 * vehicle's own length, so its nose is still outside, plus a stopping
 * distance. The car keeps asking every tick from there, moving or stopped, and
 * goes the instant the answer is yes. */
/* HOW FAR BEFORE A CROSSING THE ENTRY QUESTION IS ASKED.
 *
 * It was TWELVE PIXELS, and that is the whole of "cars enter a few pixels and
 * stop". `edge` is measured from the car's MIDDLE, so a sixty-pixel bus asked
 * whether it could go when its nose was already eighteen pixels inside the
 * junction - far too late either to stop or to reserve anything.
 *
 * The question has to be asked while there is still room to stop: half the
 * vehicle's own length, so its nose is still outside, plus a stopping
 * distance. The car keeps asking every tick from there, moving or stopped, and
 * goes the instant the answer is yes. */
/* HOW FAR BEFORE A CROSSING THE ENTRY QUESTION IS ASKED.
 *
 * It was TWELVE PIXELS, and that is the whole of "cars enter a few pixels and
 * stop". `edge` is measured from the car's MIDDLE, so a sixty-pixel bus asked
 * whether it could go when its nose was already eighteen pixels inside the
 * junction - far too late either to stop or to reserve anything.
 *
 * The question has to be asked while there is still room to stop: half the
 * vehicle's own length, so its nose is still outside, plus a stopping
 * distance. The car keeps asking every tick from there, moving or stopped, and
 * goes the instant the answer is yes. */
#define GTA_TRAFFIC_BOX_ENTRY  (12L << 16)
/* Sixteen pixels, not forty. The distance only has to cover the BRAKING - a
 * cruising car does 1.8 px a tick and stops in four - because HALF THE
 * VEHICLE'S LENGTH is added on top, and that is what keeps its nose outside
 * the crossing while it asks. At forty, cars booked crossings from two blocks
 * out and held them while they queued: (50,44) fell from 66% of the fleet
 * moving to 34%. */
#define GTA_TRAFFIC_BOX_LOOK   (16L << 16)

/* How many junction blocks deep the entry scan looks before giving up on
 * seeing the far side of a crossing. Liberty City's widest junction complexes
 * run five blocks of `+` arrows (around (213,117)), so a scan that stops short
 * cannot tell "the exit" from "still inside the crossing" - see the
 * `found_exit` logic in drive_one for what that cost. */
#define GTA_BOX_SCAN 5

/* AND DO NOT ENTER A JUNCTION YOU CANNOT CLEAR.
 *
 * How much room there has to be in front before a car commits to a crossing:
 * enough to be all the way through it and still keeping its distance, so a
 * block plus the following gap.
 *
 * This is the rule that breaks a GRIDLOCK RING, which is a different thing
 * from two cars wanting the same square and is not fixed by box_busy(). The
 * sweep found one at (108,108): an eastbound queue along row 114, a northbound
 * queue up column 117, a westbound queue along row 103 and a southbound queue
 * down column 116 - four streams around one city block, each one's head
 * blocked by the next one's tail, all four stopped for good. Every car in it
 * had entered its junction perfectly legally. What none of them did was ask
 * whether there was anywhere to go on the FAR side.
 *
 * IT WAS REJECTED TWICE ON A TEST THAT WAS TWENTY TIMES TOO SHORT, and that
 * is the lesson of the night rather than the rule itself.
 *
 * Measured over 600 and 900 ticks it looked useless or harmful - 42% against
 * 42% on the ring it was written for, and downtown seed 1 falling from 90% to
 * 78%. But a ring takes a minute of game time to form, and 900 ticks is
 * eighteen seconds: the tests were stopping before the thing being fixed had
 * happened. The developer saw it on the emulator because he waited, and said
 * so - "in 30-120 secs traffic stops due to conflict on the crossroads".
 *
 * Over 12000 ticks, which is four minutes:
 *
 *                        without it        with it
 *      (64,64)      68%, stood 78.8 s   94%, stood 6.0 s
 *      (61,52)      51%, stood 85.6 s   68%, stood 13.6 s
 *
 * and without it BOTH sites end with all twenty cars stopped for good. This is
 * the difference between a city and a car park.
 *
 * The general form of the mistake is already in this codebase's notes from
 * 2026-08-21 - "a fix tested in the presence of another bug can look like it
 * does nothing" - and this is the same shape with a horizon instead of a bug.
 * Traffic changes are now measured over thousands of ticks, not hundreds. */
#define GTA_TRAFFIC_CLEAR_BOX  (48L << 16)

/* STUCK RECOVERY, and it is the last thing standing between a jam that clears
 * and a jam that does not.
 *
 * The original runs a small state machine on `ped+0x11` whenever a driver is
 * not getting anywhere. It checks five blocks each way for road and for
 * obstacles; if it can, it REVERSES - `speed = -4`, turn request -0x20 - and
 * waits for the manoeuvre; if it cannot, it turns the car round on the spot;
 * and it gives up after 0x32 (50) frames of trying.
 * **It never deletes the car.**
 *
 * IT WAS PORTED AND IT WAS MEASURED AND IT IS NOT IN THE CODE, twice over.
 *
 * Attempt one reversed whenever the block behind was road. A reversing car
 * backs into the queue behind it, so THAT car cannot move either and reverses
 * in its turn, and the whole street walks backwards: the gap hold went from
 * 22285 car-ticks to 57435 and (64,64) from 94% moving to 56%.
 *
 * Attempt two required all five blocks behind to be road AND empty, which is
 * what the original's stuck handler actually asks - it tests each of the five
 * for cars as well. Rarer, and worse:
 *
 *                    no reverse   attempt 1   attempt 2
 *      (64,64)          94%          56%         73%
 *      (61,52)          68%          72%         17%
 *      (108,228)         -            -          41%
 *
 * all over 12000 ticks. Two independent forms, both net losses, so it was
 * removed rather than tuned a third time.
 *
 * WHY IT PROBABLY DOES NOT TRANSFER: the original reverses a car that is stuck
 * against SCENERY or a wreck, and its junctions are held by the traffic-light
 * machinery (an 88-entry array of claimed junctions) so its rings are rarer to
 * begin with. Ours reverses cars that are stuck against
 * each other, which is the one case where giving ground moves the problem
 * rather than solving it.
 *
 * WHAT WOULD SETTLE IT: reverse only the car at the BACK of a queue - one with
 * nothing within a block behind it and something within a block in front - and
 * only one car per jam per second. That is a different rule from the
 * original's and would need its own measurement over 12000 ticks at (64,64),
 * (61,52) and (108,228), which are the three sites the numbers above come
 * from. The constants below are left for whoever does it. */
#define GTA_TRAFFIC_STUCK        150
#define GTA_TRAFFIC_REVERSE      50
#define GTA_SPEED_REVERSE         4

/* WHEN A CAR GIVES UP.
 *
 * A vehicle that has stood at a genuine dead end this long is removed, even if
 * the player can see it. That is a deliberate exception to "cars are only
 * retired off screen", and it is the lesser of two visible evils: the street
 * it is standing in has one lane, so everything behind it queues for ever, and
 * a permanently jammed avenue is both more visible and more wrong than one
 * vehicle disappearing after six seconds of not moving.
 *
 * It is rare and it is always the same shape: a long vehicle that turned into
 * a street whose far end it cannot leave. Buses are 120 pixels - nearly four
 * blocks - and the map has corners they cannot get round. Deciding that eight
 * blocks earlier would need a path search; this needs a counter. */
#define GTA_TRAFFIC_GIVEUP     300

/* AND THE BACKSTOP: a car that has not travelled a single BLOCK in this many
 * ticks is removed, whatever it thinks is holding it up and whether or not the
 * player can see it. Thirty seconds.
 *
 * This is a safety net rather than a model, and it is here because the map
 * contains places a car can legally reach and then never legally leave. The
 * clearest is the viaduct at column 213, rows 93-104: layer 3 carries four
 * lanes with proper arrows, so cars spawn on it, but both of its ramps are
 * PAVEMENT blocks with no direction bits (`gtadump column 213 92`), so
 * `nav_step_layer` will not cross them. Every car that lands up there drives
 * to the end, stops for ever, and queues the whole viaduct behind it - 49761
 * car-ticks of "road ahead" at (204,108) and cars standing for 209 seconds.
 *
 * The proper fix is to stop SPAWNING on a stretch that has no exit, which
 * needs a reachability test at placement time. Until then this bounds the
 * damage: nothing can block a street indefinitely. Thirty seconds is far
 * longer than any legitimate wait - the longest honest one measured anywhere
 * in the city is about thirteen. */
#define GTA_TRAFFIC_ABANDON   1500



/* HOW A CAR DRIVES - and this is the ORIGINAL's model, read out of the game
 * rather than invented here. The notes carry the evidence for every claim
 * below.
 *
 * A CAR IS STEERED, NOT STEPPED. It has a heading `face` in 256ths of a turn
 * and it moves along it; a turn is a REQUEST (`turn`, a few units a tick) that
 * the heading follows round, so a corner is an arc and not a right angle. The
 * road direction `angle` - one of N/E/S/W - is DERIVED from the heading's
 * quadrant every tick, exactly as the original derives its own direction field
 * from the car's rotation. Everything else (following, junction claims, the
 * lights) still asks `angle`, so it did not have to change.
 *
 * A CAR FOLLOWS A ROUTE. It is given a destination and a path of blocks to it
 * (gta_route.h), and it steers towards the next node. That is the biggest
 * thing this port had wrong: traffic used to pick a legal direction at random
 * at every junction, which is a shuffle, and the original's randomness is in
 * WHERE a car is going rather than in what it does at each corner. When the
 * route runs out it asks for another, and one search runs per tick for the
 * whole fleet - the original has the same single-search rule.
 *
 * ONE BLOCK IS ONE LANE, and the map says which way that lane goes. Traffic
 * keeping right is not something the code has to arrange - the direction bits
 * have already arranged it, with the northbound and southbound lanes of an
 * avenue being different blocks. A car therefore belongs in the MIDDLE of its
 * block; an earlier version shifted it a quarter block to the right of its
 * heading and put every car on the lane line. See lane_offset().
 *
 * CARS FOLLOW EACH OTHER rather than colliding: a car looks along its own lane
 * for the one in front, matches its speed, and keeps a gap. */
/* gta_car.hold - why a car is standing still. */
#define GTA_HOLD_NONE      0
#define GTA_HOLD_QUEUE     1    /* the car in front; the normal one */
#define GTA_HOLD_LIGHT     2    /* red light at a stop line */
#define GTA_HOLD_BOX       3    /* junction box claimed by somebody else */
#define GTA_HOLD_MERGE     4    /* no room to turn into the next lane */
#define GTA_HOLD_DEADEND   5    /* no legal heading at all, not even a U-turn */
#define GTA_HOLD_ROAD      6    /* the road ladder: too little clear ahead */
#define GTA_HOLD_GAP       7    /* the bumper-to-bumper gap to the car ahead */
#define GTA_HOLD_STUCK     8    /* backing out of a jam - see GTA_TRAFFIC_STUCK */
/* HOW FAR A BODY MAY BE MOVED BY THE SEPARATION IN ONE TICK.
 *
 * This is Box2D's `b2_maxLinearCorrection` and it exists for exactly the
 * fault the developer reported: "gdy pukne auto z boku jakby teleportuje sie
 * o jakas ilosc pikseli". A striker at 20 px a tick opens seven pixels of
 * overlap in the tick it arrives, and paying all seven back at once IS a
 * teleport - one frame, seven pixels, no motion in between. Paid at two
 * pixels a tick instead, the same overlap clears in four ticks and reads as
 * a shove.
 *
 * The literature is unanimous on the shape of this (Catto's Box2D, Gaul's
 * tutorials, Chou on slops): correct a PERCENTAGE of the penetration beyond
 * a slop, split by inverse mass, after the impulse, and clamp it. The port's
 * own numbers were chosen by measurement; see PROGRESS.md. */
#define GTA_SEP_MAX_PX     2

/* ...and how far the one that DROVE INTO IT may be backed out in the same
 * tick. It is generous on purpose: a car that over-drove into another by ten
 * pixels has to be put back, or the pair simply passes through each other
 * (`ramsweep`, 208 of 2888 runs when both sides were capped at two). Being
 * stopped short is what hitting something feels like; it is the VICTIM
 * jumping that reads as a teleport. */
#define GTA_SEP_STRIKER_PX 3

/* HOW CLOSE COUNTS AS TOUCHING. The sweep stops a body clear of another one,
 * so the collision test has to be slightly generous or nothing ever registers
 * as a hit. Two pixels, because the box tests work in whole pixels. */
#define GTA_TOUCH_PX       2

/* WHAT A CAR CANNOT DO IN ONE TICK, and therefore what counts as a bug when
 * it happens: turn more than this many of the 256 steps (24 = 34 degrees, and
 * the fleet's own corner is 4 a tick), or move more than this many pixels
 * (the top speed is about six). Both are counted in gta_traffic.stat_face_jump
 * / stat_pos_jump so a fault the developer can see has a number. */
#define GTA_SANE_TURN      24
#define GTA_SANE_STEP      10

/* A car is a write-off at a hundred points of damage, and burns this long
 * before it explodes.
 *
 * EIGHTY TICKS, NOT FORTY. Forty is 0.8 s at the 50 Hz tick, and the
 * developer's verdict on it was that it "does not give you time to get away".
 * A burning car is a warning, and a warning you cannot act on is just a
 * delayed explosion; 1.6 s is two or three running strides.
 *
 * AND THEN 120: "wydluz czas palenia sie auta jeszcze o 50%" after playing
 * with the eighty. 2.4 s. */
#define GTA_CAR_WRECKED    100
#define GTA_CAR_FUSE       120

/* HOW FAR FROM THE CAMERA A WRECK IS SWEPT UP, in blocks, and why this one
 * number does NOT follow the zoom the way the ordinary despawn radius does.
 *
 * An ordinary car is traffic: it is generated around the view and retired
 * around the view, so its radius has to track how much city is on screen or
 * cars pop in and out where the player can see it. A wreck is a THING THE
 * PLAYER MADE. It has to still be there when he comes back round the block,
 * and it has to go away eventually or a long session fills the fleet with
 * scrap. Tying that to the zoom would mean a wreck vanishing because the
 * player pressed the zoom key, which is the opposite of a landmark.
 *
 * Twenty blocks is about two screens at the shipped zoom - "one screen's
 * length from the wreck to the edge of the screen", as it was asked for -
 * and it is a constant. */
#define GTA_WRECK_KEEP_BLOCKS 20

#define GTA_HOLD_COUNT     9

/* How many people in the road the fleet will look at in one tick. More than
 * the pedestrian pool holds, so a full crowd always fits. */
#define GTA_MAX_WALKERS   16

/* HOW LONG A CAR STANDS BEHIND SOMETHING BEFORE IT TRIES THE NEXT LANE, and
 * how far it is allowed to slide sideways to get there.
 *
 * A car held by the gap on open road has nothing to wait for: whatever is in
 * front is parked, wrecked, or somebody standing in the street, and none of
 * those is going to move. The original's traffic goes round. Two seconds is
 * long enough that a normal queue at a light never triggers it - a light
 * cycles faster than that - and short enough that a blocked street clears
 * while the player is still looking at it.
 *
 * The move itself is a target one block to the side; the lane keeper that
 * already exists steers the car over. It is only offered when that block is
 * road, carries the car's own direction (so it is the next lane of the same
 * carriageway and not oncoming traffic), and has nobody in it. */
#define GTA_LANE_SWAP_WAIT  100
#define GTA_LANE_SWAP_COOL  150

/* HOW LONG A LANE CHANGE MAY TAKE before it is abandoned. A change is a line
 * and a controller rather than a path, so nothing makes it finish on its own:
 * a car held by something else half way across would keep the other lane's
 * line indefinitely and sit between the two. Two seconds is far longer than
 * the manoeuvre needs at any speed a fleet car drives at - and it also
 * covers a U-turn, which uses the same state and is a half circle at
 * cornering speed: measured at 74 ticks in `gtadump steertest`. */
#define GTA_LANE_SWAP_MAX   200

/* HOW FAR DOWN THE NEW LANE IT LOOKS before pulling into it, in blocks. A
 * lane that is clear alongside and blocked a bus length on is not a way
 * past anything: filmed with both lanes blocked, a car changed into one,
 * met the obstruction, changed back, and would have weaved between the two
 * for ever - every change legal by the beside-test alone. It is also what
 * makes turning round reachable, because the U-turn is what a car does
 * when neither lane will have it. */
#define GTA_LANE_SWAP_LOOK  3

/* HOW MANY BLOCKS LEFT THE U-TURN LOOKS for the oncoming carriageway. Not
 * one: Liberty City's main streets are two lanes each way, so from the
 * inner lane the other side is TWO blocks away, and a test that looked only
 * at the block next door refused a U-turn on precisely the roads that are
 * wide enough for one. Three is a three-lane carriageway and further than
 * any road in the shipped maps. */
#define GTA_UTURN_SCAN      3

typedef struct {
    long x, y;              /* 16.16 world pixels, same units as the camera */
    int  layer;             /* map layer the car occupies */
    int  angle;             /* the ROAD direction: 0 N, 64 E, 128 S, 192 W */
    int  model;             /* index into gta_tiles.cars */
    int  remap;             /* index into that model's remap8, -1 for none */
    long speed;             /* CURRENT, 16.16 world px per tick */
    long top;               /* what it would do with the road to itself */
    long accel, brake;      /* its own, from the car table; per tick */

    /* THE HEADING THE CAR ACTUALLY DRIVES ALONG, 0..255 (gta_trig.h). It used
     * to be a drawn-only eased copy of `angle`; now it is the other way round
     * and `angle` is its quadrant. The position follows this, so the track
     * through a corner is the arc the steering describes. */
    int  face;

    /* The turn being made: which way the heading is moving, how far it has
     * come SINCE THE REQUEST WAS LAST ISSUED, and the road direction it was
     * issued in. The last two are what clear it - 90 degrees accumulated and
     * the direction field changed - and because the
     * driver re-issues the request and re-zeroes the accumulator every tick
     * that it still disagrees with the route, a corner taken short or wide
     * simply carries on until it is right. */
    int  turn, turn_accum, turn_from;

    /* The part of an angle unit the car has turned but not yet been credited
     * with, 16.16. The steering rate is speed divided by a fixed radius (see
     * GTA_TURN_RADIUS), so it is a fraction rather than the whole number of
     * units the old fixed rate gave, and throwing that fraction away every
     * tick would shorten every corner by up to a unit a tick - a quarter of
     * the turn. `turn` is now just the DIRECTION, +1 or -1. */
    long turn_frac;

    /* Ticks since this car finished a turn, saturating. Only the tests use it:
     * lane correction applied just after a corner is the reported fault ("3 to
     * 5 pixels outside the junction they slide sideways"), while correction
     * applied anywhere else is a car legitimately moving between lanes whose
     * targets differ. A single total cannot tell those apart, and tuning
     * against a single total tunes against the wrong thing. */
    int  since_turn;

    /* THE SLIDE AFTER A CORNER, per car - see gta_traffic.stat_slide.
     *
     * `slide_pos` is the cross-axis coordinate at the tick the arc finished
     * and `slide_ang` the road direction it finished on, because which axis
     * the lane lives on depends on that. `slide_left` counts the ticks down
     * and the measurement is taken when it reaches zero. Re-armed by a second
     * corner inside the window, which is correct: the slide belongs to the
     * corner it followed. */
    long slide_pos;
    unsigned long slide_serial;   /* the car that armed it - see below */
    int  slide_ang, slide_left, slide_dir;

    /* Was this car following a ROUTE when it began its last turn, or the
     * arrow-following fallback? Tests only. The suspicion was that a car whose
     * route arrives late turns late and lands off its lane; splitting the
     * corner correction by this is what settles whether that is true rather
     * than plausible. */
    int  turn_routed;

    /* The lane offset the turn AIMED at, kept so the moment the arc finishes
     * can be split into two completely different faults:
     *
     *   - the car did not land on the line it was aiming at   (geometry), or
     *   - it landed on it and the lane keeper wants a different line anyway
     *     (the aim was at the wrong target).
     *
     * Everything measured so far has been the CORRECTION the keeper applies,
     * which is the sum of both and cannot tell them apart - and four
     * hypotheses have now been tried and refuted against that one number. */
    int  turn_aim_tgt;
    int  turn_aim_bx, turn_aim_by;   /* the block it was aimed at - see
                                      * gta_traffic.stat_aim_block_wrong */

    /* THE RADIUS THIS PARTICULAR ARC IS BEING DRIVEN AT, in world pixels.
     *
     * A fixed radius still needs the turn to START at exactly one radius from
     * the lane line, and the trigger can only be tested once a tick while the
     * distance falls by a whole step - so a car fires anywhere in a step-wide
     * window and lands anywhere in a step-wide window. That is why "some
     * corners need no correction and some do, and it is not obvious which":
     * it depends on where the tick boundary happened to fall.
     *
     * So the radius is not fixed - it is set to the distance the car ACTUALLY
     * has when the turn is issued. A quarter circle displaces the car by
     * exactly its radius along the old heading, so an arc of radius `dist`
     * started at `dist` lands exactly on the line, every time, whatever the
     * speed and wherever the tick fell. */
    int  turn_radius;

    /* THE CORNER IS A PATH, NOT A STEERING RATE.  See GTA_ARC_QLEN.
     *
     * A quarter circle, fixed the moment the turn is committed: centre, radius
     * and the lane line it has to land on. Every tick the car is PLACED on that
     * circle at the arc length it has travelled - it is not integrated forward
     * from where it was. The speed therefore decides only how far round the car
     * has got, never where the circle is, which is the whole fault: with a rate
     * and an integrator, a car that braked mid-corner (for the vehicle in front,
     * every time) drew a tighter circle and came out beside its lane. */
    long arc_cx, arc_cy;    /* centre of the arc, 16.16 world px */
    long arc_s;             /* arc length travelled so far, 16.16 */
    long arc_len;           /* total arc length, 16.16 = r * GTA_ARC_QLEN */
    long arc_line;          /* the lane line it lands on, 16.16, cross axis */
    /* THE BOOKED SHAPE IS BINDING. Set when the gate books a crossing:
     * the square the path bends on and the direction it exits by
     * (255 = booked straight, no bend allowed this crossing). While
     * c->crossing the drive follows THIS, not the route - so the booked
     * squares and the driven squares are the same thing by construction. */
    int bend_bx, bend_by;
    int bend_dir;
    /* The square the car stood on when its booking was granted. The
     * commitment is PRESENCE: it lasts while the car is on this square or
     * on one of its own booked junction squares, and ends the moment it
     * is on neither - the developer's design, no edge detection. */
    int book_ax, book_ay;
    /* THE CONVOY: the id of the route this car is riding - its own serial
     * normally; after joining a leader's identical booking, the shared id
     * (= the current TAIL's serial, which is whose body-leave releases).
     * book_lx/ly is the booking's landing square, the "same exit" test. */
    unsigned long convoy;
    int book_lx, book_ly;
    /* Overlay colour slot 0..19, unique among LIVING cars - assigned at
     * spawn as the first slot no living car wears, so two cars on screen
     * can never share a colour. */
    unsigned char ov_col;
    /* last gate refusal, for the SIB probe: 1 body-in-line 2 first-square
     * 3 exit-full 4 no-room-past 15 claim 16 body-on-path 17 landing-not-
     * road 18 no-room-to-land 19 forced-no-landing; 0 = none */
    unsigned char why_box;
    signed char   why_side;             /* route side at that ask: 0/1/2/-1 */
    unsigned char why_fell;             /* turn shape failed, fell straight */

    /* Accumulated collision damage, Phase 5 item 3c. Grows with the impulse
     * of every hit the player lands on this car; nothing reads it yet except
     * the log - the visual deltas and the wreck state come later. */
    int damage;
    /* WHICH PANELS ARE DENTED - one bit per damage delta (gta_tiles.h), laid
     * over the car's sprite when it is drawn. The original dents the panel
     * that was struck, so a car remembers where it has been hit rather than
     * just how hard. */
    unsigned long dmg_bits;
    /* THE FUSE. A car at GTA_CAR_WRECKED burns for this many ticks and then
     * goes up; the original arms the same countdown when a blast writes a car
     * off, which is what makes a row of parked cars explode one after
     * another instead of all at once. 0 = not burning. */
    int fuse;

    /* Ticks left before this car can be charged for a collision again. It
     * stands in for the original's one-shot impulse latch: without it, a
     * player resting against a parked car bills it every tick for ever. */
    unsigned char ram_cool;

    /* Ticks left before this car can take a collision IMPULSE again - the
     * original's `car+0x230` latch (GTA_HIT_LATCH). ram_cool above gates only
     * the DAMAGE; this gates the physics: impulse, spin, shove and the
     * aggressor's halving all fire once per contact, not once per
     * overlapping tick. Positional separation is never gated. */
    unsigned char hit_latch;

    /* PARKED BY A PERSON, NOT BY THE SPAWNER.
     *
     * A car the player got out of is still a car - it sits where it was left,
     * it is drawn, it can be crashed into and it can be got back into. What it
     * does NOT do is drive off: nobody is in it. The traffic tick skips these
     * entirely, which is also why they cost nothing.
     *
     * Before this the car was simply deleted when the player got in and never
     * came back, so getting out made it vanish - reported from the screen as
     * "when we get out the car disappears". */
    unsigned char abandoned;

    /* BURNT OUT. What is left after the fuse above runs down: the car stays
     * exactly where it died, wearing every damage panel, solid, in the way,
     * and not going anywhere ever again.
     *
     * It used to be deleted at the moment of the explosion - "nie zostaja
     * wraki wybuchniete tylko auta znikaja po wybuchu" - which made a car bomb
     * a way of REMOVING traffic rather than blocking a street with it. A wreck
     * is `abandoned` too, so everything that already skips a driverless car
     * skips this one; `wrecked` is what stops the player getting into it and
     * what gets it swept up at GTA_WRECK_KEEP_BLOCKS instead of the ordinary
     * radius. */
    unsigned char wrecked;

    /* GOING ROUND WHAT IS IN THE WAY - see GTA_LANE_SWAP_WAIT.
     *
     * `swap_wait` counts ticks standing still behind something that will not
     * move. When it runs out and the next lane of the car's own carriageway
     * is empty, the car is GIVEN THAT LANE'S LINE (gta_car.lane_*) and drives
     * onto it under the ordinary controller - the manoeuvre is one decision,
     * not a path. It used to be a sideways slide, 1 px a tick for 32 ticks,
     * and the developer's verdict was "auta przesuwaja sie na pasach ...
     * nierealistycznie zamiast zakrecic poprawnie i przejechac".
     *
     * `swap` is now a STATE, not a countdown: 1 while a change is under way.
     * It matters for two things - the line is not re-read from the block
     * underneath while it stands (the car is deliberately between lanes), and
     * the lane beside is re-checked every tick so a change into a lane
     * somebody has just taken is abandoned rather than driven into.
     * `swap_bx/by` is the block that was aimed at, `swap_ticks` a timeout,
     * and (swap_sdx,swap_sdy) the way it went - one of them is zero, because
     * a lane is always across the direction of travel.
     *
     * `swap_cool` stops a car that has just changed lane from immediately
     * changing back, which is what a pair of blocked lanes would otherwise
     * produce: two cars weaving side to side for ever. */
    int  swap_wait;
    int  swap;
    int  swap_sdx, swap_sdy;
    int  swap_bx, swap_by;
    int  swap_ticks;
    int  swap_cool;
    /* WHAT gap_ahead() FOUND IN FRONT, this tick: 0 nothing, 1 somebody on
     * foot (or the player's car standing still), 2 a fleet car standing
     * still, 3 something moving. The lane change keys on 1 and 2, because
     * those are the two things waiting cannot fix; the hold labels cannot
     * be used for that - a car stopped behind a man on a junction block is
     * stamped GTA_HOLD_BOX by the box logic that ran before the gap did. */
    int  lead_kind;

    /* KNOCKED LOOSE BY A COLLISION.
     *
     * An AI car normally has no velocity VECTOR at all - it has a scalar
     * `speed` along `face`, which is why nothing could ever push one sideways
     * or spin it: there was nowhere to put the answer. While `knock` counts
     * down the car ignores its route, its lane and its gap, and simply moves
     * under kvx/kvy and turns under komega, both damped. When it reaches zero
     * the car goes back on the rails with whatever heading and speed it
     * finished with.
     *
     * `face16` is the heading at 16.16 resolution, needed only while loose:
     * `face` alone is 256 steps to the circle, and a spin quantised to 1.4
     * degrees a tick looks like a stutter rather than a spin. */
    int  knock;             /* ticks left loose, 0 = on the rails */
    /* KEPT AS A ZERO. The walk-back it counted is gone - the steering
     * controller brings a knocked car's heading round as part of driving
     * it back onto its lane - but the field is still written to 0 in two
     * places and read by the state bitmap the instruments print, and
     * removing it buys nothing. */
    int  recover;
    long kvx, kvy;          /* 16.16 world px per tick */
    long komega;            /* 16.16 of the 256-step circle, per tick */
    long face16;            /* heading, 16.16, while loose */

    /* ONE TURN PER CROSSING, and this is the crossing it has already used.
     *
     * Set to the junction root the car was standing in when its arc finished,
     * and cleared the moment it reaches a block that is not part of a
     * junction. While it holds, no second turn may be armed in that same
     * crossing - which is the "it turns twice in one junction" report: right
     * then left leaves the car pointing where it started, one lane over, in
     * the middle of the box.
     *
     * `turn_lock` before it named a DIRECTION and covered any junction block,
     * so it also refused perfectly legal corners at the NEXT crossing along -
     * measured at 111 of 111 missed turns on one gate. Naming the root instead
     * refuses exactly the second turn in one box and nothing else. */
    /* ONE TURN PER CROSSING. ONE BIT.
     *
     * 1 on ordinary road, 0 from the moment the car sets foot on a junction
     * block until it is back on ordinary road. A turn may only be COMMITTED
     * while it is 1 - which is the real rule of the road: you decide to turn
     * on the approach, and once you are in the box you are committed.
     *
     * Everything before this was cleverer and all of it was wrong. A lock
     * keyed on the junction ROOT and released on `turn == 0` fired on nothing
     * at all; the version that refused only the turn which UNDID the last one
     * let a car take FIVE right turns in one box, which is what the developer
     * photographed. The clever versions are gone. This is the whole rule. */
    int allow_turn;
    /* THE LAST LANE BLOCK THIS CAR STOOD ON - not part of a junction.
     *
     * The arrows a driver obeys are the ones painted on the LANE he is in, and
     * by the time a corner is committed the car is usually already over the
     * junction block, which carries every direction by definition. Asking the
     * block the car is on at that moment therefore asks a block with no
     * opinion, which is why the rule and its instrument both read "legal"
     * while the screen showed a car turning left out of a right-only lane. */
    int  lane_bx, lane_by;
    /* The last block with exactly ONE direction bit - a plain lane of a
     * straight run. `gtadump dirmap` shows why that is the test: straight
     * blocks carry one bit, junction blocks carry several. */
    int  appr_bx, appr_by;
    int  turn_chk_bx, turn_chk_by;  /* the block the arrow rule checked */
    int  turn_ahead;                /* the corner was set up a block early */
    int  turn_want_dir;             /* the direction it was committed for */
    long odo;               /* world pixels THIS car has travelled */
    long turn_free_at;      /* odo at which the turn bit may come back */

    int cross_lock_x, cross_lock_y;
    int cross_lock_dir;     /* +1 right, -1 left: which way that turn went */
    int  boxlock_ticks;             /* consecutive ticks stuck in a full box -
                                     * see gta_traffic.stat_boxlock */
    int  line_off, line_on_j, line_seen;   /* the lateral line, for the
                                            * street-against-crossing test -
                                            * see gta_traffic.stat_line_* */
    int  turn_ticks;                /* ticks this arc has been running - see
                                     * gta_traffic.stat_turn_ticks */
    int  turn_step;                 /* whole world px travelled per tick at the
                                     * moment the turn was ISSUED - the test of
                                     * whether the trigger`s tick is the fault.
                                     * See gta_traffic.stat_geom_by_step. */

    /* --- CROSSING A JUNCTION, measured end to end --------------------------
     *
     * The developer's instrument, and it asks the question the right way round:
     * a car is on a line BEFORE a crossing and on a line AFTER it, and what
     * matters is whether they are the same line. Everything measured before
     * this was the correction the lane keeper applied, which is a consequence
     * and mixes several causes together.
     *
     * `in_cross` is 1 between entering the first junction block and reaching
     * ordinary road again. `cross_pos` is the cross-axis world coordinate the
     * car came in on - its lane line - and `cross_ang` the direction it came
     * in travelling, so leaving can tell a straight-through from a turn.
     * `cross_worst` is the furthest it strays from that line WHILE INSIDE,
     * which is the "virtual carriageway centre" the crossing has no kerbs to
     * define. */
    /* COMMITTED TO THE CROSSING. Decided ONCE, on the approach, and never
     * re-opened until the car is out the far side.
     *
     *   "stoja poza skrzyzowaniem i sprawdzaja czy wolne np. 4 prosto [...]
     *    jesli potrzebuje 4 to rezerwuje 4 - nie ma prawa sie wtedy zatrzymac
     *    juz"
     *
     * The entry gate used to be evaluated every tick, including while the car
     * was already inside, so a crossing that became busy behind it stopped it
     * a few pixels past the line - which is the one place a car must never
     * stand. The gate now runs only while `crossing` is 0. */
    int  crossing;

    int  in_cross, cross_ang, cross_turns, cross_routed;
    long cross_pos, cross_worst;

    /* NINETY DEGREES IS THE MOST ANY CAR MAY TURN AT ONE JUNCTION, and this
     * remembers which way the last turn went so a second one the SAME way can
     * be refused: +1 right, -1 left, 0 free. Cleared as soon as the car is on
     * a block that is not part of a crossing.
     *
     * "In GTA NOTHING ever turned round at a junction - a turn was ninety
     * degrees at most" is the developer's report, and a U-turn here is almost
     * never issued as one. It is two ordinary ninety-degree turns, each of
     * them legal, one per block of a crossing, a second apart. Nothing that
     * looks at a single tick can see it, which is why it survived so long.
     *
     * A turn the OTHER way is still allowed, because that is a lane change
     * across a junction (right then left) and its net rotation is zero. Only
     * same-way-twice is a reversal. */
    int  turn_lock;

    /* KEEPING TO THE LANE, `car+0xbd` in the original: 0 for "in the lane",
     * otherwise the side it is drifting back from. It is a STATE rather than a
     * per-tick decision so that the correction has hysteresis - it starts when
     * the car leaves the dead band around the middle of its block and stops
     * when it is back inside, instead of pulling a little every tick for
     * ever. See GTA_LANE_TARGET. */
    int  lane_fix;

    /* WHERE IN ITS BLOCK THIS CAR'S LANE ACTUALLY IS, in world pixels -
     * `car+0x107` in the original, and the point is that it is a FIELD OF THE
     * CAR rather than something recomputed from the map every tick.
     *
     * That distinction is the whole junction bug. The outermost lane of a road
     * is not the whole block: the solid edge line is painted 14 source pixels
     * in, so the drivable part is 50 of the 64 and its centre sits about 3.5
     * world pixels further from the kerb than the block's centre does. This
     * port had that right and applied it by SAMPLING the blocks either side
     * every tick - and at a junction there is road on all four sides, so the
     * offset vanished for those blocks and came back afterwards. The car was
     * dragged sideways on the way in and back on the way out: the "cars leave
     * their lane at the junction and return" that was reported over and over,
     * and which put traffic from two streets onto one line in the middle of a
     * crossing.
     *
     * Deleting the offset outright fixed the junction and broke the straight,
     * which is what the developer then reported: "instead of fixing the wrong
     * position at the junction you broke the position on the straight, where
     * it was right". Both are true. The offset is real and belongs on the
     * straight; sampling it inside a crossing is what was wrong.
     *
     * So it is latched: updated only while the car is on a block that is NOT
     * part of a junction, and simply carried across the crossing. The original
     * cannot have the bug because it never looks at the neighbouring blocks at
     * all - a lane's position is a constant that belongs to the car. */
    int  lane_target;

    /* THE LINE THE CAR IS DRIVING ALONG, and the whole of its lateral
     * intention.
     *
     * Every sideways move used to be its own mechanism, and none of them was
     * a car turning its wheels: lane keeping SLID the body across, a lane
     * change slid it a pixel a tick for a block, the walk-back after a knock
     * turned the heading and slid at a quarter rate, and a corner PLACED the
     * car on a circle. The developer's verdict: "auta przesuwaja sie na
     * pasach ... nierealistycznie zamiast zakrecic poprawnie i przejechac".
     *
     * So there is one intention - a LINE - and one controller that steers
     * onto it (steer_along_line). Giving a car a different line is the whole
     * of "take that lane": it works out how to get there itself, from
     * wherever it is and however it got there, which is what the developer
     * asked for ("to nie moze byc zaprogramowane na twardo bo auto moze
     * puknac moze przesunac moze miec pas nagle zajety").
     *
     *   lane_axis  0: the line is x = lane_off, a north-south road.
     *              1: the line is y = lane_off, an east-west road.
     *   lane_off   the line's position on the cross axis, 16.16 world px,
     *              ABSOLUTE - block * 32 + lane_target, not an offset within
     *              a block. A line belongs to the STREET, not to whatever
     *              block the car happens to be in.
     *   lane_dir   0/64/128/192, which way along the line the car drives.
     *              A U-turn is this reversed and nothing else.
     *   lane_set   0 until somebody has given the car a line. */
    int  lane_axis;
    long lane_off;
    int  lane_dir;
    int  lane_set;

    /* WHERE IT IS GOING. `path` is the route from gta_route.h, `path_i` the
     * node being driven at, and `dest_x/dest_y` the destination the route was
     * asked for - kept so a car that loses its route can be given another to
     * the same place instead of wandering off. */
    gta_route_node path[GTA_ROUTE_MAX];
    int  path_n, path_i;
    int  dest_x, dest_y;
    int  want_route;        /* set when the route ran out or failed */
    int  route_cool;        /* ticks before it may ask again after a failure */

    /* The traffic hint of the block the car is standing in, and which block
     * that was. The hint is the one thing the navigation grid does not carry,
     * so it costs a map column walk - and the block only changes every twenty
     * ticks or so, which is what this remembers. */
    int  hint_bx, hint_by, hint_val;

    /* THE BLOCK THE CAR WAS IN LAST TICK, which is what `wait` is really
     * asking about. `wait` used to reset whenever the car moved at all, and in
     * a creeping queue a car inches forward every few ticks - so it never
     * accumulated the three seconds that lets it override the junction-box
     * rule and push in. At (204,108) cars stood for 213 seconds with the
     * escape hatch right there, resetting itself. Progress is measured in
     * BLOCKS travelled, not pixels. */
    int  last_bx, last_by;


    /* The block being driven INTO, and the point in it being aimed at - the
     * block's centre, shifted into the right-hand lane. */
    int  cell_x, cell_y;
    long tx, ty;

    /* Ticks spent waiting to turn. See GTA_TRAFFIC_PATIENCE. */
    int  wait;

    /* Ticks left of a reversing manoeuvre, 0 when not reversing. See
     * GTA_TRAFFIC_STUCK - this is the original's way out of a jam and the only
     * rule in the file that moves a car BACKWARDS. */
    int  reverse;

    /* Held at a red light this tick. Reporting only - "queueing", "stuck" and
     * "waiting at a light" are three different things and a test that cannot
     * tell them apart cannot say whether traffic is healthy. */
    int  at_light;

    /* WHY this car is not moving, for the same reason: a stopped car is a
     * question and this is the answer. Set every tick it stops. */
    int  hold;

    /* Set when the car has nowhere legal to go. NOTHING SETS THIS ANY MORE and
     * the field is kept because the tests and the draw path read it: a car
     * that ran out of road used to be deleted where it stood, in full view of
     * the player, which is the other half of "cars disappear here and there".
     * It turns round if it can and otherwise waits to be retired off screen
     * like everything else. */
    int  done;

    /* WAS THIS CAR OFF THE ROAD LAST TICK? Edge detection for
     * gta_traffic.stat_offroad_events - see there. One byte per car. */
    unsigned char offroad;      /* was the CENTRE off the road last tick */
    int  offroad_since;         /* tick the current excursion started */

    /* A turn was asked for in this block and has not happened yet - see
     * gta_traffic.stat_turn_missed. */
    int  turn_want, turn_want_bx, turn_want_by, turn_want_from, turn_want_routed;

    /* A NAME THAT SURVIVES THE FLEET BEING COMPACTED.
     *
     * `tr->cars[]` is compacted every tick when cars are retired, so index 3
     * is a different vehicle from one tick to the next and any test that
     * follows "car 3" is following a slot, not a car. That is fine for a
     * histogram and useless for the one question the developer keeps asking -
     * "what did THAT car just do?" - which needs a track through time.
     *
     * Costs one int per car and nothing per tick. */
    unsigned long serial;
} gta_car;

typedef struct {
    gta_car cars[GTA_MAX_CARS];
    int n;
    const gta_tiles *tiles;

    /* Deterministic, and its own rather than the C library's. The host harness
     * has to produce the same street of cars as the Amiga does or it stops
     * being an instrument, and `rand()` is not the same function in libnix as
     * it is in glibc. */
    unsigned long seed;

    /* Ticks left before the fleet is topped up again; see SPAWN_TICKS. */
    int spawn_wait;

    /* Simulation ticks since init. Drives the traffic lights, and being a
     * plain counter rather than a wall clock it is identical on the host and
     * on the Amiga - the drive test replays exactly what the game does. */
    unsigned long tick;

    /* Half the screen width in blocks; see gta_traffic_despawn_blocks(). */
    int view_blocks;


    /* WHERE THE CAMERA IS, IN BLOCKS, stashed at the top of each tick.
     *
     * Two rules need it and both used to run without it: nothing may be
     * deleted, and nothing may be teleported, where the player can see it.
     * The original gates every such decision on "is this car inside a view
     * rectangle expanded by the view half-width", and refuses
     * outright when the answer is yes. */
    int cam_bx, cam_by;

    /* HOW MANY CARS THE FLEET IS ALLOWED, at or below GTA_MAX_CARS.
     *
     * The ORIGINAL runs seven. Its spawner refuses to spawn once the count
     * of state-0 traffic within five blocks of the view rectangle reaches 7,
     * and drops that to 6 / 4 / 3 / 2 as the wanted level rises.
     * It also examines exactly one car slot per frame, so
     * the fleet fills slowly.
     *
     * This port runs twenty in a smaller area, and that is a difference in
     * kind rather than degree: the original does not need a junction
     * reservation because seven cars rarely contend for one crossing, while
     * twenty reliably deadlock at (204,108) whatever the reservation rule
     * says. Kept as a field rather than a constant so it can be swept. */
    int fleet_cap;

    /* RUN THE EXPENSIVE HALF OF THE OFF-ROAD INSTRUMENT? Off by default.
     *
     * `stat_offroad` samples the car's four CORNERS, which is four rotations
     * and up to twelve navigation-grid lookups per car per tick. That is fine
     * in a host test and it is not fine in the game: the simulation runs at a
     * fixed 50 Hz with up to MAX_CATCHUP ticks a frame, so on a machine
     * rendering at 6 fps the fleet ticks EIGHT times per frame and every one
     * of those diagnostics is paid eight times over. Measured on the Amiga
     * when it went in: the traffic tick went 2070 -> 2542 us, +23%, for a
     * number that is only ever printed.
     *
     * The CENTRE test stays on always - it is one lookup, and it is what
     * drives stat_offroad_events and the recovery. Only the corner sampling,
     * which exists for A/B sensitivity, is gated. */
    /* TWO JUNCTION RULES THAT CAN BE TURNED OFF FROM THE TEST.
     *
     * Both were added on 2026-08-24 and both change the flow at some sites and
     * not others, so they have to be measurable one at a time rather than
     * argued about. 1 is on, which is the shipped setting.
     *
     *   opt_cross_lock  no second turn inside one crossing (fallback turns)
     *   opt_sweep       do not commit to an arc another car is standing on
     */
    int opt_cross_lock;
    int opt_sweep;
    int opt_creep;      /* ticks wedged mid-arc before creeping out */
    int opt_nooverlap;  /* refuse any move that would start an overlap */
    int opt_unwedge;    /* ticks wedged before pushing through; 0 = never */
    int opt_boxgap;     /* follow the BOX in front, not the lane projection */
    int opt_horizon;    /* ticks the rectangle looks ahead */
    int opt_arcclaim;   /* a committed turn claims the crossing it enters */
    int opt_boxroot;    /* the entry test asks about the whole crossing */
    /* A CAR CROSSING A JUNCTION OWNS THE WHOLE OF IT.
     *
     * "jesli ktos jedzie prosto to cala droga na skrzyzowaniu jest przez niego
     * zajeta i nie powinno takiemu wjechac na skrzyzowanie"
     *
     * The reservation already existed - junction_claim() - but nothing ever
     * asked it at the gate. It was used only as the escape valve that lets one
     * car force a blocked box after three seconds. With this on, the claim is
     * taken when a car enters a crossing, held until it is out the other side,
     * and every other arm is refused entry while it stands. */
    int opt_holdbox;
    int opt_lights;     /* traffic lights - OFF until they are drawn: an
                         * invisible red reads as a car stopping for nothing
                         * ("nie wlaczaj sygnalizacji") */
    int opt_body;       /* occupancy by the whole body, not the centre */
    int opt_occ;        /* the junction occupancy matrix - see occ_rebuild() */
    int opt_occ_unused;
    int opt_commit;     /* the crossing is decided once, on the approach */
    /* THE TWO HALVES OF THE ARC RESERVATION, separately switchable, because
     * they cost completely different amounts.
     *
     *   opt_occ_hold  the cells a corner sweeps are TAKEN at the commit, so no
     *                 other vehicle may enter them. This is the half that
     *                 stops two cars standing on one square.
     *   opt_occ_look  a car ALSO eases off from GTA_TURN_LOOK before the
     *                 commit while those cells are busy. This is the half that
     *                 costs the city, because it refuses corners that would
     *                 have been fine by the time the car got there. */
    int opt_occ_hold;
    int opt_occ_look;
    int opt_arrows;     /* a turn needs the direction bit of its own block */
    int opt_keepclear;  /* a turn is refused if its exit lane is full */

    int diag_corners;

    /* The navigation grid, or a zeroed one if nobody supplied it. Routes need
     * it; without it the fleet simply does not drive, which is a great deal
     * better than driving badly and being hard to explain. */
    const gta_nav *nav;

    /* ONE ROUTE SEARCH PER TICK FOR THE WHOLE FLEET, and this is which car is
     * next in line. The original has exactly this rule - a single global "a
     * route is being found for driver N" that refuses a second request - and
     * the reason is the same: a breadth-first search over a few thousand
     * blocks is not something to do twenty times in one frame. */
    int route_turn;

    /* Handed out to each car as it is parked; see gta_car.serial. */
    unsigned long next_serial;

    /* How the route finder is getting on: searches that produced a path,
     * searches that found nothing, and how many nodes have been handed out.
     * Counters rather than a log, because this has to be answerable on the
     * Amiga as well as on the host - "are the cars actually driving routes"
     * is not a question a frame dump can answer. */
    long routes_ok, routes_failed, route_nodes;

    /* ROUTES THAT START BY SENDING THE CAR BACKWARDS, and how many of those
     * were handed out while the car was in the middle of a turn. The search
     * refuses to leave the start block the way the car came (gta_route.h), so
     * this number ought to be zero; it is counted because it was not, and
     * because the second figure is what says why. */
    long routes_backward, routes_while_turning;

    /* WHERE THE ABANDON BACKSTOP FIRED, and how often. A car deleted because
     * it has not covered a block in thirty seconds is the port admitting it
     * put a vehicle somewhere it cannot drive, and "somewhere" is the only
     * part that is actionable - the count on its own says a trap exists but
     * not which one. See GTA_TRAFFIC_ABANDON and can_get_away(). */
    long stat_abandoned;
    int  abandon_x, abandon_y, abandon_z;

    /* WHO IS ALLOWED TO FORCE A BLOCKED CROSSING - see junction_claim().
     *
     * The box rule refuses entry to a crossing whose far side is not clear,
     * and GTA_TRAFFIC_PATIENCE lets a car give up on that after three seconds
     * so that a wedged junction degrades to pushing through instead of to a
     * city-wide freeze. That valve is right and the way it was fitted was
     * wrong: FOUR CARS QUEUED AT THE FOUR ARMS RUN OUT OF PATIENCE AT THE SAME
     * TIME AND ALL FOUR PUSH IN TOGETHER, each ending up one car's length from
     * its own exit and blocked by the next. That is a cycle; nothing in it can
     * move, and the only thing that ever ended it was the abandon backstop
     * deleting a car - which the original never does and the developer
     * rightly refused to accept as a fix.
     *
     * The original arbitrates this explicitly. Its junction machinery is a
     * five-state machine - approach, entering, in the box, crossing, leaving -
     * and on entering it CLAIMS the junction: it writes 0x3c (60 frames) into
     * the
     * countdown at offset 0 of that junction's record in an 88-entry table,
     * stamps its own id, and a countdown releases it again with 0xff.
     * A claim with a timeout, not a poll.
     *
     * This is the same thing, sized for this port: the claim is only needed by
     * a car FORCING a blocked crossing, so a patient car is unaffected and the
     * throughput measured over the last two days is unchanged. One forcer per
     * crossing at a time turns the deadlock back into a queue.
     *
     * Eight entries because only one screen of traffic exists at once and a
     * car can only be in one crossing; the table failing open (see the
     * function) is deliberate - a missing claim must never be able to stop
     * traffic, only to fail to protect it. */
    unsigned char claim_x[GTA_CLAIM_MAX], claim_y[GTA_CLAIM_MAX];
    signed char   claim_z[GTA_CLAIM_MAX];
    unsigned long claim_car[GTA_CLAIM_MAX];   /* gta_car.serial, never index */
    short         claim_ttl[GTA_CLAIM_MAX];
    unsigned char claim_seen[GTA_CLAIM_MAX];  /* owner's body has covered it */

    /* The occupancy matrix, rebuilt every tick - see GTA_OCC_MAX. */
    unsigned char occ_x[GTA_OCC_MAX], occ_y[GTA_OCC_MAX];
    /* 16 hash chains over occ by (x & 15), rebuilt with the table - the
     * linear owner scan was 2.5% of the whole program (callgrind) and this
     * makes the same first-match answer walk a sixteenth of it. Entries in
     * a chain are index+1, 0 ends it. */
    unsigned char occ_head[16];
    unsigned char occ_next[GTA_OCC_MAX];
    signed char   occ_z[GTA_OCC_MAX];
    unsigned long occ_car[GTA_OCC_MAX];
    int           occ_n;
    long          stat_occ_refused;   /* entries refused because a cell was 1 */
    long          stat_knocked;       /* cars knocked loose by a collision */
    long          stat_leaned;        /* ...and set rolling by a lean (ticks) */
    long          stat_lane_swap_aborted;  /* changes given up: lane taken */
    long          stat_lane_swap_timeout;  /* ...and given up on the clock */
    long          stat_uturns;        /* cars that turned round - uturn_try */
    long          stat_fleet_hits;    /* of those, traffic hitting traffic */
    long          stat_knock_ended;   /* and how many settled back onto rails */

    /* WHAT THE FLEET IS DOING, FOR THE LOG - and this exists because the
     * developer could see a jam on the emulator that no host test reproduced,
     * and the game itself had nothing to say about it. "Check the logs" was
     * not an answer while the log went quiet the moment the game became
     * interactive.
     *
     * Counted in gta_traffic_tick() and read by whoever wants to print them;
     * `stat_moved` is in whole world pixels, summed over every car, so it is
     * the one number that cannot be argued with - a fleet that is stopped adds
     * nothing to it however healthy the percentages look. */
    /* HOW MUCH THE LANE KEEPER HAD TO DRAG CARS SIDEWAYS, in world pixels,
     * summed over the fleet. This is the reported fault made countable: "the
     * moment of the turn is wrong, so they land on the wrong line and then 3
     * to 5 pixels outside the junction they suddenly slide sideways to
     * correct". A car put down ON its lane needs no correction at all, so this
     * number going down IS the fix, and nothing else measures it - flow,
     * overlaps and reversals are all perfectly happy with a car crabbing
     * across a lane.
     *
     * ZERO SINCE 130, BY CONSTRUCTION. The sideways drag it counted no
     * longer exists: a car off its lane steers back onto it. The counter
     * stays because the instruments print it and a NON-zero would mean the
     * slide had come back from somewhere; SLIDE AFTER A CORNER is the
     * measurement that still has work to do. */
    long stat_lane_fix;

    /* ...and how much of it was applied within GTA_AFTER_TURN ticks of a car
     * finishing a corner. THIS is the reported fault; the rest is ordinary
     * lane changing. */
    long stat_lane_fix_corner;

    /* ...and that split by whether the car was on a route when it turned. */
    long stat_corner_routed, stat_corner_fallback;
    long stat_turns_routed, stat_turns_fallback;

    /* THE LANDING ERROR ITSELF, measured the moment an arc finishes, and split
     * into its two causes. `stat_land_geom` is how far the car ended from the
     * line the turn aimed at; `stat_land_aim` is how far that line was from the
     * one the lane keeper immediately asks for. They are different bugs with
     * different fixes and the correction figure is their sum. */
    long stat_landings, stat_land_geom, stat_land_aim;

    /* AND THE SAME SPLIT BY WHICH WAY THE CAR TURNED: [0] right, [1] left.
     *
     * The slide instrument found left turns half again as bad as right ones -
     * 73% on the line against 84% - and extending the street walk did not
     * touch it, so the lane TARGET is not the cause. This says which half of
     * the landing error carries the difference, and that decides where to
     * look: a bigger AIM error means the turn is aimed at the wrong block, a
     * bigger GEOMETRY error means the arc itself is wrong for that direction.
     *
     * `stat_aim_block_wrong` is the direct test of the first: the block the
     * turn was aimed at against the block the car actually finished in. The
     * trigger uses ONE step along the new heading, which is the landing block
     * for a tight right turn and need not be for a left one that crosses the
     * whole junction. */
    long stat_land_n_dir[2], stat_land_geom_dir[2], stat_land_aim_dir[2];
    /* WHERE THE CAR HAS SETTLED forty ticks after a corner, against the
     * centre of its lane, split [0] right / [1] left and bucketed
     * 0-1 / 2-3 / 4-7 / 8+ px. The uncontaminated version of the aim error -
     * see the note where it is taken. This is the number Fault B has to move. */
    long stat_settled[2][4];

    /* AND THE AIM ERROR WITH ITS SIGN KEPT, measured against the block the car
     * SETTLES in rather than the one its arc happened to end on - so it is the
     * uncontaminated version, for the same reason stat_settled is.
     *
     * The sign is the whole point. An unsigned 1.4 px could be noise spread
     * either way; a signed sum that stays near +-GTA_LANE_KERB is an offset
     * being applied in the wrong direction, which is a one-line fault rather
     * than a modelling one. `stat_aim_sign` counts how one-sided it is: [0] is
     * how many aimed LOW of the settled lane, [1] how many aimed HIGH. */
    /* THE GEOMETRY ERROR AGAINST THE LENGTH OF ONE STEP.
     *
     * With the aim measured as dead on 97% of the time, all that is left of
     * Fault B is the arc missing the line it correctly aimed at - 1.6 px a
     * corner. There are two candidates and they need opposite fixes.
     *
     * The TRIGGER: `ready` fires on the first tick where dist <= the nominal
     * radius, so a turn can begin up to ONE STEP OF TRAVEL early, and the
     * radius is then computed from that same overshot distance. If that is the
     * fault the error must GROW WITH THE STEP - a car doing 3 px a tick
     * overshoots the trigger by three times what a car doing 1 px does.
     *
     * The ARC INTEGRATION: rounding in the per-tick rotation, which has
     * nothing to do with how far the car moves between ticks and would be FLAT
     * across speeds.
     *
     * So the error is bucketed by the step length at the tick the turn was
     * issued, and the shape of that answers which one it is. Buckets are 0, 1,
     * 2, 3 and 4+ whole pixels a tick. */
    long stat_geom_by_step[5], stat_geom_by_step_n[5];

    /* HOW LONG A CORNER TAKES, in ticks, bucketed.
     *
     * The developer, playing the DOS original under DOSBox beside the port:
     * *"cars turn much more smoothly in the original, not suddenly in a
     * fraction of a second like ours"*. Our arc rate is derived from speed
     * (rate = v * K / 4R), so a fast car snaps round and a slow one crawls,
     * where the original is documented as a flat 0x20 of its 0x400 circle per
     * frame. This counts what ours actually does so the two can be compared as
     * numbers instead of impressions.
     *
     * Buckets are 1-4, 5-8, 9-16, 17-32 and 33+ ticks, i.e. up to 0.08 s,
     * 0.16 s, 0.32 s, 0.64 s and beyond at the 50 Hz tick. */
    long stat_turn_ticks[5], stat_turn_ticks_n, stat_turn_ticks_sum;

    /* THE RADIUS ACTUALLY ISSUED, summed, against the radius asked for.
     *
     * GTA_TURN_RADIUS is a CEILING: `aim_r` is the distance the car really has
     * left when the turn is triggered, clamped into [MIN, RADIUS]. Raising the
     * ceiling does nothing if the car never has that much approach - which is
     * the difference between "our corners are too tight" and "our corners are
     * decided too late", and they need opposite fixes. */
    long stat_aim_r_sum, stat_aim_r_n, stat_aim_r_capped;

    /* THE LINE ON THE OPEN STREET AGAINST THE LINE ON A JUNCTION BLOCK.
     *
     * The developer: *"the line along the straight avenue is different from the
     * one we take at the crossing - you had logging for this, so why do you not
     * see it"*. Fair, and the answer is that nothing measured it. The junction
     * instrument compares a car ENTERING a crossing with the same car LEAVING
     * it - both readings taken at the junction - so a difference between the
     * open street and the crossing is invisible to it by construction.
     *
     * This samples the lateral offset within the block on EVERY tick of every
     * car that is going straight (not turning, on a road block), split by
     * whether that block is part of a junction. Two means that differ IS the
     * jump, and the size of the difference is how many pixels it is. */
    long stat_line_street_sum, stat_line_street_n;
    long stat_line_cross_sum,  stat_line_cross_n;

    /* WHY ROUTE SEARCHES FAIL, one counter per reason - see gta_route.h.
     *
     * Two searches in three come back empty and that has been the largest
     * un-diagnosed number in the traffic for days. It was attributed to the
     * search budget; tripling the budget changed nothing at all, so the
     * attribution was wrong and there was no way to tell because a failure was
     * only ever counted, never explained. */
    long stat_route_fail[GTA_ROUTE_FAIL_KINDS];
    int  nostart_x, nostart_y, nostart_z;   /* the last car that asked for a
                                             * route from a block with no exits */

    long stat_aim_bias[2], stat_aim_bias_n[2];
    long stat_aim_sign[2][2];

    /* JUNCTION CROSSINGS, split into the two things that actually happen and
     * bucketed by HOW FAR the line moved - see gta_car.in_cross.
     *
     *   straight[] : the car went in and came out travelling the same way. Its
     *                line must not move at all; any bucket above the first is a
     *                fault, and the top one means it changed lane outright.
     *   turned[]   : the car came round a corner. Its line necessarily changes
     *                axis, so what is measured is how far the landing sits from
     *                the lane centre it is joining.
     *
     * Buckets are 0-1, 2-3, 4-7, 8+ world pixels. `*_virt` sums the worst
     * wander from the incoming line while still inside the crossing, which is
     * the part a person actually watches. */
    long stat_cross_straight[4], stat_cross_turned[4];
    long stat_cross_virt, stat_cross_virt_n;
    /* Of the straight-through crossings that came out a whole lane
     * across: how many STEERED there (two opposite turns issued
     * inside the crossing) and how many were following a route. */
    long stat_lane_change_steered, stat_lane_change_routed;

    /* THE DEADLOCK ITSELF, COUNTED - and it needed its own instrument because
     * every number this port had was a proxy for it.
     *
     * The developer photographed four cars stopped inside one crossing, each
     * one car's length from its own exit and each blocked by the next: a
     * cycle, which no amount of waiting resolves. "Flow" cannot see it (four
     * cars out of twenty is a normal-looking 80%), and `stat_abandoned` only
     * fires thirty seconds later and by then has already deleted the evidence
     * along with the car.
     *
     * So this counts the thing directly. Every tick, a car that is STOPPED on
     * a junction block and shares that crossing with at least two other
     * stopped cars is one car-tick of lock. `stat_boxlock_worst` keeps the
     * largest such group ever seen and `stat_boxlock_x/y` where it was, so a
     * run can be re-driven at the site that produced it.
     *
     * Three is the threshold on purpose: two cars nose to tail inside a wide
     * crossing while a third clears the exit is ordinary queueing, and calling
     * that a deadlock would make the number mean nothing. */
    long stat_boxlock;              /* car-ticks spent inside a locked box */
    int  stat_boxlock_worst;        /* most cars ever locked in one crossing */
    int  stat_boxlock_x, stat_boxlock_y;

    /* THE SLIDE AFTER A CORNER, AS A DISTANCE - see the note where it is
     * measured in gta_traffic.c.
     *
     * This exists because the report "cars come off the junction on the wrong
     * line and then correct" survived two numbers that both said it was small:
     * a landing error of about two pixels, and a city-wide correction total
     * that means nothing without knowing how many corners it is spread over.
     * The developer kept seeing it, so the developer was measuring something
     * the port was not. This is that thing: how far a car moves SIDEWAYS in
     * the GTA_AFTER_TURN ticks after its arc finishes.
     *
     * Buckets are 0-1 / 2-3 / 4-7 / 8+ world pixels, as everywhere else. */
    long stat_slide[4];
    /* ...and the same split by which WAY the car turned: [0] right, [1] left.
     * The developer reports the fault as one-sided - "the ones coming from
     * above and turning into my right are nearly all on the wrong line" - and
     * a total cannot answer that. */
    long stat_slide_dir[2][4];
    long stat_slide_px;

    /* Car-ticks spent stopped in the MIDDLE of a turn. With the arc's rate
     * derived from the speed, a car that stops half way round one stops
     * rotating too - which is physically right and may still be a problem,
     * because until it moves again its heading is a diagonal and every rule
     * that reads the road direction is reading a rounded guess. Counted so the
     * question can be answered instead of argued about. */
    long stat_frozen_turn;

    /* Car-ticks spent with no usable route, following the map's arrows
     * instead. That is the fallback, it is the worse driver, and it is what
     * makes a turn late - so it is the number GTA_ROUTE_REFILL exists to move. */
    long stat_no_route;

    /* WHY ROUTES DIE YOUNG. A quarter of all car-ticks are spent with no route
     * at all, and a route is twenty to thirty blocks - hundreds of ticks of
     * driving - so they are not being used up, they are being thrown away.
     * These say by which rule: the U-turn ban, the "a bus does not fit down
     * there" check, or the car simply drifting off its path. Nodes left at the
     * moment of the discard is the damning figure. */
    long stat_drop_uturn, stat_drop_fits, stat_drop_stray, stat_drop_nodes;

    /* CARS ON THE PAVEMENT, AND THIS PROJECT HAS NEVER COUNTED THEM.
     *
     * Reported from the running game with a photograph: a lorry with its nose
     * over the kerb at a crossing, on a street that is one lane each way, and
     * "about 15 cars a minute drive onto that pavement". Two days of traffic
     * work went past this because every instrument in this file measures the
     * LANE - how far from the line, how much correction after a corner - and
     * none of them measures the ROAD. A car can be perfectly on its lane
     * target and still be on the footway, if the lane target belongs to the
     * wrong block.
     *
     * MEASURED AT THE CAR'S FOUR CORNERS, NOT ITS CENTRE. The photograph shows
     * a vehicle whose centre is still over tarmac and whose front corners are
     * not; a centre test scores that clean, which is exactly how a metric
     * misses the thing it was built for. The corners come from the car's own
     * length and width out of the vehicle table, rotated by its heading.
     *
     * Three numbers, because they answer three different questions:
     *
     *   stat_offroad        car-ticks with ANY corner off a drivable block.
     *                       Sensitive; a car clipping a kerb for two ticks
     *                       registers. Good for A/B, bad for quoting.
     *   stat_offroad_events ENTRIES into the CENTRE-off-road state - the tick a
     *                       car puts its middle on the footway. This is the
     *                       number the developer can actually see happening
     *                       and the one to quote: "15 a minute" is an event
     *                       count, not a car-tick count. It is driven off the
     *                       CENTRE and not the corners on purpose - a bus is
     *                       40 px wide in a 32 px lane and overhangs the kerb
     *                       wherever it goes, so a corner-driven event counter
     *                       would tick for every bus that passed a pavement.
     *   stat_offroad_deep   car-ticks with the CENTRE off a drivable block.
     *                       The car is not clipping a kerb, it is driving on
     *                       the pavement. This one should be zero.
     *
     * stat_offroad_x/y is where the last entry happened, so a site that does
     * it repeatedly can be found and dumped with `gtadump dirmap`. */
    long stat_offroad, stat_offroad_events, stat_offroad_deep;
    int  stat_offroad_x, stat_offroad_y;

    /* WHAT THE CAR WAS DOING AT THE MOMENT IT LEFT THE ROAD. One bucket per
     * cause, counted on the ENTRY tick, because "cars end up on the pavement"
     * is a symptom with at least four plausible mechanisms and guessing which
     * is what the last two days were made of:
     *
     *   0  MID-TURN      the arc took it there - a corner cut too hard
     *   1  ON A JUNCTION straight, but inside a crossing - the lane target
     *                    inside a junction is the known-bad case
     *   2  STREET        straight, on ordinary road - the lane keeper aimed
     *                    at a block that is not road
     *   3  NO ROUTE      it had no route and was free-wheeling
     *
     * And how long the excursions last, because 8 entries over 2416 car-ticks
     * is a very different fault from 300 entries over the same total: the
     * first is a car that goes up the kerb and STAYS, the second is clipping.
     * Bucketed in ticks: <25, <100, <400, longer. */
    long stat_offroad_why[4];
    long stat_offroad_len[4];

    /* WHY A TURN THE ROUTE ASKED FOR WAS NOT TAKEN.
     *
     * The trigger is a ONE-SHOT: it fires only while the car is within
     * GTA_TURN_RADIUS of the lane line, which on a 32-px block is the last
     * dozen pixels of the approach. If anything vetoes it in that window the
     * car leaves the block still going straight, and there is no second
     * chance - the route node is then behind it and the fallback carries it
     * on. Traced from the game: a car reached its turn node at (208,110),
     * drove nineteen ticks straight through it, and ended up on the footway at
     * (208,109) where it stopped for ever.
     *
     * So the two vetoes are counted separately, because they need opposite
     * fixes: `lock` is our own anti-lane-change rule refusing a legitimate
     * second turn, and `room` is the exit lane being occupied - which the
     * comment in drive_one already admits is a known fault that drives on
     * anyway. `missed` is the count that matters: a car that left the block
     * its route said to turn in, without turning. */
    long stat_turn_refused_lock, stat_turn_refused_room, stat_turn_missed;

    /* Turns refused because the ARC ITSELF was not clear: the sweep from where
     * the car is now to where it lands passes through another vehicle. The old
     * test asked only whether the destination BLOCK had room, which is why
     * sprites climbed over each other in the middle of a corner while every
     * occupancy rule said the turn was legal. */
    long stat_turn_refused_sweep;
    /* Ticks a car was held exactly where it stood because the place it wanted
     * to move to already had a vehicle in it - see the hard test in
     * gta_traffic.c. This is the backstop that makes overlapping SPRITES
     * impossible rather than unlikely. */
    long stat_blocked_move;
    /* And turns refused because this car has already turned in this crossing. */
    long stat_turn_refused_cross;
    /* Turns refused because the crossing they would drive into is already
     * claimed by somebody else. THIS IS THE LEAK THE BOX INSTRUMENT FOUND: a
     * corner is committed on the approach block, which is not part of the
     * junction, so the arc carried the car into a claimed crossing without
     * ever passing the gate that asks. Measured at (64,64): 8101 car-ticks
     * with two cars in one box, of which only 10 had no claim at all - the
     * claim was being honoured and the turner simply never asked. */
    long stat_turn_refused_claim;
    /* Turns refused because the block the car is standing on does not carry
     * that direction - the arrows painted over the road. */
    long stat_turn_refused_arrow;
    /* Turns refused because the car was not in the OUTERMOST lane for that
     * side - a left turn taken from anywhere but the leftmost lane, or a right
     * turn from anywhere but the kerb lane. See GTA_LANE_TURN_RULE. */
    long stat_turn_refused_lane;
    /* Left turns exchanged for straight-on because the turn's path through
     * the box was already booked - "lewa zajeta, jedzie prosto". */
    long stat_left_skipped;
    long stat_joins;
    long stat_rams;                     /* player-vs-fleet hits, item 3c */                    /* convoy joins granted */
    /* Why the box gate said no, per refusal-tick: 0 body-in-line,
     * 1 first-square owned, 2 exit full, 3 no room past the box,
     * 4 the booking (a claim or a body on the path). Diagnostic. */
    long stat_box_why[5];
    /* Turns eased off because the cells the arc would sweep were not free. */
    long stat_turn_refused_occ;

    /* AND WHETHER THE LOST TURN WAS A ROUTED ONE.
     *
     * It decides what to do about the lane-change lock. The lock exists to
     * stop the FALLBACK (choose_heading, which is partly random and re-rolls
     * every tick) from taking two turns inside one crossing - a lane change.
     * A turn the ROUTE asked for cannot be that: the route audit says every
     * node is on road and every step is 4-connected. So if the missed turns
     * are mostly routed, the lock is refusing legitimate turns and should not
     * apply to them; if they are mostly fallback, it is doing its job and the
     * fault is elsewhere. [0] fallback, [1] routed. */
    long stat_turn_missed_kind[2];

    /* Times a car stopped at its corner because something was coming the other
     * way - the original's give-way rule. This should be a healthy
     * non-zero number: zero means the test never fires and cars are crossing
     * in front of moving traffic. */
    long stat_turn_gave_way;

    /* Cars put back on the road by recover_offroad(). Should be small and
     * should NEVER be the thing keeping stat_offroad_events down - if this
     * grows while the event count stays flat, the recovery is papering over a
     * driving fault instead of catching the tail of one. */
    long stat_offroad_recovered;

    long stat_moved;                    /* world px, all cars, since reset */
    /* The two impossible things, counted: a turn or a step no car could make
     * in one tick. See GTA_SANE_TURN / GTA_SANE_STEP. */
    long stat_face_jump, stat_face_jump_max, stat_face_jump_ctx;
    /* Corners abandoned because the car was shoved off its arc. */
    long stat_arc_dropped;
    /* Corner landings that refused to snap because the car had been
     * pushed off its arc - see the snap in drive_one(). */
    long stat_land_declined;
    long stat_pos_jump,  stat_pos_jump_max, stat_pos_jump_ctx;

    /* WHERE ONE TICK GOES, measured on the machine itself. The host profile
     * mis-ranks this file - it pays GHz for arithmetic and nothing for
     * memory, the 68020 the opposite - so the tick times its own phases when
     * the game supplies a clock. NULL (the default, and always in host
     * tools) costs nothing: every probe is behind `if (prof_clock)`.
     *   prof_us[0] release sweep + orphaned convoys
     *   prof_us[1] occ_rebuild
     *   prof_us[2] the drive_one loop
     *   prof_us[3] route_tick
     *   prof_us[4] despawn + park_band */
    unsigned long (*prof_clock)(void);
    unsigned long prof_us[5];

    /* One past the highest live claim slot - see the sweep in
     * gta_traffic_tick() for the invariant. Scans run to here, not to
     * GTA_CLAIM_MAX. */
    int claim_top;

    /* THE PLAYER'S CAR, as an obstacle the fleet can see. Phase 5 item 3b:
     * when the player is driving, the traffic must queue behind them and
     * must not book junction squares under their body, or every encounter
     * ends inside their sprite. Set every tick by the game; pl_active 0
     * (the default) costs nothing anywhere. */
    int  pl_active;
    long pl_x, pl_y;        /* 16.16 world */
    long pl_speed;          /* 16.16 px/tick, magnitude */
    int  pl_face;           /* 0..255 */
    int  pl_layer;
    int  pl_hl, pl_hw;      /* world px half-extents */

    /* PEOPLE IN THE ROAD - see gta_traffic_add_walker(). Sixteen is more than
     * the ped pool holds, so a full crowd always fits and there is never a
     * question of which pedestrian was dropped. */
    int  n_walk;
    long walk_x[GTA_MAX_WALKERS], walk_y[GTA_MAX_WALKERS];
    int  walk_layer[GTA_MAX_WALKERS];

    /* Lane changes taken to get round something standing in the road - see
     * GTA_LANE_SWAP_WAIT. A number, so "do they go round?" has an answer. */
    long stat_lane_swaps;

    int  stat_moving, stat_stopped;     /* cars, this tick */
    long stat_hold[GTA_HOLD_COUNT];     /* why the stopped ones are stopped */
} gta_traffic;

extern unsigned long gta_traffic_trace_serial;  /* host diagnostics, 0 = off */

/* THE STEERING CONTROLLER, EXPOSED FOR ITS OWN TEST. `gtadump steertest`
 * drives one car with nothing else running - no map, no fleet, no junctions -
 * so that a convergence fault cannot hide behind traffic. Give the car a
 * line (gta_traffic_set_line) and step it (gta_traffic_steer_step, which
 * steers and then moves at the car's own speed). Not used by the game. */
void gta_traffic_set_line(gta_car *c, int axis, long off, int dir);
void gta_traffic_steer_step(gta_car *c, int *slow);
void gta_traffic_init(gta_traffic *tr, const gta_tiles *t, unsigned long seed);

/* How much city the player can see, as a half-width in blocks. The fleet is
 * kept and spawned around that rather than around a constant, so zooming out
 * does not make cars vanish and reappear on screen. */
void gta_traffic_set_view_blocks(gta_traffic *tr, int blocks);

/* Give the traffic the navigation grid. Until this is called the fleet has no
 * routes and does not move. */
void gta_traffic_set_nav(gta_traffic *tr, const gta_nav *nav);

/* PEOPLE IN THE ROAD, refreshed by the caller every tick.
 *
 * Traffic braked for other traffic and for the player's car and for nothing
 * else, so a pedestrian crossing in front of a bus was simply run down -
 * "auta jak widza ze jest auto przed nimi czy przechodzien powinny stawac a
 * pchaja sie jak pojebane".
 *
 * They arrive as bare positions rather than through gta_peds.h on purpose:
 * this module has no business knowing what a pedestrian is, only that there
 * is something soft standing at (x,y) on layer z. The same channel will do
 * for a policeman on foot when there is one.
 *
 * Clear once a tick, add whoever is upright, and the follow rule and the
 * square reservation both see them. */
void gta_traffic_clear_walkers(gta_traffic *tr);
int  gta_traffic_add_walker(gta_traffic *tr, long x, long y, int layer);

/* Is the light green for a car on (bx,by) travelling along the x axis?
 * Exposed for the tests, which have to be able to tell a car waiting at a red
 * light from a car that is stuck. */
int gta_traffic_light_green(const gta_traffic *tr, int bx, int by, int along_x);

/* Park up to `want` cars on road blocks within `radius` blocks of (bx, by).
 * Returns how many were placed - fewer than asked for is normal and means the
 * area had no room, which is information rather than a failure. */
int gta_traffic_park(gta_traffic *tr, const gta_map *m,
                     int bx, int by, int radius, int want);

/* IS THIS BOX CLEAR OF EVERY CAR IN THE FLEET? The player's own car is not in
 * the fleet, so this is how the caller asks "may I be here" before committing
 * a step - see the bisection in gta_main.c. The original never lets a body
 * end a tick inside another one, so it never needs a big correction to get
 * it out again. */
int gta_traffic_box_free(const gta_traffic *tr, long x, long y, int face,
                         int hl, int hw, int layer);

/* STOP A MOVE AT THE CONTACT INSTEAD OF LETTING IT END INSIDE A CAR.
 *
 * Give it where the body WAS (x0,y0,ang0) and where it wants to be
 * (*x,*y,*ang, updated in place); if the destination is inside a fleet car it
 * bisects the move eight times - position and angle together, the angle the
 * short way round - and returns the last point that was clear. Returns 1 when
 * it moved the destination, 0 when nothing was in the way or when the body
 * was already overlapping (there is nothing to back off to then, and the
 * separation in gta_traffic_ram() does that job instead).
 *
 * This is the original's own method, and the reason it needs no violent
 * correction afterwards: a body that never ends a tick
 * inside another one never has to be thrown out of it. */
int gta_traffic_sweep_box(const gta_traffic *tr,
                          long x0, long y0, long ang0,
                          long *x, long *y, long *ang,
                          int hl, int hw, int layer);

/* ONE SIMULATION TICK of the whole fleet, at the caller's fixed rate - the
 * same 50 Hz tick the player runs on, and for the same reason: traffic that
 * moves per rendered frame speeds up and slows down with the frame rate, which
 * is a bug the player already had once and which is far more obvious on a car.
 *
 * The camera position is passed because the fleet follows it: cars that fall
 * too far behind are removed and new ones are put in ahead, so the city has
 * traffic wherever the player is rather than only where he started. */
void gta_traffic_tick(gta_traffic *tr, const gta_map *m,
                      long cam_x, long cam_y);

/* Queue every car for drawing. Call once per frame, before gta_render_frame;
 * the renderer sorts by layer itself. */
void gta_traffic_draw(gta_traffic *tr, gta_view *v);

/* The player's car, for the fleet to queue behind - see pl_* above. Call
 * with active 0 when the player is on foot. */
void gta_traffic_set_player(gta_traffic *tr, int active, long x, long y,
                            long speed, int face, int layer, int hl, int hw);

/* GRAB THE NEAREST CAR - entering a vehicle, the traffic side of it. Finds
 * the nearest live car within `range` world px of (x,y) on `layer`, fills
 * the out parameters with everything the driving side needs, and removes it
 * from the fleet (the tick compacts it; the release sweep frees its ground).
 * Returns 1 on success. */
int gta_traffic_grab_car(gta_traffic *tr, long x, long y, int layer,
                         int range, int *model, long *cx, long *cy,
                         int *face, int *remap, int *damage, int *had_driver);

/* Put a car back into the world where the player left it, as an abandoned one:
 * drawn, solid, enterable, and never driven away. Returns 0 if the fleet is
 * full, in which case the car really is lost - but at twenty cars against
 * GTA_MAX_CARS that does not happen in practice. */
int gta_traffic_abandon(gta_traffic *tr, int model, long x, long y, int face,
                        int layer, int remap, int damage);

/* The same, but what it leaves is a BURNT-OUT WRECK - every panel dented,
 * nobody able to get into it, and swept up on the wreck's own fixed radius
 * rather than the traffic one. The player's car is not in the fleet while he
 * is driving it, so when it blows up this is what puts the scrap back on the
 * street instead of the car simply disappearing. */
int gta_traffic_leave_wreck(gta_traffic *tr, int model, long x, long y,
                            int face, int layer, int remap);

/* THE RAM - the player's car against the fleet, one tick's worth.
 *
 * Detection is the same SAT the fleet uses on itself; the response is split
 * between the two models. The TRAFFIC car lives on rails (scalar speed along
 * its face - the accepted design), so its share of the hit is spoken in its
 * own language: speed cut to a crawl, body shoved a few pixels along the
 * push, damage bumped; its own recovery rules take it from there. The
 * PLAYER'S share comes back to the caller as a velocity delta and a yaw
 * kick for gta_vehphys to apply. Returns the number of cars hit this tick.
 * `pmass` is the raw 16.16 table mass of the player's model. */
int gta_traffic_ram(gta_traffic *tr, long px, long py, int pface,
                    int phl, int phw, long pvx, long pvy, long pmass,
                    int layer, long *dvx, long *dvy, long *dyaw,
                    long *dpx, long *dpy);

/* THE TWO JUNCTION QUESTIONS, FOR INSTRUMENTS ONLY.
 *
 * `is_junction` and `junction_root` are the engine's own, and a test that asks
 * them any other way is testing its own copy of the rule rather than the rule.
 * That mistake has already been made twice in this project's instruments
 * (the notes, "three versions of one instrument"). */
int  gta_traffic_is_junction(const gta_map *m, int bx, int by, int z);
void gta_traffic_junction_root(const gta_map *m, int bx, int by, int z,
                               int *rx, int *ry);

/* The draw angle for a car, which is its heading plus the half turn that takes
 * the art's own resting direction (south) to north. Identical to the player's
 * and deliberately spelled out rather than shared through gta_player.h, which
 * a car has no business including. */
#define GTA_CAR_ART_SOUTH 128
#define gta_car_draw_angle(c)  (((c)->face + GTA_CAR_ART_SOUTH) & 255)

#endif /* GTA_TRAFFIC_H */
