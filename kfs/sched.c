#include <inc/error.h>
#include <lib/assert.h>
#include <lib/jiffies.h>
#include <lib/kdprintf.h>
#include <lib/stdlib.h>
#include <lib/vector.h>

#include <kfs/sched.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>
#include <kfs/debug.h>

#if defined(KUDOS)
#include <kfs/ipc_serve.h>
#endif

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
		return -E_NO_MEM;

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

	return -E_NOT_FOUND;
}


int kfsd_sched_init(void)
{
	// Check that sched_init is not called multiple times
	assert(!fes);

	fes = vector_create();
	if (!fes)
		return -E_NO_MEM;

	return 0;
}

#if defined(KUDOS)
static
#endif
void sched_iteration(void)
{
	int32_t cur_ncs;
	size_t i, fes_size;
	fn_entry_t * fe;
	int r;

	// Run other fes scheduled to have run by now
	cur_ncs = jiffy_time();
	fes_size = vector_size(fes);
	for (i=0; i < fes_size; i++)
	{
		fe = vector_elt(fes, i);
		if (fe->next - cur_ncs <= 0)
		{
			fe->fn(fe->arg);

			cur_ncs = jiffy_time();
			// Set up the next callback time based on when the timer
			// should have gone off, and not necessarily when it did
			fe->next += fe->period;
		}
	}

	// Run bdesc autoreleasing at the end of the main loop
	bdesc_autorelease_pool_pop();
	assert(!bdesc_autorelease_pool_depth());
	r = bdesc_autorelease_pool_push();
	assert(r >= 0);

	// Run chdesc reclamation at the end of the main loop
	chdesc_reclaim_written();

	// Also run debug command processing
	KFS_DEBUG_NET_COMMAND();
}

#if defined(KUDOS)
void sched_loop(void)
{
	for (;;)
	{
		ipc_serve_run(); // Run ipc_serve (which will sleep for a bit)
		sched_iteration();
	}
}
#endif
