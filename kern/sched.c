#include <inc/assert.h>
#include <inc/x86.h>
#include <inc/queue.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/syscall.h>

#include <kern/env.h>
#include <kern/irq.h>
#include <kern/pmap.h>
#include <kern/monitor.h>
#include <kern/kclock.h>

/* use the same element structure for both list and tail queue */
#define tqe_next le_next
#define tqe_prev le_prev
/* missing TAILQ_FIRST and TAILQ_NEXT */
#define TAILQ_FIRST(head) ((head)->tqh_first)
#define TAILQ_NEXT(elm, field) ((elm)->field.tqe_next)

static TAILQ_HEAD(Env_tailq, Env) run_queues[ENV_MAX_PRIORITY + 1];

void sched_init(void)
{
	int i;
	for(i = 0; i != ENV_MAX_PRIORITY + 1; i++)
		TAILQ_INIT(&run_queues[i]);
}

int sched_update(struct Env * e, int priority)
{
	if(e->env_status == ENV_FREE)
		return -E_INVAL;
	if(priority < 0 || priority > ENV_MAX_PRIORITY)
		return -E_INVAL;
	/* only allow priority 0 for envs[0] */
	if(!priority && e != envs)
		return -E_INVAL;
	if(e->env_link.tqe_next || e->env_link.tqe_prev)
	{
		/* not a newly allocated environment */
		if(e->env_epriority == priority)
			return 0;
		TAILQ_REMOVE(&run_queues[e->env_epriority], e, env_link);
	}
	e->env_epriority = priority;
	TAILQ_INSERT_TAIL(&run_queues[e->env_epriority], e, env_link);
	return 0;
}

void sched_remove(struct Env * e)
{
	if(e->env_status != ENV_FREE)
	{
		TAILQ_REMOVE(&run_queues[e->env_epriority], e, env_link);
		memset(&e->env_link, 0, sizeof(e->env_link));
	}
}

// Choose a user environment to run and run it.
void sched_yield(void)
{
	int priority = ENV_MAX_PRIORITY + 1;
	struct Env * e;
	int halted = 0;
	
	/* if there are pending IRQs to be delivered to user environments,
	 * env_dispatch_irqs() sets curenv to one of them and pushes a signal
	 * handler onto the target environment's stack */
	if(env_dispatch_irqs() > 0)
	{
		/* move the environment to the end of its queue */
		TAILQ_REMOVE(&run_queues[curenv->env_epriority], curenv, env_link);
		TAILQ_INSERT_TAIL(&run_queues[curenv->env_epriority], curenv, env_link);
		env_run(curenv);
	}
	
	/* The idle environment is the only environment allowed at priority 0 */
again:
	while(priority--)
	{
		for(e = TAILQ_FIRST(&run_queues[priority]); e; e = TAILQ_NEXT(e, env_link))
		{
			if(e->env_status != ENV_RUNNABLE)
			{
				if(!e->env_ipc_recving)
					continue;
				if(e->env_ipc_timeout - jiffies > 0)
					continue;
				e->env_tf.tf_eax = -E_TIMEOUT;
				e->env_status = ENV_RUNNABLE;
				e->env_ipc_recving = 0;
			}
			/* if the current env called sys_yield() and we are about to schedule it again, sleep */
			if(e == curenv && curenv->env_tf.tf_trapno == T_SYSCALL && curenv->env_tf.tf_eax == SYS_yield && !halted)
			{
				halted = 1;
				/* make sure interrupts are enabled */
				__asm__ __volatile__("sti");
				/* halt until an interrupt occurs */
				__asm__ __volatile__("hlt");
				/* and then restart */
				priority = ENV_MAX_PRIORITY + 1;
				goto again;
			}
			TAILQ_REMOVE(&run_queues[priority], e, env_link);
			TAILQ_INSERT_TAIL(&run_queues[priority], e, env_link);
			env_run(e);
		}
	}
	
	printf("Destroyed the only environment - nothing more to do!\n");
	for(;;)
		monitor(NULL);
}
