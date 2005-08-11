#include <inc/lib.h>

void
umain(int argc, char **argv)
{
	const uint32_t jiffies = env->env_jiffies;
	const uint32_t secs  = jiffies / HZ;
	const uint32_t mins  = secs / 60;
	const uint32_t hours = mins / 60;
	const uint32_t days  = hours / 24;
	printf("%d days %02d hours %02d mins %02d secs (%d jiffes)\n",
			 days,
			 hours % 24,
			 mins  % 60,
			 secs  % 60,
			 jiffies);
}
