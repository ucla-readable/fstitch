/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h>

#ifdef __KERNEL__

#include <linux/module.h>
#include <linux/sched.h>

int jsleep(int32_t jiffies)
{
	current->state = TASK_INTERRUPTIBLE;
	return schedule_timeout(jiffies);
}

#elif defined(UNIXUSER)

#include <unistd.h>

int jsleep(int32_t jiffies)
{
	// TODO: use nanosleep to avoid unix signal interactions
	return usleep(jiffies * (1000000 / HZ));
}

#endif
