/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_LIB_ASSERT_H
#define __FSTITCH_LIB_ASSERT_H

#include <linux/kernel.h>

#ifndef NDEBUG
#define assert(cond) \
	do { \
		if (unlikely(!(cond))) { \
			printk(KERN_EMERG \
		           "Assertion failure in %s() at %s:%d: \"%s\"\n", \
			       __FUNCTION__, __FILE__, __LINE__, # cond); \
			assert_fail(); \
		} \
	} while (0)
#else
#define assert(cond) do { } while(0)
#endif

#define kpanic(info...) \
	do { \
		printk(KERN_EMERG \
		   "Featherstitch panic in %s() at %s:%d: ", \
		       __FUNCTION__, __FILE__, __LINE__); \
		printk(info); printk("\n"); \
		assert_fail(); \
	} while (0)

extern int assert_failed;

void assert_fail(void) __attribute__((__noreturn__));

#endif // !__FSTITCH_LIB_ASSERT_H
