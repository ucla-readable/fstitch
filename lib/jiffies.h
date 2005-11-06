#ifndef KUDOS_LIB_JIFFIES_H
#define KUDOS_LIB_JIFFIES_H

#define HZ			100
#define JIFFIES_PER_SECOND	HZ

static __inline int jiffy_time(void) __attribute__((always_inline));

#if defined(KUDOS)
extern const struct Env* env; // prototype here because including lib.h breaks
#include <inc/env.h>
static __inline int jiffy_time(void)
{
	return env->env_jiffies;
}

#elif defined(UNIXUSER)
#include <sys/time.h>
#include <time.h>
#include <stdio.h>
#include <assert.h>
static __inline int jiffy_time(void)
{
	struct timeval tv;
	if (gettimeofday(&tv, NULL))
	{
		perror("gettimeofday");
		assert(0);
	}
	return tv.tv_sec * JIFFIES_PER_SECOND
	       + tv.tv_usec * (1000000 / JIFFIES_PER_SECOND);
}

#else
#error Unknown target system
#endif

#endif /* !KUDOS_LIB_JIFFIES_H */
