/* See COPYRIGHT for copyright information. */

/* The Run Time Clock and other NVRAM access functions that go with it. */
/* The run time clock is hard-wired to IRQ8. */

#include <inc/x86.h>
#include <inc/stdio.h>
#include <inc/isareg.h>
#include <inc/timerreg.h>
#include <inc/env.h>

#include <kern/kclock.h>
#include <kern/picirq.h>

volatile int jiffies = 0;

unsigned
mc146818_read(void *sc, unsigned reg)
{
	outb(IO_RTC, reg);
	return inb(IO_RTC+1);
}

void
mc146818_write(void *sc, unsigned reg, unsigned datum)
{
	outb(IO_RTC, reg);
	outb(IO_RTC+1, datum);
}


void
kclock_init(void)
{
	/* initialize 8253 clock to interrupt HZ times/sec */
	outb(TIMER_MODE, TIMER_SEL0 | TIMER_RATEGEN | TIMER_16BIT);
	outb(IO_TIMER1, TIMER_DIV(HZ) % 256);
	outb(IO_TIMER1, TIMER_DIV(HZ) / 256);
	//printf("	Setup timer interrupts via 8259A\n");
	irq_setmask_8259A(irq_mask_8259A & ~(1<<0));
	//printf("	unmasked timer interrupt\n");
}

void
kclock_reinit(int hz)
{
	irq_setmask_8259A_quiet(irq_mask_8259A | (1<<0));
	/* initialize 8253 clock to interrupt hz times/sec */
	outb(TIMER_MODE, TIMER_SEL0 | TIMER_RATEGEN | TIMER_16BIT);
	outb(IO_TIMER1, TIMER_DIV(hz) % 256);
	outb(IO_TIMER1, TIMER_DIV(hz) / 256);
	irq_setmask_8259A_quiet(irq_mask_8259A & ~(1<<0));
}

void
kclock_delay(int length)
{
	int limit = jiffies + length;
	while(limit - jiffies > 0);
}
