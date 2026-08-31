#ifndef AMIGA_UCLOCK_H
#define AMIGA_UCLOCK_H
#ifdef __cplusplus
extern "C" {
#endif
/* microseconds since first call (timer.device E-clock); 0 if unavailable */
unsigned long amiga_uclock_us(void);
unsigned long amiga_uclock_freq(void);

#ifdef __MORPHOS__
/* Give the CPU back for approximately us microseconds.
 *
 * MorphOS ONLY, and deliberately so. The 68k build paces its frames with a
 * busy-wait on amiga_uclock_us(), which is the right answer on a machine that
 * has nothing else to run and where handing the CPU to the scheduler costs
 * more than the wait: a 68020 rendering GTA has no spare capacity to donate.
 *
 * A MorphOS machine is the opposite case. It renders a frame in a fraction of
 * the budget and then has ~15 ms of nothing to do sixty times a second, and a
 * busy-wait spends all of it pinning one core at 100% - which makes Ambient
 * crawl and makes the game look like it has hung even while it is running
 * perfectly. This waits on timer.device instead, so the time goes back to the
 * system.
 *
 * Sub-tick accurate: timer.device UNIT_MICROHZ, not Delay(), whose 1/50 s
 * granularity cannot express a 60 Hz frame at all. Returns immediately if the
 * device could not be opened, which leaves the caller uncapped rather than
 * stuck. */
void amiga_uclock_sleep_us(unsigned long us);
#endif
#ifdef __cplusplus
}
#endif
#endif
