/* The career: see gta_career.h and the research notes */
#include <stdio.h>
#include <string.h>

#include "gta_career.h"

#define OFF_SOUND   0x000
#define OFF_MUSIC   0x001
#define OFF_SPEED   0x002
#define OFF_FLAG7   0x007
#define OFF_TARGET  0x009
#define OFF_FRAGS   0x00d
#define OFF_LANG    0x011
#define OFF_HS      0x012
#define HS_SIZE     0x39
#define HS_ROW      0x13
#define OFF_SLOTS   0x168
#define SLOT_SIZE   0x4f
#define SL_BEST     0x0f
#define SL_CHAPTER  0x27
#define SL_LEVEL    0x2f
#define SL_SEEN     0x37
#define OFF_CUR     0x3e0

static const char *const builtin[GTA_CAREER_SLOTS] = {
    "Ulrika", "Travis", "Katie", "Mikki", "Divine", "Bubba", "Troy", "Kivlov"
};

static long get32(const unsigned char *p)
{
    unsigned long v = (unsigned long)p[0] | ((unsigned long)p[1] << 8)
                    | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
    /* signed 32 bits on a host whose long is 64 too: -1 is "locked" */
    if (v >= 0x80000000UL)
        return -(long)(0xffffffffUL - v) - 1;
    return (long)v;
}

static void put32(unsigned char *p, long v)
{
    unsigned long u = (unsigned long)v;
    p[0] = (unsigned char)u;
    p[1] = (unsigned char)(u >> 8);
    p[2] = (unsigned char)(u >> 16);
    p[3] = (unsigned char)(u >> 24);
}

static unsigned char *slot_p(gta_career *c, int s)
{
    return c->b + OFF_SLOTS + s * SLOT_SIZE;
}

static const unsigned char *slot_cp(const gta_career *c, int s)
{
    return c->b + OFF_SLOTS + s * SLOT_SIZE;
}

static int clamp_slot(int s)
{
    return s < 0 ? 0 : s >= GTA_CAREER_SLOTS ? GTA_CAREER_SLOTS - 1 : s;
}

static int clamp_level(int l)
{
    return l < 0 ? 0 : l >= GTA_CAREER_LEVELS ? GTA_CAREER_LEVELS - 1 : l;
}

void gta_career_reset_slot(gta_career *c, int s)
{
    unsigned char *p = slot_p(c, clamp_slot(s));
    int k;
    memset(p, 0, SLOT_SIZE);
    strncpy((char *)p, builtin[clamp_slot(s)], GTA_CAREER_NAME - 1);
    for (k = 0; k < GTA_CAREER_LEVELS; k++)
        put32(p + SL_BEST + 4 * k, k == 0 ? 0 : -1);
}

/* the original's routine's seed. */
void gta_career_defaults(gta_career *c)
{
    static const char *const hs_name[3] = { "BILLY", "PAUL", "STEVE" };
    int t, r, s;
    memset(c->b, 0, sizeof c->b);
    c->b[OFF_CUR] = 1;
    c->b[OFF_SOUND] = 7;
    c->b[OFF_SPEED] = 2;
    c->b[OFF_MUSIC] = 6;
    c->b[OFF_FLAG7] = 1;
    put32(c->b + OFF_TARGET, 100000L);
    put32(c->b + OFF_FRAGS, 10L);
    c->b[OFF_LANG] = 0xff;
    for (s = 0; s < GTA_CAREER_SLOTS; s++)
        gta_career_reset_slot(c, s);
    for (t = 0; t < GTA_CAREER_LEVELS; t++)
        for (r = 0; r < 3; r++) {
            unsigned char *e = c->b + OFF_HS + t * HS_SIZE + r * HS_ROW;
            put32(e, (long)(r + 1));
            strncpy((char *)e + 4, hs_name[r], GTA_CAREER_NAME - 1);
        }
    c->new_best = c->new_row = -1;
}

int gta_career_save(const gta_career *c, const char *path)
{
    FILE *f = fopen(path, "wb");
    size_t n;
    if (!f)
        return -1;
    n = fwrite(c->b, 1, GTA_CAREER_SIZE, f);
    fclose(f);
    return n == GTA_CAREER_SIZE ? 0 : -1;
}

int gta_career_load(gta_career *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    c->new_best = c->new_row = -1;
    if (f) {
        n = fread(c->b, 1, GTA_CAREER_SIZE, f);
        fclose(f);
    }
    if (n == GTA_CAREER_SIZE)
        return 0;
    gta_career_defaults(c);
    return gta_career_save(c, path) == 0 ? 1 : -1;
}

int gta_career_slot(const gta_career *c)
{
    return clamp_slot((signed char)c->b[OFF_CUR]);
}

void gta_career_set_slot(gta_career *c, int s)
{
    c->b[OFF_CUR] = (unsigned char)clamp_slot(s);
}

const char *gta_career_name(const gta_career *c, int s)
{
    const unsigned char *p = slot_cp(c, clamp_slot(s));
    /* a file from elsewhere need not be terminated: the last byte is ours */
    ((unsigned char *)p)[GTA_CAREER_NAME - 1] = 0;
    return (const char *)p;
}

void gta_career_set_name(gta_career *c, int s, const char *name)
{
    unsigned char *p = slot_p(c, clamp_slot(s));
    memset(p, 0, GTA_CAREER_NAME);
    strncpy((char *)p, name && *name ? name : builtin[clamp_slot(s)],
            GTA_CAREER_NAME - 1);
}

const char *gta_career_builtin_name(int s)
{
    return builtin[clamp_slot(s)];
}

long gta_career_best(const gta_career *c, int s, int level)
{
    return get32(slot_cp(c, clamp_slot(s)) + SL_BEST + 4 * clamp_level(level));
}

int gta_career_unlocked(const gta_career *c, int s, int level)
{
    return gta_career_best(c, s, level) != -1;
}

int gta_career_seen(const gta_career *c, int s, int level)
{
    return get32(slot_cp(c, clamp_slot(s)) + SL_SEEN + 4 * clamp_level(level)) == 1;
}

long gta_career_hs_score(const gta_career *c, int level, int row)
{
    return get32(c->b + OFF_HS + clamp_level(level) * HS_SIZE + (row % 3) * HS_ROW);
}

const char *gta_career_hs_name(const gta_career *c, int level, int row)
{
    unsigned char *e = (unsigned char *)c->b + OFF_HS
                     + clamp_level(level) * HS_SIZE + (row % 3) * HS_ROW;
    e[4 + GTA_CAREER_NAME - 1] = 0;
    return (const char *)e + 4;
}

void gta_career_record_level(gta_career *c, int level, long score, int result)
{
    int s = gta_career_slot(c), i, k;
    unsigned char *p = slot_p(c, s);
    unsigned char *t;
    level = clamp_level(level);
    c->new_best = c->new_row = -1;
    /* 1. the personal best */
    if (get32(p + SL_BEST + 4 * level) < score) {
        put32(p + SL_BEST + 4 * level, score);
        c->new_best = level;
    }
    /* 2. the public table: the highest row it beats, strictly; the rows below
     * move down and the lowest falls off */
    t = c->b + OFF_HS + level * HS_SIZE;
    for (i = 2; i >= 0; i--)
        if (get32(t + i * HS_ROW) < score)
            break;
    if (i >= 0) {
        for (k = 0; k < i; k++)
            memcpy(t + k * HS_ROW, t + (k + 1) * HS_ROW, HS_ROW);
        put32(t + i * HS_ROW, score);
        memset(t + i * HS_ROW + 4, 0, GTA_CAREER_NAME);
        strncpy((char *)t + i * HS_ROW + 4, gta_career_name(c, s),
                GTA_CAREER_NAME - 1);
        c->new_row = i;
    }
    /* 3. passed opens the next one */
    if (result == 1 && level < GTA_CAREER_LEVELS - 1
        && get32(p + SL_BEST + 4 * (level + 1)) == -1)
        put32(p + SL_BEST + 4 * (level + 1), 0);
    if (result == 1)
        put32(p + SL_SEEN + 4 * level, 1);
}

/* The original stores each cheat interleaved with a message; these are the
 * even characters, as typed. Case matters, the whole name must match. */
static const struct { const char *name; int bit; } cheats[] = {
    { "nineinarow",     GTA_CHEAT_UNLOCK },
    { "super well",     GTA_CHEAT_UNLOCK },
    { "itsgallus",      GTA_CHEAT_UNLOCK },
    { "6031769",        GTA_CHEAT_LIVES },
    { "itstantrum",     GTA_CHEAT_LIVES },
    { "iamthelaw",      GTA_CHEAT_NOCOPS },
    { "stevesmates",    GTA_CHEAT_NOCOPS },
    { "callmenigel",    GTA_CHEAT_KEYPAD },
    { "buckfast",       GTA_CHEAT_KEYPAD },
    { "porkcharsui",    GTA_CHEAT_DEBUG },
    { "Heart of Gold",  GTA_CHEAT_SEEN },
    { "iamgarypenn",    GTA_CHEAT_LANG },
    { "iamnotgarypenn", GTA_CHEAT_LANG },
    { "hate machine",   GTA_CHEAT_MULT },
    { "itcouldbeyou",   GTA_CHEAT_SCORE },
    { "suckmyrocket",   GTA_CHEAT_ARMS },
};

int gta_career_cheat(gta_career *c, int s, const char *name)
{
    int i, k;
    unsigned char *p = slot_p(c, clamp_slot(s));
    for (i = 0; i < (int)(sizeof cheats / sizeof cheats[0]); i++) {
        if (strcmp(name, cheats[i].name) != 0)
            continue;
        if (cheats[i].bit == GTA_CHEAT_UNLOCK)
            for (k = 0; k < GTA_CAREER_LEVELS; k++)
                if (get32(p + SL_BEST + 4 * k) == -1)
                    put32(p + SL_BEST + 4 * k, 0);
        if (cheats[i].bit == GTA_CHEAT_SEEN)
            for (k = 0; k < GTA_CAREER_LEVELS; k++)
                put32(p + SL_SEEN + 4 * k, 1);
        if (cheats[i].bit == GTA_CHEAT_LANG)
            c->b[OFF_LANG] = (unsigned char)(name[3] == 'n' ? 0xff : 99);
        return cheats[i].bit;
    }
    return 0;
}
