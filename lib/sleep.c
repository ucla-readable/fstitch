#include <inc/lib.h>

int
sleep(int32_t jiffies)
{
	const int32_t wakeup = jiffies + env->env_jiffies;

	if (jiffies < 0)
		return -E_INVAL;

	for (;;)
	{
		if (wakeup - env->env_jiffies <= 0)
			return 0;
		sys_yield();
	}
}
