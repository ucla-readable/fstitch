#include <inc/lib.h>

int
sleep(int32_t centisecs)
{
	if (centisecs < 0)
		return -E_INVAL;

	const int32_t wakeup = centisecs + env->env_jiffies;

	for (;;)
	{
		if (wakeup - env->env_jiffies <= 0)
			return 0;
		sys_yield();
	}

	return -E_UNSPECIFIED;
}
