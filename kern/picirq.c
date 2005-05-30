/* See COPYRIGHT for copyright information. */

#include <inc/assert.h>

#include <kern/picirq.h>


/* Keep copy of current IRQ mask */
uint16_t irq_mask_8259A = ~(1<<IRQ_SLAVE);
static bool didinit;

/* Initialize the 8259A interrupt controllers. */
void
pic_init(void)
{
	didinit = 1;

	/*
	 * ICW1:  0001g0hi
	 *    g:  0 = edge triggering, 1 = level triggering
	 *    h:  0 = cascaded PICs, 1 = master only
	 *    i:  0 = no ICW4, 1 = ICW4 required
	 */
	outb(IO_PIC1, 0x11);

	/*
	 * ICW2:  Vector offset
	 */
	outb(IO_PIC1+1, IRQ_OFFSET);

	/*
	 * ICW3:  bit mask of IR lines connected to slave PICs(master PIC),
	 *        3-bit No of IR line at which slave connects to master(slave PIC).
	 */
	outb(IO_PIC1+1, 1<<IRQ_SLAVE);

	/*
	 * ICW4:  000nbmap
	 *    n:  1 = special fully nested mode
	 *    b:  1 = buffered mode
	 *    m:  0 = slave PIC, 1 = maser PIC (ignored when b is 0, as the master/
	 *                                      slave role can be hardwired).
	 *    a:  1 = Automatic EOI mode
	 *    p:  0 = MCS-80/85 mode, 1 = intel x86 mode
	 */
	outb(IO_PIC1+1, 0x3);

	/*
	 * OCW1:  interrupt mask(start with all masked).
	 */
	outb(IO_PIC1+1, 0xff);

	/*
	 * OCW3:  0ef01prs
	 *   ef:  0x = NOP, 10 = clear specific mask, 11 = set specific mask
	 *    p:  0 = no polling, 1 = polling mode
	 *   rs:  0x = NOP, 10 = read IRR, 11 = read ISR
	 */
	outb(IO_PIC1, 0x68);             /* clear specific mask */
	outb(IO_PIC1, 0x0a);             /* read IRR by default */

	outb(IO_PIC2, 0x11);               /* ICW1 */
	outb(IO_PIC2+1, IRQ_OFFSET + 8);   /* ICW2 */
	outb(IO_PIC2+1, IRQ_SLAVE);        /* ICW3 */
	outb(IO_PIC2+1, 0x1);              /* ICW4 */

	outb(IO_PIC2+1, 0xff);             /* OCW1 */
	outb(IO_PIC2, 0x68);               /* OCW3 */
	outb(IO_PIC2, 0x0a);               /* OCW3 */

	if (irq_mask_8259A != 0xFFFF)
		irq_setmask_8259A(irq_mask_8259A);
}

void
irq_setmask_8259A_quiet(uint16_t mask)
{
	irq_mask_8259A = mask;
	if (!didinit)
		return;
	outb(IO_PIC1+1, (char)mask);
	outb(IO_PIC2+1, (char)(mask >> 8));
}

void
irq_setmask_8259A(uint16_t mask)
{
	int i;
	irq_setmask_8259A_quiet(mask);
	printf("enabled interrupts:");
	for (i = 0; i < 16; i++)
		if (~mask & (1<<i))
			printf(" %d", i);
	printf("\n");
}
