#include <kfs/ide_pio_bd.h>
#include <inc/partition.h>
#include <kfs/pc_ptable_bd.h>
#include <kfs/wt_cache_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/uhfs.h>
#include <kfs/josfs_cfs.h>
#include <kfs/table_classifier_cfs.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/kfsd.h>
#include <kfs/depman.h>
#include <inc/lib.h>

static const char * josfspath = "/";
static const char * fspaths[] = {"/k0", "/k1"};


struct module_shutdown {
	kfsd_shutdown_module shutdown;
	void * arg;
};

static struct module_shutdown module_shutdowns[10];


// Init kfsd modules.
int kfsd_init(int argc, char * argv[])
{
	BD_t * bd_disk;
	void * ptbl;
	BD_t * partitions[4] = {NULL};
	CFS_t * uhfses[2] = {NULL};
	CFS_t * josfscfs;
	CFS_t * frontend_cfs;
	uint32_t i;
	int r;

	memset(module_shutdowns, 0, sizeof(module_shutdowns));

	if (!cfs_ipc_serve())
		kfsd_shutdown();

	if (! (bd_disk = ide_pio_bd(1)) )
		kfsd_shutdown();


	/* discover partitions */
	ptbl = pc_ptable_init(bd_disk);
	if (ptbl)
	{
		uint32_t max = pc_ptable_count(ptbl);
		printf("Found %d partitions.\n", max);
		for (i = 1; i <= max; i++)
		{
			uint8_t type = pc_ptable_type(ptbl, i);
			printf("Partition %d has type %02x\n", i, type);
			if (type == PTABLE_KUDOS_TYPE)
				partitions[i-1] = pc_ptable_bd(ptbl, i);
		}
		pc_ptable_free(ptbl);

		if (!partitions[0] && !partitions[1] && !partitions[2] && !partitions[3])
		{
			printf("No KudOS partition found!\n");
			kfsd_shutdown();
		}
	}
	else
	{
		printf("Using whole disk.\n");
		partitions[0] = bd_disk;
	}

	/* setup each partition's cache, basefs, and uhfs */
	for (i=0; i < 2; i++)
	{
		BD_t * cache;
		BD_t * resizer;
		LFS_t * lfs;

		if (!partitions[i])
			continue;

		/* create a cache below the resizer */
		if (! (cache = wt_cache_bd(partitions[i], 32)) )
			kfsd_shutdown();

		/* create a resizer */
		if (! (resizer = block_resizer_bd(cache, 4096)) )
			kfsd_shutdown();

		/* create a cache above the resizer */
		if (! (cache = wt_cache_bd(resizer, 4)) )
			kfsd_shutdown();

		if (! (lfs = wholedisk(cache)) )
			kfsd_shutdown();

		if (! (uhfses[i] = uhfs(lfs)) )
			kfsd_shutdown();
	}

	if (! (josfscfs = josfs_cfs()) )
		kfsd_shutdown();


	/* setup frontend cfs */

	if (! (frontend_cfs = table_classifier_cfs(NULL, NULL, 0)) )
		kfsd_shutdown();

	if ((r = register_frontend_cfs(frontend_cfs)) < 0)
	{
		DESTROY(frontend_cfs);
		kfsd_shutdown();
	}

	for (i=0; i < 2; i++)
	{
		if (!uhfses[i])
			continue;

		r = table_classifier_cfs_add(frontend_cfs, fspaths[i], uhfses[i]);
		if (r < 0)
		{
			DESTROY(frontend_cfs);
			kfsd_shutdown();
		}
	}

	r = table_classifier_cfs_add(frontend_cfs, josfspath, josfscfs);
	if (r < 0)
	{
		DESTROY(frontend_cfs);
		kfsd_shutdown();
	}

	return 0;
}

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
	printf("kfsd shutting down.\n");
	asm("int3");

	for (i = 0; i < sizeof(module_shutdowns)/sizeof(module_shutdowns[0]); i++)
	{
		if (module_shutdowns[i].shutdown)
		{
			module_shutdowns[i].shutdown(module_shutdowns[i].arg);
			module_shutdowns[i].shutdown = NULL;
			module_shutdowns[i].arg = NULL;
		}
	}
	exit();
}

// This function will schedule and run requests.
// For now we use only write-through caches, and so simply call
// cfs_ipc_serve_run().
void kfsd_loop()
{
	for (;;)
		cfs_ipc_serve_run();
}

void bd_test(void);

void umain(int argc, char * argv[])
{
	int r;

	printf("Kfsd [0x%08x]\n", env->env_id);
	if(!argc)
	{
		binaryname = "kfsd";
		sys_env_set_name(0, "kfsd");
	}
	
	if(sys_grant_io(0))
	{
		printf("Failed to get I/O priveleges.\n");
		return;
	}
	if(depman_init())
	{
		printf("Failed to initialized DEP MAN!\n");
		return;
	}
	
	if ((r = kfsd_init(argc, argv)) < 0)
		exit();

	kfsd_loop();
	//bd_test();
}


//
// BD testing

#include <inc/partition.h>

#include <kfs/bdesc.h>
#include <kfs/ide_pio_bd.h>
#include <kfs/pc_ptable_bd.h>
#include <kfs/wt_cache_bd.h>

static uint32_t bdesc_sum(bdesc_t * bdesc)
{
	uint32_t sum = 0;
	uint32_t i;
	for(i = 0; i != bdesc->length; i++)
	{
		sum *= 3;
		sum += bdesc->ddesc->data[i];
	}
	return sum;
}

void bd_test(void)
{
	BD_t * bd;
	BD_t * cbd;
	BD_t * part = NULL;
	void * ptbl;
	uint32_t i;
	
	bd = ide_pio_bd(1);
	cbd = wt_cache_bd(bd, 4);
	ptbl = pc_ptable_init(cbd);
	if(ptbl)
	{
		uint32_t max = pc_ptable_count(ptbl);
		printf("Found %d partitions.\n", max);
		for(i = 1; i <= max; i++)
		{
			uint8_t type = pc_ptable_type(ptbl, i);
			printf("Partition %d has type %02x\n", i, type);
			if(type == PTABLE_KUDOS_TYPE && !part)
				part = pc_ptable_bd(ptbl, i);
		}
		pc_ptable_free(ptbl);
	}
	else
	{
		printf("Using whole disk.\n");
		part = cbd;
	}
	
	if(!part)
	{
		printf("No KudOS partition found!\n");
		exit();
	}
	
	printf("BD   block size is %d, block count is %d\n", CALL(bd, get_blocksize), CALL(bd, get_numblocks));
	printf("CBD  block size is %d, block count is %d\n", CALL(cbd, get_blocksize), CALL(cbd, get_numblocks));
	printf("PART block size is %d, block count is %d\n", CALL(part, get_blocksize), CALL(part, get_numblocks));
	
	for(i = 0; i != 10; i++)
	{
		bdesc_t * bdesc;
		uint8_t first;
		
		printf("\n=== Block %d sum\n", i);
		
		bdesc = CALL(bd, read_block, i);
		printf("    BD   0x%08x\n", bdesc_sum(bdesc));
		bdesc_drop(&bdesc);
		
		bdesc = CALL(cbd, read_block, i);
		printf("    CBD  0x%08x\n", bdesc_sum(bdesc));
		bdesc_drop(&bdesc);
		
		bdesc = CALL(part, read_block, i);
		printf("    PART 0x%08x\n", bdesc_sum(bdesc));
		bdesc_touch(bdesc);
		first = bdesc->ddesc->data[0];
		memmove(bdesc->ddesc->data, bdesc->ddesc->data + 1, CALL(part, get_blocksize) - 1);
		bdesc->ddesc->data[CALL(part, get_blocksize) - 1] = first;
		printf("    PART 0x%08x\n", bdesc_sum(bdesc));
		/* pass ownership on for even i, else keep it to force depman translation */
		if(i & 1)
			bdesc_retain(&bdesc);
		CALL(part, write_block, bdesc);
		if(i & 1)
			bdesc_release(&bdesc);
	}
	
	printf("\n");
	if(part != cbd)
		DESTROY(part);
	DESTROY(cbd);
	DESTROY(bd);
}
