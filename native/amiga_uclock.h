#ifndef AMIGA_UCLOCK_H
#define AMIGA_UCLOCK_H
#ifdef __cplusplus
extern "C" {
#endif
/* microseconds since first call (timer.device E-clock); 0 if unavailable */
unsigned long amiga_uclock_us(void);
unsigned long amiga_uclock_freq(void);
#ifdef __cplusplus
}
#endif
#endif
