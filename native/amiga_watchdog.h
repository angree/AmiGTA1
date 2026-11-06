/* THE WATCHDOG - a hang that reports itself.
 *
 * A Guru is cheap: amiga_trap.c logs its PC and the trap is mapped to a
 * symbol in a minute. A HANG is not. The game stopped once with nothing in
 * the log but a normal frame-rate line, no trap, the reload flag it polls
 * every half second never picked up - and the only fact available was "it
 * stopped". That is the one class of fault this harness could not answer,
 * so this makes it answer.
 *
 * A second process wakes once a second and looks at a heartbeat the main
 * loop bumps every frame. When the heartbeat has not moved for
 * AMIGA_WD_STALE_SECS it writes watchdog.log beside the game: which PHASE
 * of the frame the main loop was last in, whether the main task is
 * spinning or waiting, and the main task's saved program counter and a
 * stack dump - in the same shape as a CPU TRAP report, so the same nm
 * mapping finds the function. The main task is preempted while the
 * watchdog runs, so its registers are on its own stack at tc_SPReg: the
 * fifteen data and address registers Exec pushes at a switch, then the
 * interrupt frame - SR, PC, format. The raw top of that stack is written
 * too, so if that layout is ever wrong on some Kickstart the PC is still in
 * the dump, to be found by eye as the one value inside the code segment.
 *
 * Nothing here is portable and nothing here runs on the host.
 * Licence: MIT (ours).
 */
#ifndef AMIGA_WATCHDOG_H
#define AMIGA_WATCHDOG_H

#define AMIGA_WD_STALE_SECS 8

/* The heartbeat and the phase, written by the main loop only. */
extern volatile unsigned long amiga_wd_beat;
extern volatile int amiga_wd_phase;

#define AMIGA_WD_PHASE_INPUT    1
#define AMIGA_WD_PHASE_SIM      2
#define AMIGA_WD_PHASE_TRAFFIC  3
#define AMIGA_WD_PHASE_PEDS     4
#define AMIGA_WD_PHASE_WEAPONS  5
#define AMIGA_WD_PHASE_PLAYER   6
#define AMIGA_WD_PHASE_RENDER   7
#define AMIGA_WD_PHASE_PRESENT  8
#define AMIGA_WD_PHASE_CAP      9

#define amiga_wd_set(p) (amiga_wd_phase = (p))
#define amiga_wd_tick() (amiga_wd_beat++)

/* Start watching the calling task. Returns 0 if the process could not be
 * made - the game runs on without it. */
int  amiga_watchdog_start(void);
void amiga_watchdog_stop(void);

/* THE PROFILER: `pcprof 1` in opts.txt. Call before amiga_watchdog_start. The
 * same process then samples the main task's saved PC 50 times a second and
 * amiga_watchdog_stop writes pcprof.bin beside the game, big-endian longs: a
 * header (magic 'PCPF', the address of amiga_watchdog_start, the count), then
 * pc, the first return address on the stack, the phase, for each sample.
 * tools/bin/pcprof.py maps them to functions. A sampled PC tells the whole
 * frame's story without a clock in every section, and costs next to nothing.
 * 68k only: the saved frame's layout is exec's. */
void amiga_watchdog_set_profile(int on);

#endif /* AMIGA_WATCHDOG_H */
