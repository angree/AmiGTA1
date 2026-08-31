/* The 2.5D city renderer.
 *
 * Portable C89, no floats, no Amiga headers: it draws into a plain 8-bit
 * chunky buffer, so the same object file serves the Amiga (where that buffer
 * is amigagfx_chunky()) and the host preview in tools/gtadump.c. Being able to
 * look at a frame on the PC, in a second, is what keeps this honest - the two
 * bugs this project has already paid for were both "compiles fine, renders
 * nonsense".
 *
 * The full design note, including why the projection is what it is and why
 * nothing multiplies in the inner loop, is Phase 4 of PLAN.md. The short
 * version:
 *
 *   scale(g) = (CAM_H - GREF) / (CAM_H - g)
 *
 * a camera looking straight down from CAM_H grid levels up, so each grid level
 * is the ground plan scaled about the centre of the screen. Higher levels are
 * drawn further out, which is what puts a building's roof outboard of its base
 * and shows the wall that faces the middle of the screen.
 *
 * Licence: MIT (ours). The architecture comes from Mike Dailly's description
 * of the original engine (RESEARCH.md); no code was copied from it.
 */
#ifndef GTA_RENDER_H
#define GTA_RENDER_H

#include "gta_map.h"
#include "gta_tiles.h"
#include "gta_trig.h"

/* Grid levels are the PLANES between block layers: 6 layers of blocks have 7
 * planes, 0 (under the bottom layer) to 6 (above the top one). A block at
 * layer z occupies grid z to z+1, and its lid - the face the player walks on -
 * is at grid z+1. */
#define GTA_GRID_LEVELS  (GTA_MAP_LAYERS + 1)

/* Camera height above grid 0, in grid levels. It sets how strongly the city
 * splays outward: smaller means a wider-angle, more dramatic view with
 * taller-looking walls, larger means flatter and closer to a plain top-down
 * map.
 *
 * THIS USED TO BE DERIVED FROM `MAPPER.PAS`, THE 1995 PROTOTYPE, AND THAT WAS
 * THE WRONG ENGINE. Its numbers give 15.69 levels, which is what shipped, and
 * 15.69 levels is the "there is no perspective at all" the developer reported
 * from the game. GTA's own camera is at 6.25 to 8.25 levels - four hundred per
 * cent more splay - and the difference is the whole look of the game. The
 * prototype was the best source available until the DOS projection was read;
 * it is not a source any more.
 *
 * The absolute zoom is a separate decision and is ours: 32 pixels per block at
 * street level, measured rather than inherited (64x64 gives 5x3 blocks on a
 * 320x200 screen - see the notes). Because scale() is normalised on GREF,
 * changing the zoom does not disturb the splay. */
/* CAMERA HEIGHT, IN QUARTER GRID LEVELS. 32 = 8 levels.
 *
 * Quarters rather than whole levels because the original does not sit on a
 * whole one: read out of the original, its camera is at 6.25 to 8.25 levels
 * depending on zoom mode (its two camera modes hold heights 300 and 400,
 * minus the followed object's world z). Whole levels
 * cannot express that, and the difference between 6.25 and 7 is visible.
 *
 * THE SHIPPED VALUE IS 8 LEVELS, AND IT IS THE DEVELOPER'S MEASUREMENT, NOT
 * MINE. It is what they settled on with the original running beside the port
 * ("c8 ... better than it was"), and it lands inside the 6.25-8.25 the game
 * gives. It could not be adopted at the time because the corner-square patch
 * made a low camera paint slabs of wall over the roofs beside it - four times
 * the area at c8 that it painted at c16, measured - and that is what the
 * trapezoid walls removed.
 *
 * The 1995 prototype's 15.69 levels, which this used to ship, is a DIFFERENT
 * ENGINE's number: MAPPER.PAS is not GTA. It was the best figure available
 * before the DOS projection was read, and it is wrong for this game - it is
 * the "no perspective at all" the developer reported.
 *
 * F7 and F8 still move it live, so this is a starting point rather than a
 * ruling. Lower means more splay and taller-looking walls. */
#define GTA_CAM_H  32

/* The range F7 and F8 may move it over. The LOWER bound is a hard safety rule,
 * and not taste: level_step() divides by (cam_h - 4*g) with g up to
 * GTA_GRID_LEVELS-1 = 6, so cam_h must stay strictly above 24 QUARTER levels
 * or the renderer divides by zero and takes the machine down with it. 25 is
 * the first safe value, and it happens to be the original's own hizoom camera
 * to within a fifth of a level - which is a coincidence, but a convenient one:
 * the safety clamp and the artistic floor are the same number. */
/* THE "LIGHT 3D" PRESET - the middle setting on F5.
 *
 * Sixteen grid levels: enough splay that buildings still have visible sides
 * and the city reads as solid, little enough that a frame draws about 70 walls
 * instead of 104 and 709 columns instead of 932. Measured on gta-test.uae,
 * same view: 19563 us against 23248, so about 16% of the frame back.
 *
 * It is NOT a different renderer - the trapezoid walls, the 5/6 squash and the
 * lid path are all exactly the same code. What changes is how much city the
 * projection puts on screen, which is the only thing the camera height ever
 * changed. That is also why it composes with F7/F8: the preset sets the height
 * when the mode is entered and then gets out of the way. */
#define GTA_CAM_H_LIGHT 64      /* 16 levels */

#define GTA_CAM_H_MIN  25       /* 6.25 levels - the original`s own clamp */
#define GTA_CAM_H_MAX  160      /* 40 levels - flat enough for anyone */

/* The reference grid level, drawn at exactly GTA_TILE_DIM pixels with no
 * scaling at all. It is the street, because the street is the overwhelming
 * majority of the tiles on screen and so is the one worth the fast path.
 *
 * GRID 2, and that is a measurement rather than a guess. Dumping columns of
 * nyc.cmp (gtadump column) shows a plain road as height 3 with its only lid on
 * LAYER 1, and every building beside it starting its walls on LAYER 2:
 *
 *   column (62,60) height 3    z=1 LID=75, nothing else
 *   column (66,62) height 5    z=2,3,4 W and N walls, z=4 LID=27
 *
 * A lid sits at the TOP of its block, so layer 1's lid is grid 2 - and the
 * walls that stand on the street start at grid 2 as well. The two agree, which
 * is what makes this a fact and not a preference. Layers 0 and 1 are the solid
 * ground underneath and are never seen. */
#define GTA_GREF   2

/* Reciprocal table size: the longest run any blit will ever step across.
 *
 * 384 was sized for "the widest tile is grid 6 at about 48 px", which was true
 * while the camera sat at 16 grid levels. It is not true at the original's own
 * camera height: at cam_h 25 (6.25 levels) and the widest zoom a single grid-6
 * tile is 1088 pixels, and a WALL QUAD spans |dbx| * (step[z+1] - step[z]),
 * which is larger still - a few thousand for a block near the edge of the
 * walk. Clamping the index there does not merely round, it makes the texture
 * run out part way across and smear the last row for the rest of the span.
 *
 * So the table covers 1024 (4 KB) and tex_inc() takes an actual divide for
 * anything past it - which only happens for a quad that is mostly off-screen
 * anyway. Correct at every size, table-driven at every size that matters. */
#define GTA_RECIP_MAX 1024

/* Largest ring index the column-height cache can hold. The renderer needs
 * about (screen/2) / 28 + 2 rings, so 32 covers a view up to about 1800 pixels
 * across - far past anything an Amiga will open, and past the 640x400 the host
 * tools use to inspect frames. A target bigger than that still renders; it
 * just falls back to asking the map every time. */
#ifdef __MORPHOS__
/* 64, BECAUSE 32 WAS SIZED FOR A 320-WIDE SCREEN AND THIS ONE IS 640.
 *
 * The ring count is dst_w / (2 * step0) + 2, and step0 shrinks with the zoom.
 * At 320 wide the worst corner the game can reach - zoom 8 at camera height
 * 25 - works out at 31, which is how 32 was arrived at: exactly one ring of
 * headroom. Double the width and that same corner is 60.
 *
 * Nothing BREAKS at 32 - `cache` turns itself off and every lookup goes to the
 * map instead, exactly as the paragraph above promises. But it turns off for
 * every zoom below 16, and it then costs six map lookups per cell instead of
 * one, over four times as many cells: a slowdown of more than an order of
 * magnitude, arriving precisely at the wide zoom where the frame is already
 * the heaviest. 25 KB of BSS to keep the cache alive across the whole zoom
 * range is not a close call.
 *
 * The Amiga keeps 32. It cannot reach past 31, so the extra would be 25 KB of
 * a 68020's memory bought to hold nothing. */
#define GTA_MAX_RINGS 64
#else
#define GTA_MAX_RINGS 32
#endif

/* Largest lid height the row map covers. A lid taller than this is off the
 * bottom of a 320x200 frame long before it is reached. */
#define GTA_VROW_MAX 256

/* Zoom range, in screen pixels per block at the reference level. Continuous
 * between these, one pixel at a time.
 *
 * 32 is the default and it is not arbitrary: GTA's blocks are 64x64 and the
 * DOS build ran at 640x480, which is ten blocks across the screen - the same
 * field of view as 32-pixel blocks on our 320x200. So the default zoom here is
 * the original's, and the art is at its intended size.
 *
 * **The original's zoom LIMITS are still not confirmed, but the PROJECTION now
 * is.** What has not been read out is the range the camera slides over as the
 * car speeds up. These bounds are chosen for what the renderer can do: at 16
 * the
 * visible region is four times as many blocks and the frame is walk-bound
 * (PERF.md), and at 64 one block fills a fifth of the screen, which is the
 * "unplayably zoomed in" view the tile-scale decision rejected once already.
 *
 * ONE THING THE ORIGINAL DOES THAT WE DO NOT: it uses a different camera
 * HEIGHT per zoom mode (hizoom 300, lozoom 400), so its splay changes with the
 * zoom. Ours is one height for every zoom. If the perspective ever looks wrong
 * at the extremes of `-`/`=`, that is why. */
/* 8 rather than 16 because the HALF-RESOLUTION mode hands the renderer half
 * the displayed zoom: a player asking for 16 pixels per block on screen means
 * 8 in a 160x100 buffer. Nothing offers 8 as a displayed zoom. */
#define GTA_ZOOM_MIN 8
#define GTA_ZOOM_MAX 64

/* Arena for the pre-scaled lid cache. Populating every (lid, rotation, level)
 * would be about 5.6 MB at the default zoom; a view needs a few hundred
 * entries, so this is sized for the working set rather than the worst case.
 * When it fills, blits fall back to scaling per pixel - slower, never wrong -
 * and `lc_full` counts how often, so a bad size shows up in the log instead of
 * as a mystery. */
#define GTA_LIDCACHE_BYTES (1536UL * 1024UL)

/* How many sprites one frame may hold. Everything on top of the city is one -
 * the player, pedestrians, cars, explosions - and the list is refilled from
 * scratch every frame by whatever is simulating them.
 *
 * 64 is not a guess at how busy Liberty City gets; it is what fits the
 * ORDERING scheme. Sprites are sorted into the layer loop, so the list is
 * walked once per layer, six times a frame. At 64 that is 384 comparisons,
 * which is lost in the noise beside 2700 map lookups. A hundred pedestrians
 * would want a bucket per layer instead, and that is the change to make when
 * there are a hundred pedestrians - not before. */
#define GTA_MAX_SPRITES 64

/* One sprite queued for this frame.
 *
 * The position is in the same units as the camera - 16.16 world pixels at the
 * reference scale, 32 to a block - so a sprite and the camera can be given the
 * same coordinates and mean the same place.
 *
 * `layer` is what fixes the draw order, and it is a map layer rather than a
 * grid level on purpose: the renderer's outer loop is the layer loop, and a
 * sprite standing on the lid of layer L is drawn after layer L and before
 * layer L+1. That is what lets a pedestrian walk UNDER a bridge and be hidden
 * by it, which drawing all the sprites after the city could never do.
 *
 * `grid` is the plane the sprite's feet are on - layer + 1 for something
 * standing on that layer's lid - and it decides both where on screen the
 * sprite lands and how big it is drawn. */
typedef struct {
    long wx, wy;        /* 16.16 world pixels, reference scale */
    int  layer;         /* map layer, for draw order */
    int  grid;          /* grid level of the surface it stands on */
    int  index;         /* index into gta_tiles.sprites */
    int  angle;         /* 0..255, 0 = north, clockwise (gta_trig.h) */
    /* PALETTE REMAP, 0 for none. The sprite's pixels are looked up through
     * table `remap` of the baked set before they are written - which is how
     * one pedestrian sheet dresses a whole city and one car sprite comes in a
     * dozen colours. Ranges are in gta_style.h: 1..43 cars, 128..187 people. */
    int  remap;
    /* SPRITE DELTA to lay over the base art, or -1 for the plain sprite.
     * An open car door, a damage panel, a brake light: see gta_tiles.h. The
     * overlay is decoded into the view's scratch buffer at draw time, because
     * the base sprite in the tile set is shared and must never be edited. */
    int  delta;
} gta_sprite_req;

typedef struct {
    const gta_map   *map;
    const gta_tiles *tiles;

    /* Camera position, 16.16 fixed point, in world pixels at the reference
     * scale (so one block is GTA_TILE_DIM = 32 of these). The point named here
     * lands exactly in the middle of the screen. */
    long cam_x, cam_y;

    unsigned char *dst;
    int dst_w, dst_h, dst_pitch;

    /* Zoom: screen pixels per block at the reference level, anywhere between
     * GTA_ZOOM_MIN and GTA_ZOOM_MAX. GTA_TILE_DIM (32) is the default and the
     * only value where a street tile needs no scaling at all. */
    int zoom_px;
    int cam_h;          /* camera height in QUARTER grid levels - GTA_CAM_H */

    /* Palette index the frame is cleared to. 0 (black) is what the game wants,
     * but a diagnostic run can set it to something loud and then "nothing was
     * drawn here" stops looking exactly like "a black tile was drawn here" -
     * which is the difference between a renderer bug and the map's own
     * artwork, and it is not a distinction the eye can make on its own. */
    int clear_index;

    /* FLAT 2D: every grid level drawn at the same pitch, i.e. a camera
     * infinitely far away instead of GTA_CAM_H levels up.
     *
     * This is the mode for a machine that cannot afford the 2.5D projection,
     * and it is a projection change rather than a second renderer - three
     * things fall out of it on their own:
     *
     *   - a block's lid lands on the block's own footprint, so nothing is
     *     displaced outward and there is no parallax between levels;
     *   - a wall's two x planes coincide, so its rectangle has zero width and
     *     draw_block skips it. That removes every masked per-pixel blit;
     *   - only the topmost opaque lid of a column can be seen, because the
     *     ones below land in exactly the same rectangle - so the layer loop
     *     starts at that lid instead of at zero (see col_top).
     *
     * Collision does not read any of this - it reads the map's ground_type -
     * so the two modes play identically. It is a display option, not a fork of
     * the game. */
    int flat_2d;

    /* Diagnostic: skip every blit but do all the walking, projection and map
     * lookups. The difference against a normal frame is what the pixel loops
     * cost, which is the number that decides whether Phase 7 should attack the
     * blitters or the traversal. */
    int debug_no_blits;

    /* Rebuilt by gta_render_frame(); public because the host preview prints
     * them and because a debugger is more useful when they are visible. */
    long step[GTA_GRID_LEVELS];   /* 16.16 tile pitch of each grid level, X */

    /* AND THE SAME PITCH DOWN THE SCREEN, WHICH IS NOT THE SAME NUMBER.
     *
     * The original squashes Y to five sixths of X, and that is not a guess or
     * a taste: it is written twice in the original. The projection itself is
     *
     *   *x = (((wx - camx) / z + 0x7f) * zoom >> 16) + centre_x;
     *   *y = ((zoom * ((wy - camy) / z + 0x7f) >> 16) * 5) / 6 + centre_y;
     *
     * - the `* 5 / 6` is on the Y line and nowhere else. The routine that
     * fills the lattice of screen coordinates for one grid level says it
     * again in the step it walks the lattice with: `x += step * 0x40` against
     * `y += (step * 0x140) / 6`, i.e. 64 across and 53.33 down.
     *
     * WHY IT EXISTS: a block is square in the world, and the screen it was
     * drawn for does not have square pixels. Squashing Y by 5/6 is what makes
     * a square block look square. The visible consequence is field of view -
     * the original shows 7.5 blocks down the screen where an unsquashed 200
     * rows show 6.25 - and that 1.2x is exactly what the developer measured
     * against the PC build: three lanes of road there, two and a half here.
     *
     * Everything downstream has to carry two numbers instead of one: the lid
     * cache is w x h rather than w x w, the ring radius ry comes from stepy,
     * and the sprite rotozoom scales its two axes separately. */
    long stepy[GTA_GRID_LEVELS];  /* 16.16 tile pitch of each grid level, Y */
    /* THE RESERVATION OVERLAY - the developer's debugging eyes. When on,
     * every junction square gets a per-pixel checkerboard: black = free,
     * a colour = booked by that car (9 colours, serial-keyed), and each
     * committed car's start square shows in its colour too. Drawn after
     * the ground, under the sprites. ov_tr is the gta_traffic, opaque
     * here to keep the header free of the traffic types. */
    const void *ov_tr;
    int ov_on;
    long ox[GTA_GRID_LEVELS];     /* 16.16 screen x of the camera block's
                                   * left edge, per level                 */
    long oy[GTA_GRID_LEVELS];

    long recip[GTA_RECIP_MAX + 1]; /* (32 << 16) / n, so no runtime divide */

    /* Column heights for the visible region, filled by the layer-0 pass and
     * read by the five above it.
     *
     * The draw order is layer-first (see gta_render.c), so the visible region
     * is walked once per layer. Without this cache each of those walks asks
     * the map for every column's height again - about 2700 lookups a frame
     * where 225 would do, and gta_map.h warns that the accessor is cheap but
     * not free. */
    unsigned char col_h[(2 * GTA_MAX_RINGS + 1) * (2 * GTA_MAX_RINGS + 1)];

    /* FLAT 2D ONLY: the topmost layer of each column that carries an opaque
     * lid. Everything below it is hidden by that lid - exactly, because in
     * flat 2D the lids of one column all project onto the same rectangle - so
     * the layer passes below it can be skipped outright. Filled by the layer-0
     * pass alongside col_h.
     *
     * It is wrong in 2.5D and must never be used there: a lower lid IS visible
     * in 2.5D, displaced inward from the one above it, and skipping it is what
     * would put holes in every building. */
    unsigned char col_top[(2 * GTA_MAX_RINGS + 1) * (2 * GTA_MAX_RINGS + 1)];

    /* --- pre-scaled lid cache -------------------------------------------
     *
     * Only GTA_GREF is drawn at exactly 32 pixels, so only the street gets a
     * straight row copy; every other grid level goes through the per-pixel
     * scaling loop, which is about six operations a pixel instead of one. At
     * the default zoom six of the seven levels are on that slow path, and
     * zoomed out ALL of them are - which is why 16 px per block costs nearly
     * half the frame rate.
     *
     * So each (lid, rotation, level) is scaled once, on first use, into an
     * arena, and every later blit of it is a copy. Filling is lazy because
     * populating all of them would be 5.6 MB: a view uses a few hundred.
     *
     * A tile's on-screen size is made CONSTANT per level - ceil(step) - so a
     * cache entry is valid wherever the camera is. Rounding up rather than
     * down means neighbouring tiles overlap by at most a pixel instead of
     * occasionally leaving a gap, which is the safe direction here; this
     * renderer's whole history says gaps are what to fear. Ceiling also leaves
     * a level whose step is a whole number of pixels - the street - at exactly
     * 32, so it keeps the memcpy path and needs no cache entry at all. */
    unsigned char  *lc_arena;
    unsigned char **lc_slot;
    unsigned long   lc_used;
    int             lc_slots;
    int             lc_w[GTA_GRID_LEVELS];   /* ceil(step)  per level, pixels */
    int             lc_h[GTA_GRID_LEVELS];   /* ceil(stepy) per level, pixels */

    /* WHICH SOURCE ROW EACH DESTINATION ROW OF A FULL-WIDTH LID COMES FROM.
     *
     * The 5/6 squash means a lid that needs no horizontal scaling still needs
     * vertical scaling - 32 wide by 27 tall at the reference level - and that
     * is every level in flat 2D. Doing it arithmetically costs a multiply, a
     * shift and a clamp per ROW inside what is otherwise a pure memcpy loop.
     *
     * The mapping depends only on the level, so it is built once whenever the
     * lid cache is rebuilt and read with a single byte load. 32 levels' worth
     * would be 6 KB; capping it at GTA_VROW_MAX keeps it under 2 KB and covers
     * every lid height a 320x200 frame can show. Past that blit_vscale falls
     * back to stepping an accumulator, which is still cheaper than the multiply
     * it replaced. */
    unsigned char   lc_vrow[GTA_GRID_LEVELS][GTA_VROW_MAX];
    int             lc_vrow_ok[GTA_GRID_LEVELS];
    int             lc_zoom;                 /* zoom lc_w/lc_slot were built for */
    int             lc_flat;                 /* ...and which projection */
    int             lc_cam_h;                /* ...and which camera height */
    long            lc_fills, lc_full;

    /* Sprites queued for this frame. Cleared by gta_render_frame() once it has
     * drawn them, so the caller re-adds every frame and never has to remove. */
    gta_sprite_req sprites[GTA_MAX_SPRITES];
    int n_sprites;

    /* WHERE A SPRITE WITH A DELTA IS ASSEMBLED. One buffer, grown on demand
     * and never shrunk, because only one sprite in a frame has ever needed
     * one - the car the player is getting into. A game that never opens a
     * door never allocates it. */
    unsigned char *spr_scratch;
    unsigned long  spr_scratch_cap;

    /* Counters for the last frame, for the log and for measurement. */
    long lids_drawn, walls_drawn, columns_visited, sprites_drawn;
} gta_view;

/* Bind a map and a tile set. Fills the reciprocal table - the only thing here
 * that costs a division - and allocates the pre-scaled lid cache. The renderer
 * works without that allocation, just slower, so a failure is not fatal. */
void gta_render_init(gta_view *v, const gta_map *map, const gta_tiles *tiles);

/* Release what gta_render_init() allocated. */
void gta_render_free(gta_view *v);

/* Where frames go. Safe to call again after a resize. */
void gta_render_target(gta_view *v, unsigned char *dst, int w, int h, int pitch);

/* Put the camera on the centre of block (bx, by). */
void gta_render_look_at_block(gta_view *v, int bx, int by);

/* Move the camera by whole pixels at the reference scale. Clamped to the map.
 *
 * Deliberately in WORLD pixels (one block is always GTA_TILE_DIM of them), not
 * screen pixels, so the camera travels the city at the same speed whatever the
 * zoom is. */
void gta_render_move(gta_view *v, int dx_px, int dy_px);

/* Change the zoom by `delta` pixels per block and return the new value.
 * Continuous, one pixel at a time, clamped to GTA_ZOOM_MIN..GTA_ZOOM_MAX. */
int gta_render_zoom(gta_view *v, int delta);

/* Set the zoom directly, clamped to the same range. */
void gta_render_set_zoom(gta_view *v, int px);

/* The camera height, live. F7 lowers it (more dramatic, taller walls), F8
 * raises it (flatter, closer to a plain top-down map). Clamped to
 * [GTA_CAM_H_MIN, GTA_CAM_H_MAX] - the lower bound stops a divide by zero. */
void gta_render_set_cam_h(gta_view *v, int h);
int  gta_render_cam_h(gta_view *v, int delta);

/* Queue a sprite for the NEXT frame. Returns 0 if the list is full, in which
 * case the sprite is silently not drawn - a dropped pedestrian is better than
 * a refused frame, and v->n_sprites says how close to the limit a scene runs.
 *
 * Coordinates are 16.16 world pixels at the reference scale, the same units as
 * the camera. See gta_sprite_req for what `layer` and `grid` mean and why they
 * are separate. */
int gta_render_add_sprite(gta_view *v, long wx, long wy, int layer, int grid,
                          int index, int angle);

/* The same, with a palette remap table - see gta_sprite_req.remap. Pass 0 and
 * it is exactly gta_render_add_sprite(). */
int gta_render_add_sprite_r(gta_view *v, long wx, long wy, int layer, int grid,
                            int index, int angle, int remap);

/* The same again, with a sprite DELTA laid over the base art - see
 * gta_sprite_req.delta. Pass -1 and it is exactly gta_render_add_sprite_r(). */
int gta_render_add_sprite_d(gta_view *v, long wx, long wy, int layer, int grid,
                            int index, int angle, int remap, int delta);

/* Draw one sprite immediately, rotated and scaled, wherever the camera
 * currently is. gta_render_frame() calls this at the right point in the layer
 * loop; it is exposed because the host tools draw sprites without a frame
 * around them. Grid levels must already be set up, i.e. after a frame. */
void gta_render_sprite(gta_view *v, const gta_sprite_req *sp);

/* Draw one frame into the target. Clears it first, then draws the city and the
 * queued sprites, then empties the sprite list. */
void gta_render_frame(gta_view *v);
void gta_render_set_overlay(gta_view *v, const void *tr, int on);

/* Blow a reduced-resolution frame up into the display buffer, replicating each
 * pixel sx across and sy down. sx and sy are 1 or 2.
 *
 * WHY THE RENDERER OWNS THIS and not the platform layer: it is the same code
 * on AGA, on RTG and on the host, it touches no Amiga header, and the host
 * tools have to be able to look at exactly what the Amiga will show. The
 * horizontal pass writes one 32-bit word per two source pixels rather than two
 * bytes, which is what keeps it cheap enough to be worth doing at all - the
 * point of a reduced mode is to stop drawing pixels, not to spend the saving
 * on copying them.
 *
 * This does NOT reduce chunky-to-planar: the display buffer is still full
 * size, so c2p costs what it always did. What it reduces is the walk and the
 * blits, which together are 69% of the frame. Cutting c2p as well means a
 * physically smaller screen, which is a platform-layer decision and is
 * written up in the notes. */
void gta_render_expand(const unsigned char *src, int sw, int sh, int spitch,
                       unsigned char *dst, int dpitch, int sx, int sy);

#endif /* GTA_RENDER_H */
