/* THE LEVEL SCRIPT'S DECLARATION BLOCK - the objects a mission is played
 * with, read out of gtadata/mission.ini.
 *
 * WHAT THIS IS AND WHY IT IS SEPARATE FROM THE INTERPRETER. A level script
 * has two halves and the file does not mark the boundary: every line in a
 * section is either a DECLARATION - a thing that exists in the world, with a
 * position - or a COMMAND - a step of the logic, with none. The original
 * tells them apart by whether the line carries coordinates, parses the
 * declarations into 1400 object records and the commands into 5000 command
 * records, and keeps two 65536-entry maps from a line number to an index in
 * either. This file is the first of those
 * two halves; the interpreter is the next step and will read what is here.
 *
 * A line looks like
 *
 *     id [flag] (x,y,z) NAME a b [c d]
 *
 * `id` is the line number, which is also the handle every other line uses to
 * refer to this one. `flag`, when present, means PERMANENT - the object
 * survives the script's RESET; 2 additionally marks a kill-frenzy car. The
 * coordinates are stored as the file writes them, because whether they are
 * blocks or pixels DEPENDS ON THE TYPE (a TELEPHONE is in blocks, a CRANE in
 * pixels), and converting here would lose that; gta_script_is_block() says
 * which, and the game side converts when it places the thing.
 *
 * In Liberty City's section [1] there are 575 of these: 151 POWERUP (which
 * gta_pickup.c already reads on its own), 128 TELEPHONE, 95 DUMMY, 55
 * TRIGGER and the rest.
 *
 * Licence: MIT (ours).
 */
#ifndef GTA_SCRIPT_H
#define GTA_SCRIPT_H

#include "gta_tiles.h"
#include "gta_nav.h"
#include "gta_render.h"


/* The type ids are the original's own - the index of the name in its object
 * type table, so the original's own `case 0x32` and a POWERUP here are the
 * same number. */
#define GTA_DECL_CAR         0
#define GTA_DECL_PED         1
#define GTA_DECL_OBJECT      2
#define GTA_DECL_PLAYER      3
#define GTA_DECL_DRIVER      4
#define GTA_DECL_PARKED      5
#define GTA_DECL_TELEPHONE   7
#define GTA_DECL_TRIGGER     8
#define GTA_DECL_DOOR        9
#define GTA_DECL_TARGET     10
#define GTA_DECL_FUTURE     11
#define GTA_DECL_COUNTER    12
#define GTA_DECL_CRANE      13
#define GTA_DECL_DUMMY      15
#define GTA_DECL_SPRAY      16
#define GTA_DECL_BOMBSHOP   20
#define GTA_DECL_FUTUREPED  24
#define GTA_DECL_CARTRIGGER 25
#define GTA_DECL_HELLS      27
#define GTA_DECL_FUTURECAR  29
#define GTA_DECL_MPHONES    34
#define GTA_DECL_PHONE_TOGG 39
#define GTA_DECL_DUM_MISSION_TRIG 42
#define GTA_DECL_POWERUP    50
#define GTA_DECL_GUN_TRIG   57

/* Ours, for the names the original keeps elsewhere - they are declarations in
 * the file all the same and the port has to carry them. Numbered from 200 so
 * they can never collide with the table above. */
#define GTA_DECL_GTA_DEMAND      200
#define GTA_DECL_MISSION_COUNTER 201
#define GTA_DECL_SECRET_MISSION_COUNTER 202
#define GTA_DECL_MISSION_TOTAL   203
#define GTA_DECL_TARGET_SCORE    204
#define GTA_DECL_UNKNOWN         255

/* A TRIGGER'S STATE, and the numbers are the original's own
 * 1 armed, 3 started, 4 disabled. It lives on
 * the declaration because that is where the original keeps it - the object
 * record IS the trigger - and because a trigger is the only declaration
 * with any state at all so far. */
#define GTA_TRIG_NONE     0
#define GTA_TRIG_ARMED    1
#define GTA_TRIG_STARTED  3
#define GTA_TRIG_DISABLED 4
/* A DOOR keeps its state in the same field, in numbers a trigger never uses,
 * so the trigger scan (which fires only on ARMED) can never mistake one for
 * the other. A door starts SHUT: the declaration creates it "off until
 * DOOR_ON", as the original has it. */
#define GTA_DOOR_SHUT     5
#define GTA_DOOR_OPEN     6

typedef struct {
    short line;                 /* the line number, and the handle */
    unsigned char type;         /* GTA_DECL_* */
    unsigned char flag;         /* 0 temporary, 1 permanent, 2 frenzy car */
    short x, y, z;              /* as written - blocks or pixels, see below */
    short a, b, c, d;           /* the numbers after the name, 0 if absent */
    unsigned char n_num;        /* how many of a..d the line actually had */
    unsigned char state;        /* GTA_TRIG_*, triggers only */
    short counter;              /* COUNTER declarations only - the running
                                 * value. The original keeps it in the
                                 * object's handle field and starts it at the
                                 * declaration's first number: COUNTER 0 0 is
                                 * zero, MISSION_COUNTER 4 0 is four. */
    unsigned long handle;       /* what the world made of this declaration -
                                 * for a car, the fleet serial. 0 = nothing
                                 * has been made yet. */
} gta_decl;

/* A DECLARED OBJECT THAT IS IN THE WORLD. The declaration says where in
 * blocks; this is where in world pixels, on the layer it stands on, with the
 * heading in the port's 256ths. The phones are the first of these - 128 of
 * them in Liberty City - and the parked mission cars and the triggers will
 * join them. */
typedef struct {
    long x, y;                  /* 16.16 world pixels, the block centre */
    short line;                 /* the declaration it came from */
    unsigned char layer;
    unsigned char angle;
    /* A TELEPHONE'S STATE, the original's object state `+0xc`: 0 idle, 1 and
     * 2 the two RINGING frames it flips between. The style file has all
     * three - sprite 249 is the plain blue phone, 250 the same with a red
     * arrow beside it and 251 the phone itself red - so the ring is data,
     * not something this port has to draw. */
    unsigned char ring;
    /* ANSWERED. The original sets the object's handle to -1 and the phone
     * never rings again for the rest of the level. */
    unsigned char dead;
} gta_placed;

#define GTA_MAX_PHONES 160
#define GTA_MAX_MCARS   16

/* A MISSION CAR - one the script PARKS somewhere in the city.
 *
 * It cannot simply be handed to the traffic fleet at load time: the fleet is
 * a pool of about twenty cars kept AROUND THE PLAYER, and its recycler takes
 * any slot whose car is off screen, so nine cars scattered over Liberty City
 * are gone within seconds of the level starting - measured, they were not
 * there when the player walked up to where the file puts them. So the script
 * keeps them itself and hands one over when the player comes near, once. */
typedef struct {
    long x, y;
    short line;                 /* the declaration, and the handle */
    short model;                /* the port's car-table RECORD, not the
                                 * original's model id */
    unsigned char layer, angle;
    unsigned char done;         /* handed to the fleet already */
} gta_mcar;

/* ---- THE LOGIC BLOCK AND ITS INTERPRETER --------------------------------
 *
 * The other half of a section: `id NAME p1 p2 p3 p4 p5`, no coordinates. The
 * file separates the two halves with a line whose label is NEGATIVE, and the
 * logic block ends at the next one - in Liberty City's section [1] that is
 * 575 declarations, then 2116 commands.
 *
 * HOW A COMMAND ADVANCES, and it is the same for almost all of them
 * `p2` is where to go on SUCCESS and `p3` on
 * FAILURE, where 0 means the next command in FILE ORDER, -1 means end this
 * process (and re-arm the trigger that started it), and anything else is a
 * LINE NUMBER to jump to. `p5` is a score paid on success. A command that
 * wants its five numbers for something else says so, per command.
 *
 * The original runs 32 processes and executes ONE COMMAND PER ACTIVE PROCESS
 * PER TICK at 25 Hz - not one per frame and not the whole block at once,
 * which is what makes SURVIVE and the side processes work at all.
 */
#define GTA_MAX_CMDS   2400
#define GTA_MAX_PROC     32
#define GTA_MAX_CNAMES   96

/* The commands this port acts on. The numbers are the original's opcodes -
 * the index of the name in its command table - so the original's own
 * `case 0x59` and KICKSTART here are the same thing. Everything
 * else is carried as GTA_CMD_OTHER and takes the success path, so the script
 * walks its own logic instead of stopping at the first thing not written
 * yet; the log counts how often each was hit, which is the list of what to
 * implement next. */
#define GTA_CMD_SURVIVE    4
#define GTA_CMD_DONOWT    17
#define GTA_CMD_DISABLE   18
#define GTA_CMD_ENABLE    19
#define GTA_CMD_MISSION_END 45
#define GTA_CMD_STARTUP   60
#define GTA_CMD_KICKSTART 89
#define GTA_CMD_KILL_SIDE_PROC 98
#define GTA_CMD_MOBILE_BRIEF 111
#define GTA_CMD_POWERUP_ON  64
#define GTA_CMD_POWERUP_OFF 100
#define GTA_CMD_IS_POWERUP_DONE 133
#define GTA_CMD_DECCOUNT   20
#define GTA_CMD_INCCOUNT  127
#define GTA_CMD_COMPARE   128
#define GTA_CMD_ARROW      26
#define GTA_CMD_ARROW_OFF  36
#define GTA_CMD_ARROWPED   51
#define GTA_CMD_ARROWCAR   52
#define GTA_CMD_MPHONE     16
#define GTA_CMD_ANSWER      2
#define GTA_CMD_STEAL       3
#define GTA_CMD_CAR_ON     33
#define GTA_CMD_CHECK_CAR  85
#define GTA_CMD_PARKED_ON  88
#define GTA_CMD_IS_PED_IN_CAR 90
#define GTA_CMD_IS_GOAL_DEAD 105
#define GTA_CMD_BRIEF          15
#define GTA_CMD_P_BRIEF        37
#define GTA_CMD_P_BRIEF_TIMED  68
#define GTA_CMD_CANCEL_BRIEFING 91
#define GTA_CMD_SPEECH_BRIEF  110
#define GTA_CMD_MESSAGE_BRIEF 116
#define GTA_CMD_FRENZY_BRIEF  135
#define GTA_CMD_MISSION_END_S  45
#define GTA_CMD_PARK           23
#define GTA_CMD_DOOR_ON        70
#define GTA_CMD_DOOR_OFF       71
#define GTA_CMD_OPEN_DOOR      73
#define GTA_CMD_CLOSE_DOOR     74
#define GTA_CMD_RESET         129
#define GTA_CMD_RESET_BRIEFS  145
#define GTA_CMD_KEEP_THIS_PROC 130
#define GTA_CMD_KILL_PROCESS   96
#define GTA_CMD_KILL_SPEC_PROC 101
#define GTA_CMD_PED_ON          31
#define GTA_CMD_PED_BACK        40
#define GTA_CMD_KILL_PED        79
#define GTA_CMD_IS_PED_ARRESTED 120
#define GTA_CMD_DEAD_ARRESTED  132

/* WHERE A PIECE OF TEXT GOES. The original has two displays and a card, and
 * which one a command uses is fixed:
 *
 *   (a) THE BRIEF BOX along the bottom - an ICON at the left and the text
 *       beside it. Six kinds, and the icon of each is a sprite of the style
 *       file's "arrow" category: the numbering below IS that category's.
 *   (b) THE PAGER at the top left - a little device that scrolls one line.
 *   (c) A BIG CENTRED MESSAGE, up to three words: "MISSION COMPLETE!".
 */
#define GTA_BRIEF_PHONE   0     /* BRIEF        - icon 11, a mobile phone */
#define GTA_BRIEF_SPEECH  1     /* SPEECH_BRIEF - icon 10, a mouth */
#define GTA_BRIEF_MOBILE  2     /* MOBILE_BRIEF - icon 12, a handset */
#define GTA_BRIEF_INFO    3     /* icon 14, a blue "i" */
#define GTA_BRIEF_COP     4     /* icon 13, a policeman's head */
#define GTA_BRIEF_FRENZY  5     /* FRENZY_BRIEF - icon 15, a skull */
#define GTA_BRIEF_PAGER   6     /* P_BRIEF */
#define GTA_BRIEF_PAGER_T 7     /* P_BRIEF_TIMED - `arg` seconds counting down */
#define GTA_BRIEF_BIG     8     /* MESSAGE_BRIEF */
#define GTA_BRIEF_CANCEL  9     /* CANCEL_BRIEFING - `key` is the tag */
#define GTA_CMD_OTHER    254
#define GTA_CMD_NONE     255

typedef struct {
    short line;                 /* the label, and the jump target */
    unsigned char op;           /* GTA_CMD_* */
    unsigned char name;         /* index into the script's name table */
    long p1, p2, p3, p4, p5;
} gta_cmd;

/* A PROCESS. `cmd` is the command index it is about to run; `state` is the
 * original's "fresh" flag, which a timed command uses as its counter. */
typedef struct {
    short cmd;                  /* -1 when the slot is free */
    short trigger;              /* the declaration line that started it */
    long  count;                /* SURVIVE and friends */
    /* A SECOND COUNTER. ANSWER needs two - how many ring cycles are left and
     * how far through the current one it is - and the original keeps the
     * second in a per-process scratch word of its own. */
    long  count2;
    unsigned char fresh;
    /* THE KICKSTART THAT MADE THIS ONE, by line, so KILL_SIDE_PROC can
     * name it: `60 KILL_SIDE_PROC 32001` stops the child that
     * `32001 KICKSTART 13` started. -1 for a process a trigger started. */
    short parent;
    /* KEEP_THIS_PROC: RESET leaves it alone. */
    unsigned char keep;
} gta_proc;

/* WHAT THE INTERPRETER IS ALLOWED TO TOUCH.
 *
 * The script must not reach into the game - this module is built by the host
 * tools as well, and gta_pickup/gta_traffic/gta_peds are not theirs to know
 * about. So the game hands it a table of things it may ask for, the way it
 * already hands over one function for the pager brief. Every entry may be
 * null; a command whose callback is missing takes the success path and is
 * counted, which is what every unwritten command does.
 *
 * Positions are BLOCKS, because that is what the declarations carry and what
 * the original stores for a POWERUP (`x := x*0x40+0x20` is the pixel centre
 * of a block). */
typedef struct {
    /* POWERUP_ON: put a crate of `kind` holding `amount` on that block. */
    void (*powerup_on)(void *ctx, int line, int kind, int amount,
                       int bx, int by);
    /* POWERUP_OFF: take it away again, collected or not. */
    void (*powerup_off)(void *ctx, int line, int bx, int by);
    /* IS_POWERUP_DONE: non-zero when it has been COLLECTED - which the
     * original reads as "there is no longer one at that position". */
    int  (*powerup_done)(void *ctx, int line, int bx, int by);
    /* ARROW: point at (wx, wy), 16.16 world pixels, or switch it off. */
    void (*arrow)(void *ctx, int on, long wx, long wy);

    /* ---- CARS. A job is nearly always about one particular car, so the
     * world has to be able to make one and then be asked about it later.
     * The handle is the fleet's own serial and it survives the player
     * driving the car about. ---- */

    /* PARKED_ON / CAR_ON: put the declared car down. `model_id` is the
     * ORIGINAL'S model number, which is not this port's car-table record -
     * the world side converts. Returns the handle, 0 when it could not. */
    unsigned long (*car_on)(void *ctx, int line, int model_id,
                            int bx, int by, int angle);
    /* Where it is now; 0 when it is nowhere - which for a mission car
     * usually means the player is driving it, so ask player_car() too. */
    int  (*car_pos)(void *ctx, unsigned long h, long *wx, long *wy);
    /* Wrecked, or gone from the world altogether. */
    int  (*car_dead)(void *ctx, unsigned long h);
    /* The car the player is driving, 0 when he is on foot. */
    unsigned long (*player_car)(void *ctx);
    /* ...and its model in the ORIGINAL'S numbering, or -1. */
    int  (*player_car_model)(void *ctx);
    /* ARROWCAR: point at a car and keep pointing as it moves. */
    void (*arrow_car)(void *ctx, unsigned long h);
    /* MONEY. `p5` on a command's success path is a score, and the original
     * multiplies it by the player's mission multiplier. */
    void (*score)(void *ctx, long points);
    /* THE PLAYER'S CAR, for PARK: where it is and whether it has stopped.
     * 0 when he is on foot. */
    int  (*player_car_at)(void *ctx, long *wx, long *wy, int *stopped);
    /* MISSION_END: one more on the score multiplier, for good. */
    void (*mission_done)(void *ctx);
    /* RESET: this car is not the mission's any more - the fleet may sweep
     * it up like any other. */
    void (*car_release)(void *ctx, unsigned long h);

    /* ---- PEOPLE. A job's passenger, victim or contact: one particular
     * person the script creates, points at and puts in a car. ---- */

    /* PED_ON: put the declared person at (wx, wy) facing `angle`. Returns a
     * handle, 0 when there is no room. */
    unsigned long (*ped_on)(void *ctx, int line, long wx, long wy, int angle);
    /* Where he is; 0 when he is gone. */
    int  (*ped_pos)(void *ctx, unsigned long h, long *wx, long *wy);
    /* Dead, or gone from the world. */
    int  (*ped_dead)(void *ctx, unsigned long h);
    /* ARROWPED. */
    void (*arrow_ped)(void *ctx, unsigned long h);
    /* PED_BACK: is he in that car yet? Taking him out of the world when he
     * is, which is what "he got in" looks like from outside. */
    int  (*ped_into_car)(void *ctx, unsigned long ped, unsigned long car);
    /* KILL_PED. */
    void (*ped_kill)(void *ctx, unsigned long h);
    /* THE PLAYER HIMSELF: is he being arrested, and is he dead? A job
     * watches both and gives up when either happens. */
    int  (*player_arrested)(void *ctx);
    int  (*player_dead)(void *ctx);
    /* THE SPRAY SHOP: paint the car the player is driving `remap` and lose
     * the heat with it. 0 when he is not in one - on foot, or the shop is
     * shut because he has just used it. */
    int  (*respray)(void *ctx, int remap);
} gta_script_world;

typedef struct {
    gta_decl *d;
    int n;                      /* declarations kept */
    int n_lines;                /* lines seen in the section */
    int n_unknown;              /* names this port does not know yet */

    void (*brief_fn)(void *ctx, int kind, int key, int arg);
    void *brief_ctx;
    /* THE SCRIPT'S OWN CLOCK IS 25 Hz. The simulation runs at 50 and the
     * original's interpreter at 25 - "one command per active process per
     * tick", and ANSWER's `p4` cycles are 50 ticks of twenty seconds
     * (MISSIONS.md 6.4), which is 25 a second. Ticking it with the
     * simulation ran every SURVIVE and every ring at double speed. */
    int half;
    const gta_script_world *world;
    void *world_ctx;

    gta_placed phone[GTA_MAX_PHONES];
    int n_phones;
    gta_mcar mcar[GTA_MAX_MCARS];
    int n_mcars;

    gta_cmd *c;
    int n_cmds;
    char cname[GTA_MAX_CNAMES][16];
    int n_cnames;
    long hit[GTA_MAX_CNAMES];   /* how often each name was executed */

    gta_proc proc[GTA_MAX_PROC];
    int n_started, n_ended, n_steps;
    long n_rearmed, n_disabled, n_enabled;
    /* THE PHONES. Only one run may ring at a time in the whole city
     * anywhere, and for 250 ticks after answering one the player is
     * "on a phone job" and no other phone will start. */
    int phone_ringing;
    int phone_cool;
    long n_answered;
    long n_resprays;
    long px, py;                /* the player, as of this tick */
    int  on_foot;
    int spr_phone;              /* map object type 0x28, or -1 */
} gta_script;

/* Read section `level` of a mission.ini. Returns 0 on success; the script is
 * empty and harmless when the file is missing, which is how the port treats
 * every optional data file. */
int  gta_script_load(gta_script *s, const char *path, int level);
void gta_script_free(gta_script *s);

/* The declaration with this line number, or 0. Handles are how one line
 * refers to another - a PHONE_TOGG names its MPHONES, a CARTRIGGER names its
 * PARKED car - so this is the lookup the interpreter will live on. */
const gta_decl *gta_script_by_line(const gta_script *s, int line);

/* Are this type's coordinates in BLOCKS (as against world pixels)? The file
 * mixes the two and the type is what says which. */
int  gta_script_is_block(int type);

/* PUT THE DECLARED OBJECTS IN THE WORLD. Reads the layer each one stands on
 * out of the navigation grid, the way the crates do, because the file's own z
 * is not the port's layer. Returns how many were placed. */
int  gta_script_place(gta_script *s, const gta_nav *nav, const gta_tiles *t);

/* WHICH RECORD OF THE CAR TABLE IS THE ORIGINAL'S MODEL `model_id`.
 *
 * The script writes the ORIGINAL'S model numbers - PARKED 44 is the tanker -
 * and this port indexes the car table by RECORD, which is a different number
 * for everything past the first few (the tanker is record 32). The table
 * carries the original's id in `model_id`, so this is a search through 38
 * records, done once when the level loads. -1 when the style has no such
 * model.
 *
 * Getting this wrong is silent: the wrong car appears and nothing complains.
 * The record's own numbering has already been confused once in this port
 * (POLICE.md's model 4 is a record index, not a model id). */
int  gta_script_model_index(const gta_tiles *t, int model_id);

/* The layer a thing declared at this block stands on - out of the navigation
 * grid, because the file's z is not this port's layer. -1 when nothing there
 * can be stood on. */
int  gta_script_stand_layer(const gta_nav *nav, int bx, int by);

/* WHERE A DECLARATION IS, in 16.16 world pixels. Handles both halves of the
 * file's split: a block-addressed type is centred in its block, a pixel one
 * is the original's 64-units-per-block halved into this port's 32. Returns 0
 * when there is no such line. */
int gta_script_decl_pos(const gta_script *s, int line, long *wx, long *wy);

/* The block centre in 16.16 world pixels, and the file's 1024ths as this
 * port's 256ths. */
#define gta_script_centre(b)  ((((long)(b) * 32 + 16)) << 16)
#define gta_script_angle(r)   ((int)(((r) / 4) & 255))

/* Draw what is within `blocks` of the camera. */
void gta_script_draw(gta_script *s, gta_view *v, int blocks);


/* Read the LOGIC block of the same section - the commands. Separate from
 * gta_script_load() because it is a separate half of the file and a separate
 * source file; call it after. */
int  gta_script_load_cmds(gta_script *s, const char *path, int level);

/* ONE TICK OF THE SCRIPT: the trigger scan, then one command for every
 * running process. `px, py` are the player in 16.16 world pixels, and
 * `on_foot` is zero when he is in a car - a phone cannot be answered from
 * the driving seat, which is the whole reason the flag is here. */
void gta_script_tick(gta_script *s, long px, long py, int on_foot);

/* Where a piece of text goes: `kind` is one of GTA_BRIEF_*, `key` the entry
 * in the .FXT, `arg` the seconds for a timed pager line. The interpreter
 * calls this rather than reaching into the game. */
void gta_script_set_brief(gta_script *s,
                          void (*fn)(void *ctx, int kind, int key, int arg),
                          void *ctx);

/* Everything else the interpreter may do to the world. `w` must outlive the
 * script; the game passes a static table. */
void gta_script_set_world(gta_script *s, const gta_script_world *w, void *ctx);

/* What the script did, for the five-second report. */
void gta_script_report(const gta_script *s);

/* The triggers still ARMED within `blocks` of a point, printed. A one-off
 * diagnostic: after the opening block has disabled twenty of them, nothing
 * in the file says which are still live. */
void gta_script_live_triggers(const gta_script *s, long px, long py,
                              int blocks);

/* THE NEXT MISSION CAR THE PLAYER HAS COME NEAR, or -1. Marks it done, so
 * each is handed over once and the fleet owns it from then on. */
int  gta_script_due_car(gta_script *s, long cam_x, long cam_y, int blocks);

/* WHERE THE LEVEL PUTS THE PLAYER. The PLAYER declaration carries the block
 * and, in `b`, the heading in 1024ths; Liberty City's is
 * `294 1 (105,119,4) PLAYER 293 256` - block (105,119), facing east, beside
 * the PARKED car named by `a`. It matters beyond neatness: the level has no
 * entry point of its own, it starts because that block is also TRIGGER 303,
 * and a player put down anywhere else never fires it. Returns 0 when the
 * script has no PLAYER line. */
int gta_script_player_start(const gta_script *s, int *bx, int *by, int *angle);

/* The name of a type, for logs. Never 0. */
const char *gta_script_type_name(int type);

#endif
