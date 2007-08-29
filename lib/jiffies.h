/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef FSTITCH_LIB_JIFFIES_H
#define FSTITCH_LIB_JIFFIES_H

static __inline int jiffy_time(void) __attribute__((always_inline));

#ifdef __KERNEL__

#include <linux/jiffies.h>

static __inline int jiffy_time(void)
{
	return (int) get_jiffies_64();
}

#elif defined(UNIXUSER)

/* Jiffies are used mostly for timers, so we'll save
 * CPU by making the granularity 1/50 of a second. */
#define HZ 50

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
	return tv.tv_sec * HZ + tv.tv_usec * HZ / 1000000;
}

#endif

#endif /* !FSTITCH_LIB_JIFFIES_H */
