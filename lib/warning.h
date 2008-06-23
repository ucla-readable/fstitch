/* This file is part of Featherstitch. Featherstitch is copyright 2005-2008 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef FSTITCH_LIB_WARNING_H
#define FSTITCH_LIB_WARNING_H

/* Printing warning messages every time something frequent happens can really
 * slow Featherstitch down. This library allows such messages to easily be
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

/* _warning(), used to implement the warning() macro, is so short that it should
 * be inlined to save the function call overhead on these (frequent) messages */
struct warning;
static inline void _warning(const char * message, struct warning * period, const char * function) __attribute__((always_inline));

/* There are slightly different implementations for the kernel and userspace... */

#ifdef __KERNEL__

#include <linux/sched.h>

struct warning {
	const int seconds;
	int32_t last;
};

#define WARNING_INITIALIZER(seconds) {seconds, 0}

static inline void _warning(const char * message, struct warning * period, const char * function)
{
	if(jiffies - period->last > HZ * period->seconds)
	{
		printf("%s(): %s\n", function, message);
		period->last = jiffies;
	}
}

#elif defined(UNIXUSER)

#include <sys/time.h>
#include <time.h>

struct warning {
	const int seconds;
	struct timeval last;
};

#define WARNING_INITIALIZER(seconds) {seconds, {0, 0}}

static inline void _warning(const char * message, struct warning * period, const char * function)
{
	struct timeval now;
	gettimeofday(&now, NULL);
	if(now.tv_sec - period->last.tv_sec > period->seconds && now.tv_usec >= period->last.tv_usec)
	{
		printf("%s(): %s\n", function, message);
		period->last = now;
	}
}

#endif

#endif /* !FSTITCH_LIB_WARNING_H */
