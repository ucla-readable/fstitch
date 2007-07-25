#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/reboot.h>
#include <asm/bug.h>

#include <kfs/kernel_serve.h>
#include <lib/kernel-assert.h>

#define REBOOT 1

int assert_failed = 0;

void assert_fail(void)
{
	dump_stack();
	kfsd_global_lock.locked = 0;
	assert_failed = 1;
	
#if REBOOT
	printk(KERN_EMERG "Waiting 15 seconds before reboot...\n");
	current->state = TASK_INTERRUPTIBLE;
	schedule_timeout(HZ * 15);
	printk(KERN_EMERG "Time's up! Rebooting...\n");
	kernel_restart(NULL);
#endif
	
	BUG();
	/* WTF? */
	for(;;);
}
