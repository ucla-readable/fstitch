#include <lib/types.h>
#include <lib/jiffies.h>

#include <linux/module.h>
int
jsleep(int32_t jiffies)
{
	current->state = TASK_INTERRUPTIBLE;
	return schedule_timeout(jiffies);
}
