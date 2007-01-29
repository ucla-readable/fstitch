#ifndef KUDOS_LIB_JIFFIES_H
#define KUDOS_LIB_JIFFIES_H

#include <linux/jiffies.h>

#define JIFFIES_PER_SECOND	HZ

static __inline int jiffy_time(void) __attribute__((always_inline));

static __inline int jiffy_time(void)
{
	return (int) get_jiffies_64();
}

#endif /* !KUDOS_LIB_JIFFIES_H */
