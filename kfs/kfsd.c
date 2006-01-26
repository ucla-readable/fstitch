#include <inc/error.h>
#include <stdlib.h>
#include <string.h>
#include <lib/mmu.h>
#include <lib/stdio.h>

#if defined(KUDOS)
#include <kfs/sched.h>
#elif defined(UNIXUSER)
#include <kfs/fuse_serve.h>
#endif

#include <kfs/kfsd.h>
#include <kfs/kfsd_init.h>

struct module_shutdown {
	kfsd_shutdown_module shutdown;
	void * arg;
};

static struct module_shutdown module_shutdowns[10];

int kfsd_register_shutdown_module(kfsd_shutdown_module fn, void * arg)
{
	int i;

	for (i = 0; i < sizeof(module_shutdowns)/sizeof(module_shutdowns[0]); i++)
	{
		if (!module_shutdowns[i].shutdown)
		{
			module_shutdowns[i].shutdown = fn;
			module_shutdowns[i].arg = arg;
			return 0;
		}
	}

	return -E_NO_MEM;
}

// Shutdown kfsd: inform modules of impending shutdown, then exit.
void kfsd_shutdown(void)
{
	int i;
	printf("Syncing and shutting down KFSD.\n");

	for (i = 0; i < sizeof(module_shutdowns)/sizeof(module_shutdowns[0]); i++)
	{
		if (module_shutdowns[i].shutdown)
		{
			module_shutdowns[i].shutdown(module_shutdowns[i].arg);
			module_shutdowns[i].shutdown = NULL;
			module_shutdowns[i].arg = NULL;
		}
	}
	exit(0);
}

void kfsd_main(int argc, char ** argv)
{
	int r;

	memset(module_shutdowns, 0, sizeof(module_shutdowns));

	if ((r = kfsd_init(argc, argv)) < 0)
		exit(r);

#if defined(KUDOS)
	sched_loop();
#elif defined(UNIXUSER)
	fuse_serve_loop();
#else
#error Unknown target system
#endif
	kfsd_shutdown();
}

#if defined(KUDOS)
#include <inc/lib.h> // binaryname
void umain(int argc, char * argv[])
{
	int r, i;

	if(!argc)
	{
		binaryname = "kfsd";
		if((r = sys_env_set_name(0, "kfsd")) < 0)
		{
			printf("Failed to set env name: %e\n", r);
			return;
		}
	}
	
	if((r = sys_grant_io(0)) < 0)
	{
		printf("Failed to get I/O priveleges: %e\n", r);
		return;
	}
	/*if((r = sys_env_set_priority(0, ENV_MAX_PRIORITY)) < 0)
	{
		printf("Failed to set priority: %e\n", r);
		return;
	}*/

	// Allocate more pages for the stack because we sometimes need it (for chdesc graph traversal)
	for(i = 2; i != 33; i++)
	{
		r = sys_page_alloc(0, (void *) (USTACKTOP - i * PGSIZE), PTE_U | PTE_W | PTE_P);
		assert(r >= 0);
	}

	kfsd_main(argc, argv);
}

#elif defined(UNIXUSER)
int main(int argc, char * argv[])
{
	kfsd_main(argc, argv);
	return 0;
}

#else
#error Unknown target system
#endif
