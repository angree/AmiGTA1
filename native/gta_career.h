/* The career: saved progress, level unlock, high scores (the research notes).
 *
 * THE FILE IS THE ORIGINAL'S: player_a.dat, 0x3f1 bytes, no header, a raw dump
 * of the DOS game's RAM at 0x191c8c (the original's routine reads it, the original's routine writes
 * it after every level). Little-endian, and the dwords are unaligned, so every
 * field here goes through a byte accessor: a player's own file from the PC
 * works, and ours works on the PC.
 *
 *   0x000 sound 0..7, 0x001 music 0..7, 0x002 text speed 1..3, 0x003 music
 *   mode, 0x007 a flag, 0x008.. multiplayer goal, 0x011 language override,
 *   0x012 six high-score tables of 0x39, 0x168 eight player slots of 0x4f,
 *   0x3e0 the current slot.
 *
 * A slot: name[15], best[6] (int32, -1 = LOCKED), chapter[2], level[2],
 * seen[6] (the level's cut-scene has played).
 *
 * The six level slots are the port's levels[] in order: LC1, LC2, SA1, SA2,
 * VC1, VC2 - the original's single-player table at 0x130bb8.
 *
 * Portable C89, no floats, no Amiga headers. Licence: MIT (ours). */
#ifndef GTA_CAREER_H
#define GTA_CAREER_H

#define GTA_CAREER_SIZE    0x3f1
#define GTA_CAREER_SLOTS   8
#define GTA_CAREER_LEVELS  6
#define GTA_CAREER_NAME    15

typedef struct {
    unsigned char b[GTA_CAREER_SIZE];
    /* what the last record_level() changed, for the blinking on the screen */
    int new_best;               /* level slot whose best was beaten, or -1 */
    int new_row;                /* public table row 0..2 entered, or -1 */
} gta_career;

/* Read it, or seed the original's defaults when it is missing or short, and
 * then write it at once, as the original's routine does. 0 = read, 1 = seeded, -1 = the
 * defaults could not be written either. */
int  gta_career_load(gta_career *c, const char *path);
int  gta_career_save(const gta_career *c, const char *path);
void gta_career_defaults(gta_career *c);

int  gta_career_slot(const gta_career *c);
void gta_career_set_slot(gta_career *c, int s);
const char *gta_career_name(const gta_career *c, int s);
void gta_career_set_name(gta_career *c, int s, const char *name);
const char *gta_career_builtin_name(int s);

long gta_career_best(const gta_career *c, int s, int level);    /* -1 = locked */
int  gta_career_unlocked(const gta_career *c, int s, int level);
int  gta_career_seen(const gta_career *c, int s, int level);

/* Public table `level`, row 0 lowest .. 2 highest. */
long gta_career_hs_score(const gta_career *c, int level, int row);
const char *gta_career_hs_name(const gta_career *c, int level, int row);

/* the original's routine after a single-player level, whatever the result: personal
 * best, the public table, and the next level unlocked on result 1. */
void gta_career_record_level(gta_career *c, int level, long score, int result);

/* The reset-slot screen's Reset (state 0xe): built-in name, LC1 only, no
 * cut-scenes seen; the public tables are not touched. */
void gta_career_reset_slot(gta_career *c, int s);

/* A name typed on the rename screen, checked against the cheat list
 * (the original's routine:1111-1471). Returns the cheat's bit (GTA_CHEAT_*), 0 for none.
 * Unlocks and `seen` are written into the slot here; the rest are flags for
 * the session, as in the original. */
#define GTA_CHEAT_UNLOCK   0x01
#define GTA_CHEAT_LIVES    0x02     /* infinite lives */
#define GTA_CHEAT_NOCOPS   0x04
#define GTA_CHEAT_KEYPAD   0x08     /* keypad-* gives weapons */
#define GTA_CHEAT_DEBUG    0x10
#define GTA_CHEAT_SEEN     0x20
#define GTA_CHEAT_LANG     0x40     /* special.fxt on/off */
#define GTA_CHEAT_MULT     0x80     /* multiplier starts at 10 */
#define GTA_CHEAT_SCORE    0x100    /* score 999999999 */
#define GTA_CHEAT_ARMS     0x200    /* weapons 99, armour, jail card */
int  gta_career_cheat(gta_career *c, int s, const char *name);

#endif
