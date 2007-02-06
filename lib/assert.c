#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/reboot.h>
#include <asm/bug.h>

#include <kfs/kernel_serve.h>
#include <lib/assert.h>

#define REBOOT 1

void assert_fail(void)
{
	dump_stack();
	kfsd_global_lock.locked = 0;
	
#if REBOOT
	printk(KERN_EMERG "Waiting 15 seconds before reboot...\n");
	current->state = TASK_INTERRUPTIBLE;
	schedule_timeout(HZ * 15);
	kernel_restart(NULL);
#endif
	
	BUG();
	/* WTF? */
	for(;;);
}
