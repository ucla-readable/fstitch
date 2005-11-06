#include <lib/types.h>
#include <lib/jiffies.h>

#if defined(KUDOS)
#include <inc/lib.h>
int
sleepj(int32_t jiffies)
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

#elif defined(UNIXUSER)
#include <unistd.h>
int
sleepj(int32_t jiffies)
{
	// TODO: use nanosleep to avoid unix signal interactions
	return usleep(jiffies * (1000000 / JIFFIES_PER_SECOND));
}

#else
#error Unknown target system
#endif

