/* The level script's declaration block. Read gta_script.h first.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gta_script.h"

/* THE NAME TABLE. The ids are the original's, so this is the same mapping
 * its parser does by looking the word up in a table of pointers - see
 * the original's own table. Longest first is not needed: the name is
 * matched whole, against a token the reader has already cut out. */
static const struct { const char *name; int type; } decl_names[] = {
    { "CAR",         GTA_DECL_CAR },
    { "PED",         GTA_DECL_PED },
    { "OBJECT",      GTA_DECL_OBJECT },
    { "PLAYER",      GTA_DECL_PLAYER },
    { "DRIVER",      GTA_DECL_DRIVER },
    { "PARKED",      GTA_DECL_PARKED },
    { "TELEPHONE",   GTA_DECL_TELEPHONE },
    { "TRIGGER",     GTA_DECL_TRIGGER },
    { "DOOR",        GTA_DECL_DOOR },
    { "TARGET",      GTA_DECL_TARGET },
    { "FUTURE",      GTA_DECL_FUTURE },
    { "COUNTER",     GTA_DECL_COUNTER },
    { "CRANE",       GTA_DECL_CRANE },
    { "DUMMY",       GTA_DECL_DUMMY },
    { "SPRAY",       GTA_DECL_SPRAY },
    { "BOMBSHOP",    GTA_DECL_BOMBSHOP },
    { "FUTUREPED",   GTA_DECL_FUTUREPED },
    { "CARTRIGGER",  GTA_DECL_CARTRIGGER },
    { "HELLS",       GTA_DECL_HELLS },
    { "FUTURECAR",   GTA_DECL_FUTURECAR },
    { "MPHONES",     GTA_DECL_MPHONES },
    { "PHONE_TOGG",  GTA_DECL_PHONE_TOGG },
    { "DUM_MISSION_TRIG", GTA_DECL_DUM_MISSION_TRIG },
    { "POWERUP",     GTA_DECL_POWERUP },
    { "GUN_TRIG",    GTA_DECL_GUN_TRIG },
    { "GTA_DEMAND",  GTA_DECL_GTA_DEMAND },
    { "MISSION_COUNTER", GTA_DECL_MISSION_COUNTER },
    { "SECRET_MISSION_COUNTER", GTA_DECL_SECRET_MISSION_COUNTER },
    { "MISSION_TOTAL", GTA_DECL_MISSION_TOTAL },
    { "TARGET_SCORE", GTA_DECL_TARGET_SCORE },
    { 0, 0 }
};

const char *gta_script_type_name(int type)
{
    int i;
    for (i = 0; decl_names[i].name; i++)
        if (decl_names[i].type == type)
            return decl_names[i].name;
    return "?";
}

/* WHOSE COORDINATES ARE PIXELS. The file mixes blocks and world pixels and
 * only the type says which; everything not listed here is in blocks. A
 * TELEPHONE at (108,108,4) is a block and the original multiplies it up to
 * the block centre itself; a CRANE at (864,14114,192) is already pixels. */
int gta_script_is_block(int type)
{
    switch (type) {
    case GTA_DECL_CRANE:
    case GTA_DECL_TARGET:
    case GTA_DECL_FUTUREPED:
    case GTA_DECL_PED:
    case GTA_DECL_OBJECT:
    /* FUTURE, the drop-off's own type, is one of these and was not. See the
     * head of this function's caller: reading it as blocks overflowed the
     * fixed-point conversion and put Liberty City's only drop-off at
     * (18448,18448). */
    case GTA_DECL_FUTURE:
        return 0;
    default:
        return 1;
    }
}

static int name_to_type(const char *w, int len)
{
    int i;
    for (i = 0; decl_names[i].name; i++) {
        if ((int)strlen(decl_names[i].name) == len &&
            strncmp(decl_names[i].name, w, (size_t)len) == 0)
            return decl_names[i].type;
    }
    return GTA_DECL_UNKNOWN;
}

/* ONE LINE. Returns 1 when it is a declaration - which is to say when it
 * carries coordinates. A command line has a name and numbers and no
 * brackets, and belongs to the other half of the script. */
static int parse_line(const char *ln, gta_decl *out)
{
    const char *p = ln, *br, *w;
    int line = 0, flag = 0, x = 0, y = 0, z = 0, wl = 0;
    long v[4];
    int nv = 0, i;

    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9')
        return 0;                       /* comments, blanks, section heads */
    line = (int)strtol(p, (char **)&p, 10);

    /* The optional permanence flag sits between the line number and the
     * bracket, and nothing else can. */
    while (*p == ' ' || *p == '\t') p++;
    if (*p >= '0' && *p <= '9')
        flag = (int)strtol(p, (char **)&p, 10);

    br = strchr(p, '(');
    if (!br)
        return 0;                       /* a command, not a declaration */
    if (sscanf(br, "(%d,%d,%d)", &x, &y, &z) != 3)
        return 0;

    p = strchr(br, ')');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    w = p;
    while ((*p >= 'A' && *p <= 'Z') || *p == '_' || (*p >= '0' && *p <= '9'))
        p++;
    wl = (int)(p - w);
    if (wl <= 0)
        return 0;

    for (nv = 0; nv < 4; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '-' && (*p < '0' || *p > '9'))
            break;
        v[nv++] = strtol(p, (char **)&p, 10);
    }

    out->line = (short)line;
    out->type = (unsigned char)name_to_type(w, wl);
    out->flag = (unsigned char)flag;
    out->x = (short)x; out->y = (short)y; out->z = (short)z;
    out->a = out->b = out->c = out->d = 0;
    for (i = 0; i < nv; i++) {
        short sv = (short)v[i];
        if (i == 0) out->a = sv;
        else if (i == 1) out->b = sv;
        else if (i == 2) out->c = sv;
        else out->d = sv;
    }
    out->n_num = (unsigned char)nv;
    return 1;
}

int gta_script_load(gta_script *s, const char *path, int level)
{
    FILE *f;
    char ln[256];
    int sec = -1, cap = 0;

    memset(s, 0, sizeof *s);
    f = fopen(path, "r");
    if (!f)
        return 1;

    /* TWO PASSES over a small text file rather than a guess at the size:
     * Liberty City's section [1] holds 575 declarations and the bonus
     * levels hold a handful, and an array sized for the worst case would be
     * 1400 records nobody uses. */
    while (fgets(ln, sizeof ln, f)) {
        if (ln[0] == '[') {
            if (sec == level) break;
            sec = atoi(ln + 1);
            continue;
        }
        if (sec != level) continue;
        {
            gta_decl tmp;
            if (parse_line(ln, &tmp)) cap++;
        }
    }
    if (cap == 0) { fclose(f); return 1; }

    s->d = (gta_decl *)malloc((size_t)cap * sizeof(gta_decl));
    if (!s->d) { fclose(f); return 1; }

    rewind(f);
    sec = -1;
    while (fgets(ln, sizeof ln, f)) {
        if (ln[0] == '[') {
            if (sec == level) break;
            sec = atoi(ln + 1);
            continue;
        }
        if (sec != level) continue;
        s->n_lines++;
        if (s->n >= cap) continue;
        if (!parse_line(ln, &s->d[s->n]))
            continue;
        if (s->d[s->n].type == GTA_DECL_UNKNOWN)
            s->n_unknown++;
        s->n++;
    }
    fclose(f);
    return 0;
}

void gta_script_free(gta_script *s)
{
    if (s->d) free(s->d);
    s->d = 0;
    s->n = 0;
}

static int stand_layer(const gta_nav *nav, int bx, int by);

/* WHICH LAYER A THING DECLARED AT THIS BLOCK STANDS ON. The file's z is the
 * original's own and does not survive this port's layer numbering; the
 * crates already answer it by looking (gta_pickup.c), and a telephone stands
 * on the pavement exactly as a crate does. */
int gta_script_model_index(const gta_tiles *t, int model_id)
{
    int i;
    for (i = 0; i < t->n_cars; i++)
        if (t->cars[i].model_id == (unsigned char)model_id)
            return i;
    return -1;
}

int gta_script_decl_pos(const gta_script *s, int line, long *wx, long *wy)
{
    const gta_decl *d = gta_script_by_line(s, line);
    if (!d)
        return 0;
    if (gta_script_is_block(d->type)) {
        *wx = gta_script_centre(d->x);
        *wy = gta_script_centre(d->y);
        return 1;
    }
    /* PIXELS, in the original's 64 units to a block; this port has 32, so
     * one of ours is two of theirs. And the original's own rule: a position
     * that lands exactly on a block corner is nudged to the block's CENTRE
     * (`if ((x % 0x40 == 0) && (y % 0x40 == 0)) { x += 0x20; y += 0x20; }`,
     * the original's own rule) - otherwise an arrow at a corner points at
     * the join of four blocks rather than at the thing standing there. */
    {
        long x = d->x, y = d->y;
        if ((x & 63) == 0 && (y & 63) == 0) { x += 32; y += 32; }
        *wx = x << 15;
        *wy = y << 15;
    }
    return 1;
}

int gta_script_player_start(const gta_script *s, int *bx, int *by, int *angle)
{
    int i;
    for (i = 0; i < s->n; i++) {
        if (s->d[i].type != GTA_DECL_PLAYER)
            continue;
        *bx = s->d[i].x;
        *by = s->d[i].y;
        *angle = gta_script_angle(s->d[i].b);
        return 1;
    }
    return 0;
}

int gta_script_stand_layer(const gta_nav *nav, int bx, int by)
{
    return stand_layer(nav, bx, by);
}

static int stand_layer(const gta_nav *nav, int bx, int by)
{
    int z;
    for (z = 0; z < GTA_MAP_LAYERS; z++) {
        int g = gta_nav_ground(gta_nav_at_m(nav, bx, by, z));
        if (g == 2 || g == 3 || g == 4)         /* road, pavement, field */
            return z;
    }
    return -1;
}

int gta_script_place(gta_script *s, const gta_nav *nav, const gta_tiles *t)
{
    int i, lost = 0;

    /* Map object type 0x28 (40) is the telephone - the original creates one
     * at every TELEPHONE declaration's block centre, with the rotation the
     * line carries. */
    s->spr_phone = gta_tiles_object_sprite(t, 0x28);
    s->n_phones = 0;
    s->n_drops = 0;
    /* Kept because a DROP_ON's sprite and its layer are not known until the
     * command runs. */
    s->tiles = t;
    s->nav = nav;

    for (i = 0; i < s->n; i++) {
        const gta_decl *d = &s->d[i];
        int lz;
        if (d->type != GTA_DECL_TELEPHONE)
            continue;
        if (s->n_phones >= GTA_MAX_PHONES)
            break;
        if (d->x < 0 || d->y < 0 || d->x >= GTA_MAP_DIM || d->y >= GTA_MAP_DIM)
            continue;
        lz = stand_layer(nav, d->x, d->y);
        if (lz < 0) { lost++; continue; }
        {
            gta_placed *p = &s->phone[s->n_phones++];
            p->x = (((long)d->x * 32 + 16) << 16);
            p->y = (((long)d->y * 32 + 16) << 16);
            p->layer = (unsigned char)lz;
            /* THE FILE'S ROTATION IS IN 1024THS and this port's circle is
             * 256 steps, so a quarter of it. 768 is west, 256 east. */
            p->angle = (unsigned char)((d->b / 4) & 255);
            p->line  = d->line;
            p->spr   = -1;
        }
    }
    /* THE MISSION CARS, kept here rather than given to the fleet - see
     * gta_mcar. The model number in the file is the ORIGINAL'S and has to
     * be turned into this port's car-table record. */
    s->n_mcars = 0;
    for (i = 0; i < s->n; i++) {
        const gta_decl *d = &s->d[i];
        int lz, mi;
        if (d->type != GTA_DECL_PARKED) continue;
        if (s->n_mcars >= GTA_MAX_MCARS) break;
        if (d->x < 0 || d->y < 0 || d->x >= GTA_MAP_DIM || d->y >= GTA_MAP_DIM)
            continue;
        mi = gta_script_model_index(t, d->a);
        lz = stand_layer(nav, d->x, d->y);
        if (mi < 0 || lz < 0) { lost++; continue; }
        {
            gta_mcar *c = &s->mcar[s->n_mcars++];
            c->x = gta_script_centre(d->x);
            c->y = gta_script_centre(d->y);
            c->line = d->line;
            c->model = (short)mi;
            c->layer = (unsigned char)lz;
            c->angle = (unsigned char)gta_script_angle(d->b);
            c->done = 0;
        }
    }

    /* NUMBERS THAT CANNOT BE WHAT THE TYPE SAYS. The city is 256 blocks
     * square and five layers deep, so a declaration this port reads as
     * BLOCKS whose z is 64 or more is being read in the wrong units - which
     * is exactly how the drop-off ended up at (18448,18448). One line each,
     * because there is one such declaration in Liberty City and it is the
     * level author's own slip. */
    {
        int odd = 0;
        for (i = 0; i < s->n; i++) {
            const gta_decl *d = &s->d[i];
            if (!gta_script_is_block(d->type)) continue;
            if (d->x <= 255 && d->y <= 255 && d->z < 64) continue;
            printf("gta: script - declaration %d (%s) reads as blocks but "
                   "carries (%d,%d,%d) - pixels?\n", d->line,
                   gta_script_type_name(d->type), d->x, d->y, d->z);
            odd++;
        }
        if (odd) fflush(stdout);
    }

    printf("gta: script - %d telephones placed, %d mission cars waiting%s "
           "(phone sprite %d)\n", s->n_phones, s->n_mcars,
           lost ? " (some had no ground)" : "", s->spr_phone);
    fflush(stdout);
    return s->n_phones;
}

void gta_script_draw(gta_script *s, gta_view *v, int blocks)
{
    long r = (long)blocks << 21;
    int i;

    /* THE DROP-OFFS FIRST, because they are drawn whether or not this style
     * has a telephone: the two have nothing to do with each other and an
     * early return over the phone sprite would take them with it. */
    for (i = 0; i < s->n_drops; i++) {
        long dx = s->drop[i].x - v->cam_x, dy = s->drop[i].y - v->cam_y;
        if (s->drop[i].dead || s->drop[i].spr < 0) continue;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > r || dy > r) continue;
        gta_render_add_sprite(v, s->drop[i].x, s->drop[i].y,
                              s->drop[i].layer, s->drop[i].layer,
                              s->drop[i].spr, s->drop[i].angle);
    }

    if (s->spr_phone < 0)
        return;
    for (i = 0; i < s->n_phones; i++) {
        long dx = s->phone[i].x - v->cam_x, dy = s->phone[i].y - v->cam_y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > r || dy > r) continue;
        gta_render_add_sprite(v, s->phone[i].x, s->phone[i].y,
                              s->phone[i].layer, s->phone[i].layer,
                              /* THE RING IS DATA: 249 is the idle phone,
                               * 250 the same with a red arrow and 251 the
                               * phone itself red. */
                              s->spr_phone + (int)s->phone[i].ring,
                              s->phone[i].angle);
    }
}

int gta_script_due_car(gta_script *s, long cam_x, long cam_y, int blocks)
{
    long r = (long)blocks << 21;
    int i;
    for (i = 0; i < s->n_mcars; i++) {
        long dx, dy;
        if (s->mcar[i].done) continue;
        dx = s->mcar[i].x - cam_x; if (dx < 0) dx = -dx;
        dy = s->mcar[i].y - cam_y; if (dy < 0) dy = -dy;
        if (dx > r || dy > r) continue;
        s->mcar[i].done = 1;
        return i;
    }
    return -1;
}

const gta_decl *gta_script_by_line(const gta_script *s, int line)
{
    int i;
    /* A LINEAR WALK, and it stays one until something asks for it a hundred
     * times a tick. The declarations are in file order, which is line order,
     * so a binary search is available the moment it is worth writing. */
    for (i = 0; i < s->n; i++)
        if (s->d[i].line == (short)line)
            return &s->d[i];
    return 0;
}
