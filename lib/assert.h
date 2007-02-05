#ifndef __KUDOS_LIB_ASSERT_H
#define __KUDOS_LIB_ASSERT_H

#include <linux/kernel.h> // printk()
#include <asm/bug.h>

#define assert(cond) \
	do { \
		if (unlikely(!(cond))) { \
			printk(KERN_EMERG \
		           "Assertion failure in %s() at %s:%d: \"%s\"\n", \
			       __FUNCTION__, __FILE__, __LINE__, # cond); \
			kfsd_global_lock.locked = 0; \
			BUG(); \
		} \
	} while (0)

// static_assert(x) will generate a compile-time error if 'x' is false.
#define static_assert(x) switch (x) case 0: case (x):

/* This must be after the definition of assert(). */
#include <kfs/kernel_serve.h>

#endif // !__KUDOS_LIB_ASSERT_H
