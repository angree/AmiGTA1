/* The crates - see gta_pickup.h.
 *
 * Licence: MIT (ours).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "gta_pickup.h"
#include "gta_player.h"     /* the ground types */

/* The item object for a kind, from the original's open-crate table:
 * 1..4 -> 0x4e..0x51, 6 -> 0x61, 9 -> 0x65, 10 -> 0x60, 0xb -> 0x64,
 * 0xc -> 0x62, 0xd/0xf -> 0x63, 0xe -> 0x5f. */
static int item_object(int kind)
{
    switch (kind) {
    case 1: case 2: case 3: case 4: return 0x4e + kind - 1;
    case 6: case 7: case 8:         return 0x61;
    case 9:                         return 0x65;
    case 10:                        return 0x60;
    case 11:                        return 0x64;
    case 12:                        return 0x62;
    case 13: case 15:               return 0x63;
    case 14:                        return 0x5f;
    default:                        return -1;
    }
}

/* The lowest layer at (bx,by) a person can stand on, or -1. */
static int stand_layer(const gta_nav *nav, int bx, int by)
{
    int z;
    for (z = 0; z < GTA_MAP_LAYERS; z++) {
        int g = gta_nav_ground(gta_nav_at_m(nav, bx, by, z));
        if (g == GTA_GROUND_PAVEMENT || g == GTA_GROUND_ROAD ||
            g == GTA_GROUND_FIELD)
            return z;
    }
    return -1;
}

int gta_pickups_add(gta_pickups *pk, long x, long y, int layer, int kind, int amount)
{
    gta_pickup *p;
    if (pk->n >= GTA_MAX_PICKUPS)
        return 0;
    p = &pk->p[pk->n++];
    p->x = x;
    p->y = y;
    p->layer = layer;
    p->kind = kind;
    p->amount = amount;
    p->state = GTA_PICKUP_CRATE;
    return 1;
}

int gta_pickups_load(gta_pickups *pk, const char *ini_path, int level,
                     const gta_nav *nav, const gta_tiles *t)
{
    FILE *f;
    char line[160];
    int sec = -1, k;
    int no_layer = 0;

    memset(pk, 0, sizeof *pk);
    pk->tiles = t;
    pk->spr_crate = gta_tiles_object_sprite(t, 0x54);
    pk->spr_open  = gta_tiles_object_sprite(t, 0x55);
    for (k = 0; k < 16; k++) {
        int o = item_object(k);
        pk->spr_item[k] = o >= 0 ? gta_tiles_object_sprite(t, o) : -1;
    }

    f = fopen(ini_path, "r");
    if (!f) {
        printf("gta: pickups - no %s, no crates\n", ini_path);
        fflush(stdout);
        return 0;
    }
    /* "[1]" opens a section; lines inside are
     *   N (x,y,z) POWERUP kind amount
     * with an optional flag between N and the bracket. Everything else in
     * the file - and the file is long - is skipped by the first character. */
    while (fgets(line, (int)sizeof line, f)) {
        const char *s = line;
        if (*s == '[') {
            sec = atoi(s + 1);
            if (sec > level) break;     /* sections are in order */
            continue;
        }
        if (sec != level)
            continue;
        if (!strstr(s, "POWERUP"))
            continue;
        {
            const char *b = strchr(s, '(');
            int x, y, z, kind, amount;
            const char *w;
            if (!b) continue;
            if (sscanf(b, "(%d,%d,%d)", &x, &y, &z) != 3) continue;
            w = strstr(b, "POWERUP");
            if (!w || sscanf(w + 7, "%d %d", &kind, &amount) != 2) continue;
            if (x < 0 || y < 0 || x >= GTA_MAP_DIM || y >= GTA_MAP_DIM) continue;
            {
                int lz = stand_layer(nav, x, y);
                if (lz < 0) { no_layer++; continue; }
                if (!gta_pickups_add(pk, (((long)x * 32 + 16) << 16),
                                     (((long)y * 32 + 16) << 16), lz, kind, amount))
                    break;
            }
        }
    }
    fclose(f);
    printf("gta: pickups - %d crates from %s section [%d]%s (crate sprite %d)\n",
           pk->n, ini_path, level, no_layer ? " (some had no ground)" : "",
           pk->spr_crate);
    fflush(stdout);
    return pk->n;
}

void gta_pickups_draw(gta_pickups *pk, gta_view *v, int blocks)
{
    int i;
    long r = (long)blocks << 21;
    for (i = 0; i < pk->n; i++) {
        gta_pickup *p = &pk->p[i];
        long dx, dy;
        if (p->state == 0) continue;
        dx = p->x - v->cam_x; if (dx < 0) dx = -dx;
        dy = p->y - v->cam_y; if (dy < 0) dy = -dy;
        if (dx > r || dy > r) continue;
        if (p->state == GTA_PICKUP_CRATE) {
            if (pk->spr_crate >= 0)
                gta_render_add_sprite(v, p->x, p->y, p->layer, p->layer,
                                      pk->spr_crate, 0);
        } else {
            int s = p->kind >= 0 && p->kind < 16 ? pk->spr_item[p->kind] : -1;
            if (pk->spr_open >= 0)
                gta_render_add_sprite(v, p->x, p->y, p->layer, p->layer,
                                      pk->spr_open, 0);
            if (s >= 0)
                gta_render_add_sprite(v, p->x, p->y, p->layer, p->layer, s, 0);
        }
    }
}

int gta_pickups_open_at(gta_pickups *pk, long x, long y, int layer, int radius)
{
    int i, n = 0;
    long r = (long)radius << 16;
    for (i = 0; i < pk->n; i++) {
        gta_pickup *p = &pk->p[i];
        long dx, dy;
        if (p->state != GTA_PICKUP_CRATE || p->layer != layer) continue;
        dx = p->x - x; if (dx < 0) dx = -dx;
        dy = p->y - y; if (dy < 0) dy = -dy;
        if (dx > r || dy > r) continue;
        p->state = GTA_PICKUP_OPEN;
        pk->stat_opened++;
        n++;
        printf("gta: crate %d opened at (%ld,%ld) - kind %d, %d\n", i,
               p->x >> 16, p->y >> 16, p->kind, p->amount);
    }
    if (n) fflush(stdout);
    return n;
}

int gta_pickups_take(gta_pickups *pk, long x, long y, int layer, int radius,
                     int *kind, int *amount)
{
    int i;
    long r = (long)radius << 16;
    for (i = 0; i < pk->n; i++) {
        gta_pickup *p = &pk->p[i];
        long dx, dy;
        if (p->state != GTA_PICKUP_OPEN || p->layer != layer) continue;
        dx = p->x - x; if (dx < 0) dx = -dx;
        dy = p->y - y; if (dy < 0) dy = -dy;
        if (dx > r || dy > r) continue;
        p->state = 0;
        pk->stat_taken++;
        *kind = p->kind;
        *amount = p->amount;
        return 1;
    }
    return 0;
}
