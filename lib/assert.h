#ifndef __KUDOS_LIB_ASSERT_H
#define __KUDOS_LIB_ASSERT_H

#if defined(KUDOS)
#include <inc/assert.h>
#elif defined(UNIXUSER)
#include <assert.h>
#elif defined(__KERNEL__)
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
/* This must be after the definition of assert(). */
#include <kfs/kernel_serve.h>
#else
#error Unknown target system
#endif

#endif // !__KUDOS_LIB_ASSERT_H
