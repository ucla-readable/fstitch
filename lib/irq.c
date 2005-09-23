// User-level IRQ handler support.
// We use an assembly language wrapper around a C function.
// The assembly wrapper is in lib/irqentry.S.

#include <inc/lib.h>
#include <inc/irq.h>


// Assembly language IRQ entry point defined in lib/irqentry.S.
extern void _irq_upcall(void);

static irq_handler_t irq_handlers[MAX_IRQS] = {NULL};

void _irq_handler(int irq)
{
	if(irq_handlers[irq])
		irq_handlers[irq](irq);
}

/* just like the kernel version */
int request_irq(int irq, irq_handler_t handler)
{
	if(irq < 0 || MAX_IRQS <= irq)
		return -E_INVAL;
	if(irq_handlers[irq] && handler)
		return -E_BUSY;
	
	if(handler)
	{
		int r;
		if(!env->env_irq_upcall)
			sys_set_irq_upcall(0, _irq_upcall);
		irq_handlers[irq] = handler;
		r = sys_assign_irq(0, irq, 1);
		if(r < 0)
		{
			irq_handlers[irq] = NULL;
			return r;
		}
	}
	else
	{
		sys_assign_irq(0, irq, 0);
		irq_handlers[irq] = NULL;
	}
	
	return 0;
}
