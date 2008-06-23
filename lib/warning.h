/* This file is part of Featherstitch. Featherstitch is copyright 2005-2008 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef FSTITCH_LIB_WARNING_H
#define FSTITCH_LIB_WARNING_H

/* Printing warning messages every time something frequent happens can really
 * slow Featherstitch down. This "library" allows such messages to easily be
 * printed with a specified maximum frequency. Rather than code like this:
 * 
 * void foo(int x, int y)
 * {
 *     if(x < 10)
 *         printf("%s(): x < 10!!\n", __FUNCTION__);
 *     if(y > 20)
 *         printf("%s(): y > 20!!\n", __FUNCTION__);
 * }
 * 
 * You can instead write this, to make sure foo() will print a message at most
 * once every 2 seconds:
 * 
 * void foo(int x, int y)
 * {
 *     DEF_WARNING(foo_warn, 2);
 *     if(x < 10)
 *         warning("x < 10!!", foo_warn);
 *     if(y > 20)
 *         warning("y > 20!!", foo_warn);
 * }
 * 
 * The DEF_WARNING line sets up storage to keep track of when a message was last
 * displayed, and to remember the allowed frequency. */

#define DEF_WARNING(name, seconds) static struct warning name = WARNING_INITIALIZER(seconds)
#define warning(message, period) _warning(message, &(period), __FUNCTION__)

#ifdef __KERNEL__

#include <linux/sched.h>
#define LAST_TYPE int32_t
#define LAST_INITIALIZER 0

#elif defined(UNIXUSER)

#include <sys/time.h>
#include <time.h>
#define LAST_TYPE struct timeval
#define LAST_INITIALIZER {0, 0}

#endif

struct warning {
	int seconds, suppressed;
	LAST_TYPE last;
};
#define WARNING_INITIALIZER(seconds) {seconds, 0, LAST_INITIALIZER}

/* _warning(), used to implement the warning() macro, is short enough that it
 * should be inlined to save the function call overhead on these messages */
static inline void _warning(const char * message, struct warning * period, const char * function) __attribute__((always_inline));
static inline void _warning(const char * message, struct warning * period, const char * function)
{
#ifdef __KERNEL__
	int32_t now = jiffies;
	if(now - period->last > HZ * period->seconds)
#elif defined(UNIXUSER)
	struct timeval now;
	gettimeofday(&now, NULL);
	if(now.tv_sec - period->last.tv_sec > period->seconds && now.tv_usec >= period->last.tv_usec)
#endif
	{
		const char * format = period->suppressed ? "%s(): %s [suppressed %d]\n" : "%s(): %s\n";
		printf(format, function, message, period->suppressed);
		period->suppressed = 0;
		period->last = now;
	}
	else
		period->suppressed++;
}

#endif /* !FSTITCH_LIB_WARNING_H */
