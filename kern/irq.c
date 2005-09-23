#include <inc/error.h>

#include <kern/pmap.h>
#include <kern/picirq.h>
#include <kern/trap.h>
#include <kern/irq.h>
#include <kern/env.h>

static irq_handler_t irq_handlers[MAX_IRQS] = {NULL};
static int last_unexpected_irq = 0;
static uint16_t irq_mask_backup;

int request_irq(int irq, irq_handler_t handler)
{
	/* don't allow IRQ 0 */
	if(irq < 1 || MAX_IRQS <= irq)
		return -E_INVAL;
	if(irq_handlers[irq] && handler)
		return -E_BUSY;
	irq_handlers[irq] = handler;
	return 0;
}

void probe_irq_on(void)
{
	if(!last_unexpected_irq)
		irq_mask_backup = irq_mask_8259A;
	last_unexpected_irq = -1;
	irq_setmask_8259A_quiet(0);
}

int probe_irq_off(void)
{
	int irq = last_unexpected_irq;
	irq_setmask_8259A_quiet(irq_mask_backup);
	last_unexpected_irq = 0;
	return irq;
}

void dispatch_irq(int irq)
{
	if(irq_handlers[irq])
		irq_handlers[irq](irq);
	else if(!last_unexpected_irq)
		printf("spurious interrupt on IRQ %d\n", irq);
	else
		last_unexpected_irq = irq;
}

static envid_t irq_env[MAX_IRQS] = {0};
static int irq_count[MAX_IRQS] = {0};
static int env_irq_total = 0;

static void env_irq_handler(int irq)
{
	struct Env * e;
	
	if(envid2env(irq_env[irq], &e, 0) < 0)
	{
		/* env is gone, deregister it - this should probably never happen */
		irq_setmask_8259A(irq_mask_8259A | (1 << irq));
		request_irq(irq, NULL);
		irq_env[irq] = 0;
		env_irq_total -= irq_count[irq];
		irq_count[irq] = 0;
	}
	else
	{
		irq_count[irq]++;
		env_irq_total++;
	}
}

void env_dispatch_irqs(void)
{
	static int irq_index = -1;
	
	uint32_t old_fault_mode = page_fault_mode;
	uint32_t * ustack;
	struct Env * env;
	
	if(!env_irq_total)
		return;
	
	/* we have to be very careful that the check
	 * above keeps this loop from being infinite! */
	do
		if(++irq_index == MAX_IRQS)
			irq_index = 0;
	while(!irq_count[irq_index]);
	
	irq_count[irq_index]--;
	env_irq_total--;
	
	if(envid2env(irq_env[irq_index], &env, 0) < 0)
	{
		/* env is gone, deregister it - this should probably never happen */
		irq_setmask_8259A(irq_mask_8259A | (1 << irq_index));
		request_irq(irq_index, NULL);
		irq_env[irq_index] = 0;
		env_irq_total -= irq_count[irq_index];
		irq_count[irq_index] = 0;
		return;
	}
	
	/* can't deliver an IRQ without a handler */
	if(!env->env_irq_upcall)
		return;
	
	/* wouldn't want to wrap around 0 */
	if(env->env_tf.tf_esp < 6 * sizeof(uint32_t))
		env_destroy(env);
	
	/* switch curenv */
	if(curenv)
		curenv->env_tf = *UTF;
	*UTF = env->env_tf;
	curenv = env;
	lcr3(env->env_cr3);
	
	/* now push the handler onto the stack */
	/* FIXME: if there is not enough space on the user stack, this will
	 * kill the environment instead of invoking its page fault handler */
	
	page_fault_mode = PFM_KILL;
	ustack = (uint32_t *) TRUP(UTF->tf_esp);
	
	ustack[-1] = UTF->tf_eip;
	ustack[-2] = UTF->tf_eflags;
	ustack[-6] = irq_index;
	
	UTF->tf_esp = (register_t) &ustack[-6];
	UTF->tf_eip = curenv->env_irq_upcall;
	
	page_fault_mode = old_fault_mode;
}

int env_assign_irq(int irq, struct Env * env)
{
	int r;
	
	if(!env->env_irq_upcall)
		return -E_INVAL;
	if(irq_env[irq])
		return -E_BUSY;
	irq_env[irq] = env->env_id;
	
	if((r = request_irq(irq, env_irq_handler)) < 0)
	{
		irq_env[irq] = 0;
		return r;
	}
	irq_setmask_8259A(irq_mask_8259A & ~(1 << irq));
	
	return 0;
}

int env_unassign_irq(int irq, struct Env * env)
{
	if(irq_env[irq] != env->env_id)
		return -E_INVAL;
	irq_setmask_8259A(irq_mask_8259A | (1 << irq));
	request_irq(irq, NULL);
	env_irq_total -= irq_count[irq];
	irq_count[irq] = 0;
	irq_env[irq] = 0;
	return 0;
}

int env_irq_unassign_all(struct Env * env)
{
	int irq;
	for(irq = 0; irq != MAX_IRQS; irq++)
		if(irq_env[irq] == env->env_id)
			env_unassign_irq(irq, env);
	return 0;
}
