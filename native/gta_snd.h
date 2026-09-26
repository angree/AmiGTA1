/* WHICH SOUND IS WHICH - the bank indices the original uses, by event.
 *
 * `gta_sfx.h` reads the bank and `gta_audio.h` plays it; this is the third
 * thing you need, and it is the one that cannot be derived from either. The
 * numbers are the original's own, taken out of the the original and already
 * written up in `the research notes` and `the research notes` - the tables
 * there give a sound id beside each event, so nothing here is a guess and
 * nothing here needed a fresh the original.
 *
 * They index `level001.snd`, the 131-entry effects bank. The SPEECH bank is a
 * different file with 71 entries (vocalcom) and a different loader, and this
 * header has nothing to do with it.
 *
 * A sound this port has not identified is simply absent from the list rather
 * than guessed at: playing the wrong index is worse than silence, because it
 * sounds like a bug in the audio layer when it is a bug in a table.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_SND_H
#define GTA_SND_H

/* ---- weapons (WEAPONS.md, the summary table) ---------------------------- */
#define GTA_SND_PISTOL       0x21    /* 33 */
#define GTA_SND_MACHINEGUN   0x22    /* 34 */
#define GTA_SND_FLAME        0x23    /* 35 */
#define GTA_SND_ROCKET       0x24    /* 36 */

/* A bullet that hits a wall or the water, rather than a person. */
#define GTA_SND_BULLET_HIT   0x2b    /* 43 */
/* The blast itself - the original's routine, which every explosion goes through. */
#define GTA_SND_EXPLOSION    0x27    /* 39 */
/* Somebody catches fire, and the death that follows a hundred ticks later. */
#define GTA_SND_IGNITE       0x18    /* 24 */
#define GTA_SND_BURN_DEATH   0x19    /* 25 */

/* ---- pickups (WEAPONS.md: "weapons 2, other kinds 4 (even) / 3 (odd)") --- */
#define GTA_SND_PICKUP_GUN   2
#define GTA_SND_PICKUP_EVEN  4
#define GTA_SND_PICKUP_ODD   3

/* ---- NAMED FROM THEIR CALL SITES, and wired ---------------------------
 *
 * Not from the notes: read out of the the original this session, each one
 * named by what the code around it does rather than by what it sounds like.
 *
 * THE SPRAY SHOP. the original's routine's trigger scan, in the branch whose sibling
 * prints `s_no_respray` and which asks the original's routine whether the player is in
 * a car - so this is the paint shop, played at the trigger's own block. */
#define GTA_SND_RESPRAY      0x2a

/* A PUNCH LANDING. the original's routine, twice, and both sites set the victim's state
 * to 0xaf - which this port already calls "punched down" (gta_peds.h). */
#define GTA_SND_PUNCH        0x10

/* A DEATH THAT LEAVES A BODY. the original's routine, twice, immediately after
 * the original's routine(..., 0x3f, ...) - map object 0x3f is the blood pool
 * (WEAPONS.md) - and with the victim's state set to 0x2d, the corpse. */
#define GTA_SND_DEATH        0x11

/* A SPLASH. the original's routine, three sites, each gated on the ground type under
 * the object being 1 and each preceded by map object 0x36. the original's routine
 * plays it for a CAR coming down on water; wired there (a jump that falls
 * short into the river, 201). */
#define GTA_SND_SPLASH       0x12

/* A CAR CRUSHED - NOT THE ORDINARY CRASH. the original's routine plays it twice, and
 * each time it has just set the car's damage (+0xf7) to 100: a write-off.
 * Its one caller is the original's routine, which walks records of stride 0x55b - the
 * TRAIN (see 0x29 below) - so this is a car the train ran into. 194 wired it
 * to every wall hit and ram; that was wrong and was taken out again in 197.
 * Not wired: the port has no trains. */
#define GTA_SND_CRASH        10

/* THE IMPACTS - the ordinary crash, graded by speed. the original's routine (two cars
 * meeting) takes the faster of the pair and the original's routine plays 6 below speed
 * 7, 7 below 17, 8 above (car+0x1c, the original's per-frame speed); the
 * same function is reached from the original's routine (a car and what the original's routine
 * walks - not yet read, so the wall is NOT one of these here). */
#define GTA_SND_IMPACT_LIGHT 6
#define GTA_SND_IMPACT_MED   7
#define GTA_SND_IMPACT_HARD  8

/* THE DOOR. the original's routine, the ped's animation states: 0x22 (the get-in
 * animation finished - it then sets the ped's mode to 7, IN A CAR) and 0x19
 * (the get-out finished - mode 2, on foot, state 0x88 standing) both play
 * 1, at the ped. It is the one sound at the end of both, so it is the door
 * shutting. 0x87 plays it too. (203) */
#define GTA_SND_DOOR         1
/* ...AND 5 BESIDE IT at the end of a get-in (0x22, and 0x87), only when the
 * car's +0x88 is 0 - never on the way out. Named by that: the car being
 * started. Not heard yet - an agent cannot; the developer's ear settles it. */
#define GTA_SND_CAR_START    5

/* THE TELEPHONE RINGING. Not a one-shot: the trigger scan's phone call
 * (the original's routine) registers each ringing phone with the original's routine - four
 * slots, x/y/z and an on flag at the original's table + slot * 0xd - and the car
 * sound loop the original's routine takes the NEAREST of them every frame and puts it
 * on the positional list with `mov ecx, 0x1c` as the sample (read with
 * capstone at 0x154db; the id lands in the record's [ebp-0x20a], where the
 * siren's 0x43 goes). Sample 28 is 2.1 s at 11025 Hz. (204) */
#define GTA_SND_PHONE        0x1c

/* THE SKID. The car sound loop the original's routine again (capstone, 0x139af..): for
 * the player's car, x = car+0x144 = |the sum of the axles' lateral forces|
 * (the original's table, the original's routine - forced to 21.0 on a handbrake above speed
 * 2). When x > 12, or the handbrake (+0x13a) is on above speed 1, it plays
 * 0x41 at 8000 + 150 x Hz. The bus (sound_function 5) wants x > 40 above
 * speed 14 and uses x / 2. (206) */
#define GTA_SND_SKID         0x41
/* The bus stopping: sound_function 5, the speed back to 0 from above 4 -
 * the air brake, 0x29 (the train's stop at a station is the same sample). */
#define GTA_SND_AIR_BRAKE    0x29
/* A burning wreck: the original's routine (the explosion) counts +0xf9 up as it sets
 * the car on fire, and the loop plays 0x4b for a car with +0xf9 set - for
 * good, nothing clears it but a repair. Wired for the port's wrecks
 * (gta_car.wrecked, which only an explosion sets): the nearest one (211). */
#define GTA_SND_WRECK_FIRE   0x4b

/* THE AI CARS' HORNS (212; WIRED in 213). the original's routine, the
 * blocked car's AI, when it cannot proceed and is still too fast for what
 * is ahead - `(v > 10 && d < 4) || (v > 4 && d < 2) || (v > 0 && d == 0)`,
 * v = +0x1c, d = the short at +0x7c (settled below) - throws the
 * game's dice: above 50 it honks (+0x12d = 40), 26..50 it yells
 * (the original's routine, lines 30..50). the original's routine counts +0x12d down a frame at
 * a time for a car with a horn (< 60) and picks its pattern once, +0x12f =
 * rand % 10. The loop sounds the model's horn sample (0x3a + horn / 10) while
 * byte [0x1302b1 + 50 * pattern - counter] is non-zero - ten rows of 50,
 * e.g. pattern 0 = forty frames solid, 1 = ten on, four off, four on, four
 * off, four on, four off, ten on (read with out/honk.py). And
 * d (213): +0x7c is the look-ahead's probe distance in BLOCKS - the probe point
 * is (base + d * 0x40) units ahead, 0x40 being a block - cycling 1..3 while
 * clear. The port: d = gta_traffic's gap >> 21, v = speed / GTA_SPEED_UNIT. */

/* ANOTHER CAR'S RADIO. The loop, for a car that is not the player's with
 * its door animating (the original's routine), switches on the model's `radio` byte
 * (+0xa8): 0 -> 0x7e, 1 -> 0x7f, 3 -> 0x80, 4 -> 0x81, 5 -> 0x82, and
 * nothing for 2 or above 5 - two-second music snippets, heard when a door
 * opens on a car with somebody in it. Wired at the carjack's door (211). */
#define GTA_SND_RADIO_BASE   0x7e

/* A CAR INTO A WALL. the original's routine is the car-meets-world handler (walls,
 * buildings, slope sides - COLLISION.md) and it ends with 9 when |car+0x231|
 * > 1; the original's routine writes the car's speed there, the same value as +0x1c.
 * So the wall is 9, played once an impact because the response zeroes the
 * speed. 197 left the wall silent because the original's routine's call had not been
 * read; it has now (202). */
#define GTA_SND_WALL         9

/* A CAR COMING DOWN. the original's routine, the car's fall: back on the ground within
 * sixteen units, it plays 0xe (with damage and four debris objects 0xd) -
 * or, when the block under it is water, the SPLASH (0x12) instead, once. */
#define GTA_SND_LANDING      0xe

/* A BODY HITTING THE GROUND. AI states 0x26 and 0x98 both play it and both
 * then set the state to 0x2b - which this port already calls "lying" - and
 * 0x93..0x98 is the sequence of being dragged out of a car. So it is the thud
 * at the end of a fall, not the fall itself. Not wired: the port's pull-out
 * and punch-down already have their own sound at the START of the sequence
 * (0x10) and adding a second at the end wants the two heard together first. */
#define GTA_SND_BODY_DOWN    0x0f

/* THE SIREN. The car sound loop in the original's routine reads car+0x113, which
 * the original's routine (POLICE.md: "siren on", the dispatch) sets to 1 and
 * the original's routine (siren off) clears. CORRECTED IN 206: the byte at +0xa9 of
 * the record +0x291 points at is the model's `horn`, not a counter. Mode 1
 * (lights on) holds 0x43 on the loop list for a model whose horn is 0x21 or
 * >= 60 - the police car's is 127 - so it sounds for as long as the lights
 * are on. Mode 2 ("sounding": the original's routine on the horn key, the original's routine
 * for the AI by its look-ahead) plays the model's horn sample, 0x3a + horn
 * / 10 at its rate + (horn % 10) x 512, or 0x42 when the horn is >= 60. */
#define GTA_SND_SIREN        0x43
#define GTA_SND_SIREN_YELP   0x42    /* mode 2 of a car with no horn */

/* ---- THE VOICE: vocalcom, a different bank ------------------------------
 *
 * Everything the game SAYS goes through the original's routine(line), which seeks into
 * vocalcom.raw and reads the line off the disk; the `< 0x47` every caller
 * tests is the bank's 71 entries. Played with gta_audio_speak(), never
 * gta_audio_play() - these numbers do not index level001. The ids come from
 * the original's routine(line) (said at full volume, not positional), each named by its
 * call site; the texts are english.fxt's.
 *
 *   the original's routine, the big message: its text compared with three keys */
#define GTA_VOICE_MISSION_COMPLETE  1   /* [2500] MISSION COMPLETE! */
#define GTA_VOICE_MISSION_FAILED    2   /* [2501] MISSION FAILED! */
#define GTA_VOICE_FRENZY_FAILED     5   /* [2504] FRENZY FAILED! */
/*   the kill frenzy starting: KF_BRIEF_TIMED (the original's routine case 0x89) and
 *   the frenzy's weapon handed over (the original's routine) */
#define GTA_VOICE_FRENZY            3
/*   a frenzy's count reached in time (the original's routine case 0x68, FRENZY_CHECK)
 *   and a race won (the original's routine) - the success sting */
#define GTA_VOICE_FRENZY_PASSED     4
/*   a race lost (the original's routine), and the original's routine */
#define GTA_VOICE_LOST              6
/*   an extra life (the original's routine case 0xf, text [life+]) */
#define GTA_VOICE_EXTRA_LIFE        0xb
/*   BUSTED (the original's routine) and WASTED (the original's routine) - the same line */
#define GTA_VOICE_BUSTED_WASTED     0xc
/*   a powerup's job done (the original's routine) */
#define GTA_VOICE_POWERUP_DONE      0x11
/*
 * THE POSITIONAL ONES, read out of the machine code (the ids are in
 * registers the tools lost; `rand` is the original's LCG x*0x79+1 mod 0x800):
 *   the original's routine  the cop's shout: 0x12 + kind, + (ped % 3) * 4 - three
 *                 voices of four shouts each, 18..29 (POLICE.md 0x6f, 0xe6)
 *   the original's routine  rand % 21 + 30 = 30..50, then silent for rand % 6 + 4
 *                 calls: the original's routine, 00031ac0, 000bf340 (the car code)
 *   the original's routine  rand % 9 + 52 = 52..60, then silent for 2 calls
 *   the original's routine  rand % 8 + 62 = 62..69, then silent for 3 - both from
 *                 the original's routine, which also plays effects 0x15..0x17
 *   the original's routine  rand & 3 + 7 = 7..10, at volume 60..99: the original's routine
 * WIRED (gta_main.c): 7..10 on a kill in the frenzy; the cop's 0x12 when he
 * gets out of his car (cop_shout); 30..50 when a ram above speed 10 makes
 * the driver yell (street_yell). Not yet: the cop's 0x13 (the target got
 * into a car) and 0x15 (the arrest - the port's arrest and its BUSTED
 * jingle land on one tick and the jingle would cut it), the blocked car's
 * and the pedestrians' calls of the original's routine, and 52..69 (the original's routine). */

/* ---- CANDIDATES, NOT WIRED --------------------------------------------
 *
 * Every positional sound the game plays goes through the original's routine(x,y,z,id),
 * and the whole list of ids it is ever called with can be read straight out
 * of the the original - twenty-five of them, all between 0 and 0x2b, which
 * is itself the evidence that the id space is the enum bounded at 0x4e and
 * not the 131-entry bank. By the function they are called from:
 *
 *   0x2a   the TRIGGER SCAN (the original's routine) - ONE call, in the respray branch:
 *          it is GTA_SND_RESPRAY, NOT the phone (PROGRESS 194)
 *   0x29   the original's routine, twice
 *   0x10 0x11 0x12   the original's routine, the get-in/car code
 *   0 1 5 0xf 0x10 0x12 0x13 0x14 0x19   the original's routine, the ped and car AI
 *          state machine - the deaths, the screams, the impacts
 *   6 7 9 0xb 0xc 0xd   the original's routine / the original's routine - 9 is the wall
 *          (GTA_SND_WALL, wired); the original's routine is a car meeting a MAP
 *          OBJECT and picks by the object's type (psVar3[5]): 2 -> 6,
 *          3 -> 7, then 0xc, 0xb, and 0x59 -> 0xd. Not wired: the port's
 *          cars do not collide with map objects.
 *   0x15 0x16 0x17   the original's routine
 *   0x18   the original's routine and the original's routine - catching fire, which agrees with
 *          WEAPONS.md
 *   0x2b   the original's routine / the original's routine - the bullet on a wall, which agrees
 *   10     the original's routine, twice
 *
 * AND WHAT THE AI STATE MACHINE PLAYS, by state, which is as far as the call
 * sites alone can take it:
 *
 *   state 3, 8    -> 0x13        state 0xc, 0x10 -> 0x14   (a per-ped flag
 *                                gates both pairs - two sounds, four states)
 *   state 0x11, 2 -> 0           state 0x19, 0x67 -> 1     (both guarded by
 *                                the original's routine == 1, an animation finishing)
 *   state 0x22    -> 1 AND 5 together, storing a car handle - getting in?
 *   state 0x26, 0x98 -> 0x0f     state 0x37 -> 0x10        state 0x5d -> 0x19
 *   state 0x87    -> 1 AND 5 together, like 0x22
 *
 * NAMING THE REST NEEDS A PED-STATE TABLE and there is not one in the research notes
 * That is the next piece of research, not another read of these call sites:
 * the states are the original's `ped+0x17` and the port already knows a
 * handful of them by name (0xaf punched down, 0x2b lying, 0x93..0x98 the
 * pull-out, 0x89..0x90 shot) from gta_peds.h. Filling the rest in from
 * the original's routine is a day's work and would settle every id at once.
 *
 * NONE OF THESE IS WIRED. Which of them is the engine, the skid, the crash or
 * the siren needs the call site read, not the list; and the whole table is
 * waiting on the one question an agent cannot answer - see below.
 *
 * WHAT IS NOT IN HERE YET, and what would settle each one:
 *
 *   the engine, the skid, the crash, the door, the telephone ring, the siren
 *   - none of them appears with an id in the notes. The way to find them is
 *   the way the weapon ids were found: the call site in the the original,
 *   not by ear and not by counting entries in the bank. The car code is
 *   around the original's routine / the original's routine and the phone is a map object.
 *
 * Do not fill these in by listening to samples and matching them to guesses.
 * This project has twice had a reader that was syntactically right and
 * factually wrong, and a sound table is the easiest place of all to be
 * confidently wrong.
 */

#endif /* GTA_SND_H */
