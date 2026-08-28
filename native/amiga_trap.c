/*
 * amiga_trap.c - turn a Guru into a log line.
 *
 * AmigaOS lets a task install its own CPU-exception handler
 * (Task->tc_TrapCode). Ours runs in supervisor mode, records the trap
 * number and the faulting PC, rewrites the return PC in the exception frame
 * so that `rte` lands in amiga_trap_land() in user mode, which longjmps back
 * to whoever called amiga_trap_arm(). That caller then has the address of
 * the crash to log, plus the opcode words under it, and can exit cleanly.
 *
 * Why: a Software Failure requester says "#8000000B" and nothing else. With
 * the binary unstripped, a PC maps to a function with `nm` in one grep, and
 * the opcode words tell FPU instruction from bad pointer at a glance.
 *
 * Only ever installed on the task that calls amiga_trap_arm(); nothing here
 * touches the system's own handler for other tasks.
 *
 * Exception frame as seen by tc_TrapCode (68020+, and 68000):
 *     sp+0   long   trap number (pushed by exec)
 *     sp+4   word   SR
 *     sp+6   long   PC
 *     sp+10  word   format/vector (68010+ only)
 * We only read PC and overwrite it in place, then drop the trap number and
 * rte - the CPU pops whatever frame format it pushed.
 */
#include <exec/tasks.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "amiga_trap.h"

/* longwords of user stack printed by amiga_trap_describe (128 = 512 bytes) */
#define AMIGA_TRAP_STACK_LONGS 128

extern struct ExecBase *SysBase;

/* Filled by the supervisor-mode handler, read after the longjmp. */
volatile unsigned long amiga_trap_number = 0;
volatile unsigned long amiga_trap_pc     = 0;
volatile unsigned long amiga_trap_sr     = 0;
volatile unsigned long amiga_trap_fmt    = 0;   /* 68010+ format/vector word */
volatile unsigned long amiga_trap_regs[16];   /* d0-d7, a0-a7 at the fault */
volatile unsigned long amiga_trap_frame[12];  /* raw supervisor stack at handler entry */

jmp_buf amiga_trap_jb;
static APTR    s_old_trapcode = 0;
static struct Task *s_task = 0;
static int     s_armed = 0;

void amiga_trap_land(void);

/*
 * Supervisor-mode entry. Motorola syntax so it reads like the CPU manuals;
 * gas accepts it. Register-neutral except for the frame surgery.
 */
__asm__(
"\t.text\n"
"\t.align 2\n"
"\t.globl _amiga_trap_handler\n"
"_amiga_trap_handler:\n"
"\tmove.l  d0,-(sp)\n"                 /* scratch                          */
/* Record only the FIRST trap: if a second one fires before the first has
 * been reported (our own rte faulting, say), the original must survive. */
"\ttst.l   _amiga_trap_number\n"
"\tbne     1f\n"
"\tmove.l  4(sp),d0\n"                 /* trap number                      */
"\tmove.l  d0,_amiga_trap_number\n"
"\tmove.l  10(sp),d0\n"                /* PC (frame is 4 bytes lower now)  */
"\tmove.l  d0,_amiga_trap_pc\n"
"\tclr.l   d0\n"
"\tmove.w  8(sp),d0\n"                 /* SR                               */
"\tmove.l  d0,_amiga_trap_sr\n"
"\tclr.l   d0\n"
"\tmove.w  14(sp),d0\n"                /* format/vector word               */
"\tmove.l  d0,_amiga_trap_fmt\n"
"\tmove.l  (sp)+,d0\n"                 /* restore scratch                  */
"\tmovem.l d0-d7/a0-a7,_amiga_trap_regs\n"
/* Raw copy of the supervisor stack as we found it (trap number, then the
 * CPU exception frame): the frame layout is the one thing this whole file
 * assumes, so keep the evidence. */
"\tmovem.l d0/a0/a1,-(sp)\n"
"\tlea     12(sp),a0\n"                /* = entry sp: trap number          */
"\tlea     _amiga_trap_frame,a1\n"
"\tmoveq   #11,d0\n"
"2:\tmove.l  (a0)+,(a1)+\n"
"\tdbf     d0,2b\n"
"\tmovem.l (sp)+,d0/a0/a1\n"
/* a7 above is the supervisor stack; what the crashed code was using is the
 * USP - record that instead, the stack dump in amiga_trap_describe needs it. */
"\tmove.l  a0,-(sp)\n"
"\tmove.l  usp,a0\n"
"\tmove.l  a0,_amiga_trap_regs+60\n"
"\tmove.l  (sp)+,a0\n"
"\tmove.l  d0,-(sp)\n"
"1:\tmove.l  (sp)+,d0\n"                /* (second trap joins here)         */
"\tmove.l  #_amiga_trap_land,6(sp)\n"  /* rewrite return PC                */
/* Land in user mode with interrupts on, whatever state the fault was in: a
 * fault inside Supervisor() or Disable() would otherwise leave the logger
 * running in supervisor mode, which ends in a double bus fault (HALT). */
"\tclr.w   4(sp)\n"                    /* SR := 0                          */
"\taddq.l  #4,sp\n"                    /* drop the trap number             */
"\trte\n"
);

extern void amiga_trap_handler(void);

/* User mode again, on the crashed stack: get off it immediately. Undo any
 * Forbid()/Disable() nesting the crashed code was holding first, or DOS
 * calls from the logger would deadlock. */
void amiga_trap_land(void)
{
	while (SysBase->TDNestCnt >= 0) Permit();
	while (SysBase->IDNestCnt >= 0) Enable();
	longjmp(amiga_trap_jb, 1);
}

/* setjmp itself lives in the caller's frame (amiga_trap_arm is a macro):
 * a jmp_buf taken inside a function that has since returned is unusable. */
void amiga_trap_install(void)
{
	if (!s_armed)
	{
		s_task = FindTask(NULL);
		s_old_trapcode = s_task->tc_TrapCode;
		s_task->tc_TrapCode = (APTR)amiga_trap_handler;
		s_armed = 1;
	}
}

void amiga_trap_disarm(void)
{
	if (s_armed && s_task)
	{
		s_task->tc_TrapCode = s_old_trapcode;
		s_armed = 0;
	}
}

static const char *trap_name(unsigned long n)
{
	switch (n)
	{
	case 2:  return "bus error";
	case 3:  return "address error";
	case 4:  return "illegal instruction";
	case 5:  return "divide by zero";
	case 6:  return "CHK";
	case 7:  return "TRAPV";
	case 8:  return "privilege violation";
	case 9:  return "trace";
	case 10: return "line-A emulator";
	case 11: return "line-F emulator (FPU instruction on a CPU without FPU, or a jump into data)";
	default: return "?";
	}
}

/*
 * Format the last trap into buf. Reads up to 8 opcode words at the PC when
 * the PC looks like it points at something readable (ROM or the first 256 MB
 * of address space; a wild PC could itself fault, and we are past caring
 * about elegance at that point but not about recursion).
 */
void amiga_trap_describe(char *buf, int len)
{
	unsigned long pc = amiga_trap_pc;
	unsigned long trapno = amiga_trap_number;
	int n = 0;
	amiga_trap_number = 0;   /* consumed: the next trap may be recorded again */
	n += snprintf(buf + n, len - n,
		"CPU TRAP %lu (%s) at PC 0x%08lx SR 0x%04lx frame 0x%04lx%s\n",
		trapno, trap_name(trapno), pc, amiga_trap_sr, amiga_trap_fmt,
		(amiga_trap_sr & 0x2000) ? " [SUPERVISOR]" : "");
	n += snprintf(buf + n, len - n,
		"  d0-d7: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
		amiga_trap_regs[0], amiga_trap_regs[1], amiga_trap_regs[2], amiga_trap_regs[3],
		amiga_trap_regs[4], amiga_trap_regs[5], amiga_trap_regs[6], amiga_trap_regs[7]);
	n += snprintf(buf + n, len - n,
		"  a0-a7: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n",
		amiga_trap_regs[8], amiga_trap_regs[9], amiga_trap_regs[10], amiga_trap_regs[11],
		amiga_trap_regs[12], amiga_trap_regs[13], amiga_trap_regs[14], amiga_trap_regs[15]);
	{
		int i;
		n += snprintf(buf + n, len - n, "  raw frame:");
		for (i = 0; i < 12; i++)
			n += snprintf(buf + n, len - n, " %08lx", amiga_trap_frame[i]);
		n += snprintf(buf + n, len - n, "\n");
	}
	/* Where our own code is: PC - textbase + (nm address of amiga_trap_land)
	 * gives the address to look up in the unstripped binary. */
	n += snprintf(buf + n, len - n, "  textbase: amiga_trap_land is at 0x%08lx (nm it to map the PC)\n",
		(unsigned long)(void *)amiga_trap_land);
	/* Chip RAM, Z2/Z3 fast RAM (this machine: 0x10000000-0x1FFFFFFF) and ROM
	 * are readable; anything else is left alone. */
	if ((pc & 1) == 0 && (pc < 0x20000000UL || (pc >= 0xF00000UL && pc < 0x1000000UL)))
	{
		const unsigned short *w = (const unsigned short *)pc;
		int i;
		n += snprintf(buf + n, len - n, "  opcode words:");
		for (i = 0; i < 8 && n < len - 8; i++)
			n += snprintf(buf + n, len - n, " %04x", (unsigned)w[i]);
		n += snprintf(buf + n, len - n, "\n");
	}
	if (pc >= 0xF80000UL && pc < 0x1000000UL)
		n += snprintf(buf + n, len - n, "  (PC is inside the Kickstart ROM)\n");
	/* User stack dump: the return addresses in it are the backtrace. Mapped to
	 * symbols on the host by winuae/harness/trapmap.py (needs the textbase
	 * line above and an nm listing of the unstripped binary). */
	{
		unsigned long usp = amiga_trap_regs[15];
		if ((usp & 1) == 0 && usp >= 0x1000UL && usp < 0x20000000UL)
		{
			const unsigned long *st = (const unsigned long *)usp;
			int i;
			for (i = 0; i < AMIGA_TRAP_STACK_LONGS && n < len - 12; i++)
			{
				if ((i & 7) == 0)
					n += snprintf(buf + n, len - n, "%s  usp+%03x:", i ? "\n" : "", i * 4);
				n += snprintf(buf + n, len - n, " %08lx", st[i]);
			}
			n += snprintf(buf + n, len - n, "\n");
		}
	}
}
