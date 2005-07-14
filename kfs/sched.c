#include <inc/vector.h>
#include <inc/error.h>
#include <inc/malloc.h>

#include <kfs/ipc_serve.h>
#include <kfs/sched.h>
#include <kfs/bdesc.h>

struct fn_entry {
	sched_callback fn;
	void * arg;
	int32_t period;
	int32_t next;
};
typedef struct fn_entry fn_entry_t;


vector_t * fes = NULL;


int sched_register(const sched_callback fn, void * arg, int32_t freq_centisecs)
{
	int r;
	fn_entry_t * fe = malloc(sizeof(*fe));
	if (!fe)
		return -E_NO_MEM;

	// Note: no check [currently] to see if fn is already in fes

	fe->fn = fn;
	fe->arg = arg;
	fe->period = freq_centisecs;
	fe->next = env->env_jiffies + freq_centisecs;

	r = vector_push_back(fes, fe);
	if (r < 0)
	{
		free(fe);
		return r;
	}

	return 0;
}

int sched_unregister(const sched_callback fn)
{
	size_t fes_size = vector_size(fes);
	size_t i;
	fn_entry_t * fe;

	for (i=0; i < fes_size; i++)
	{
		fe = vector_elt(fes, i);
		if (fn == fe->fn)
		{
			free(fe);
			vector_erase(fes, i);
			return 0;
		}
	}

	return -E_NOT_FOUND;
}


int sched_init()
{
	// Check that sched_init is not called multiple times
	assert(!fes);

	fes = vector_create();
	if (!fes)
		return -E_NO_MEM;

	return 0;
}

void sched_loop()
{
	for (;;)
	{
		int32_t cur_ncs;
		size_t i, fes_size = vector_size(fes);
		fn_entry_t * fe;
		int r;

		// Run cvs_ipc_serve each loop (which will sleep for a bit)
		ipc_serve_run();

		// Run other fes scheduled to have run by now
		cur_ncs = env->env_jiffies;
		for (i=0; i < fes_size; i++)
		{
			fe = vector_elt(fes, i);
			if (fe->next - cur_ncs <= 0)
			{
				fe->fn(fe->arg);

				cur_ncs = env->env_jiffies;
				fe->next = cur_ncs + fe->period;
			}
		}

		// Run bdesc autoreleasing at the end of the main loop
		bdesc_autorelease_pool_pop();
		r = bdesc_autorelease_pool_push();
		assert(r >= 0);
	}
}
