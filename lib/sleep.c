#include <lib/platform.h>
#include <lib/jiffies.h>

#ifdef __KERNEL__

#include <linux/module.h>

int jsleep(int32_t jiffies)
{
	current->state = TASK_INTERRUPTIBLE;
	return schedule_timeout(jiffies);
}

#elif defined(UNIXUSER)

int jsleep(int32_t jiffies)
{
	// TODO: use nanosleep to avoid unix signal interactions
	return usleep(jiffies * (1000000 / HZ));
}

#endif
