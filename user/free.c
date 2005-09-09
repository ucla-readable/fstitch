#include <inc/lib.h>

struct mem_stats {
	size_t used;
	size_t used_kernboot;
	size_t free;
	size_t present;
};


static void
print_usage(const char *bin)
{
	printf("Usage: %s [-bkm]\n", bin);
}

static void
detect_mem_stats(struct mem_stats *ms)
{
	struct Page *p;
	for(p = (struct Page*)UPAGES; get_pte(p) & PTE_P; p++)
	{
		if(p->pp_ref > 0)
		{
			ms->used += PGSIZE;
			if(p->pp_ref == KERNBOOT_PPREF)
				ms->used_kernboot += PGSIZE;
		}
	}

	ms->present = PGSIZE * (p - (struct Page*)UPAGES);
	ms->free = ms->present - ms->used;
}

void
umain(int argc, char **argv)
{
	if(argc > 2 || get_arg_idx(argc, (const char**) argv, "-h"))
	{
		print_usage(argv[0]);
		exit();
	}

	struct mem_stats ms;
	memset(&ms, 0, sizeof(ms));
	detect_mem_stats(&ms);

	char unit_name;
	int  unit;

	if(argc == 1) {
		unit_name = 'K';
		unit      = 1024;
	} else if(get_arg_idx(argc,(const char**)  argv, "-b")) {
		unit_name = 'B';
		unit      = 1;
	} else if(get_arg_idx(argc, (const char**) argv, "-k")) {
		unit_name = 'K';
		unit      = 1024;
	} else if(get_arg_idx(argc, (const char**) argv, "-m")) {
		unit_name = 'M';
		unit      = 1024*1024;
	} else {
		print_usage(argv[0]);
		exit();
		// appease compiler warnings
		unit_name = '?';
		unit = -1;
	}

	ms.used          = ROUNDUP32(ms.used, unit) / unit;
	ms.used_kernboot = ROUNDUP32(ms.used_kernboot, unit) / unit;
	ms.present       = ROUNDUP32(ms.present, unit) / unit;
	ms.free          = ROUNDUP32(ms.free, unit) / unit;

	printf("Total: %d%c, Used: %d%c (Kernel boot: %d%c), Free: %d%c\n",
			 ms.present, unit_name,
			 ms.used, unit_name, ms.used_kernboot, unit_name,
			 ms.free, unit_name);
}
