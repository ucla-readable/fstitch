#if defined(KUDOS)
#include <inc/lib.h>
#elif defined(UNIXUSER)
#include <unistd.h>
#endif

int
sleepj(int32_t jiffies)
{
#if defined(UNIXUSER)
	return usleep(jiffies * 10000);
#elif defined(KUDOS)
	const int32_t wakeup = jiffies + env->env_jiffies;

	if (jiffies < 0)
		return -E_INVAL;

	for (;;)
	{
		if (wakeup - env->env_jiffies <= 0)
			return 0;
		sys_yield();
	}
#endif
}
