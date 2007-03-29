#ifndef __KUDOS_LIB_ASSERT_H
#define __KUDOS_LIB_ASSERT_H

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
		   "Kudos panic in %s() at %s:%d: ", \
		       __FUNCTION__, __FILE__, __LINE__); \
		printk(info); printk("\n"); \
		assert_fail(); \
	} while (0)

void assert_fail(void) __attribute__((__noreturn__));

#endif // !__KUDOS_LIB_ASSERT_H