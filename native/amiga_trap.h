/* amiga_trap.h - see amiga_trap.c. */
#ifndef AMIGA_TRAP_H
#define AMIGA_TRAP_H

#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Install the handler on the calling task (once) and setjmp. Evaluates to 0
 * on the arming call and non-zero when a CPU exception has been caught and
 * control has come back here. Use exactly like setjmp() - it is one, so the
 * frame that arms must still be live when the trap happens. */
void amiga_trap_install(void);
extern jmp_buf amiga_trap_jb;
#define amiga_trap_arm() (amiga_trap_install(), setjmp(amiga_trap_jb))

/* Put the task's original handler back. */
void amiga_trap_disarm(void);

/* Multi-line, human-readable account of the last trap. */
void amiga_trap_describe(char *buf, int len);

extern volatile unsigned long amiga_trap_number;
extern volatile unsigned long amiga_trap_pc;
extern volatile unsigned long amiga_trap_sr;
extern volatile unsigned long amiga_trap_fmt;
extern volatile unsigned long amiga_trap_regs[16];

#ifdef __cplusplus
}
#endif

#endif
