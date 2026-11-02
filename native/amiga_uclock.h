#ifndef AMIGA_UCLOCK_H
#define AMIGA_UCLOCK_H
#ifdef __cplusplus
extern "C" {
#endif
/* microseconds since first call (timer.device E-clock); 0 if unavailable */
unsigned long amiga_uclock_us(void);
unsigned long amiga_uclock_freq(void);
/* The E-clock's own low word, no arithmetic at all - for a timer that is
 * read dozens of times a tick (drive_one's sections). Convert the SUM of
 * differences with amiga_uclock_freq() at print time, never per read. */
unsigned long amiga_uclock_raw(void);
/* Wait `us` microseconds in the OS (timer.device UNIT_MICROHZ), not on the
 * CPU; and give the timer back at exit. */
void amiga_uclock_sleep_us(unsigned long us);
void amiga_uclock_close(void);
#ifdef __cplusplus
}
#endif
#endif
