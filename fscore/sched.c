/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/vector.h>

#include <fscore/fstitchd.h>
#include <fscore/sched.h>
#include <fscore/bdesc.h>
#include <fscore/patch.h>
#include <fscore/debug.h>
#include <fscore/revision.h>

#define DEBUG_TIMING 0
#include <fscore/kernel_timing.h>
KERNEL_TIMING(timing);

struct fn_entry {
	sched_callback fn;
	void * arg;
	int32_t period;
	int32_t next;
};
typedef struct fn_entry fn_entry_t;


static vector_t * fes = NULL;

int sched_register(const sched_callback fn, void * arg, int32_t freq_jiffies)
{
	int r;
	fn_entry_t * fe = malloc(sizeof(*fe));
	if (!fe)
		return -ENOMEM;

	// Note: no check [currently] to see if fn is already in fes

	fe->fn = fn;
	fe->arg = arg;
	fe->period = freq_jiffies;
	fe->next = jiffy_time() + freq_jiffies;

	r = vector_push_back(fes, fe);
	if (r < 0)
	{
		free(fe);
		return r;
	}

	return 0;
}

int sched_unregister(const sched_callback fn, void * arg)
{
	size_t fes_size = vector_size(fes);
	size_t i;
	fn_entry_t * fe;

	for (i=0; i < fes_size; i++)
	{
		fe = vector_elt(fes, i);
		if (fn == fe->fn && arg == fe->arg)
		{
			free(fe);
			vector_erase(fes, i);
			return 0;
		}
	}

	return -ENOENT;
}

static void fstitchd_sched_shutdown(void * ignore)
{
	size_t i;

	for (i = 0; i < vector_size(fes); i++)
	{
		fn_entry_t * fe = vector_elt(fes, i);
		free(fe);
		vector_elt_set(fes, i, NULL);
	}

	vector_destroy(fes);
	fes = NULL;
	TIMING_DUMP(timing, "CALLBACK", "callbacks");
}

int fstitchd_sched_init(void)
{
	int r;

	// Check that sched_init is not called multiple times
	assert(!fes);

	fes = vector_create();
	if (!fes)
		return -ENOMEM;

	r = fstitchd_register_shutdown_module(fstitchd_sched_shutdown, NULL, SHUTDOWN_POSTMODULES);
	if (r < 0)
	{
		vector_destroy(fes);
		fes = NULL;
		return r;
	}

	return 0;
}

void sched_run_callbacks(void)
{
	int32_t cur_ncs;
	size_t i, fes_size;
	KERNEL_INTERVAL(interval);

	// Run other fes scheduled to have run by now
	cur_ncs = jiffy_time();
	fes_size = vector_size(fes);
	for (i=0; i < fes_size; i++)
	{
		fn_entry_t * fe = vector_elt(fes, i);
		if (fe->next - cur_ncs <= 0)
		{
			TIMING_START(interval);
			fe->fn(fe->arg);
			sched_run_cleanup();
			TIMING_STOP(interval, timing);

			cur_ncs = jiffy_time();
			// Set up the next callback time based on when the timer
			// should have gone off, and not necessarily when it did
			fe->next += fe->period;
		}
	}
}

void sched_run_cleanup(void)
{
	int r;

#ifdef __KERNEL__
	// In-flight blocks are only supported in the kernel
	revision_tail_process_landing_requests();
#endif

	// Run bdesc autoreleasing
	bdesc_autorelease_pool_pop();
	assert(!bdesc_autorelease_pool_depth());
	r = bdesc_autorelease_pool_push();
	assert(r >= 0);

	// Run patch reclamation
	patch_reclaim_written();
}
