#include <inc/vector.h>
#include <inc/lib.h>
#include <inc/partition.h>

#include <kfs/ide_pio_bd.h>
#include <kfs/pc_ptable.h>
#include <kfs/wt_cache_bd.h>
#include <kfs/wb_cache_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/nbd_bd.h>
#include <kfs/journal_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/uhfs.h>
#include <kfs/josfs_cfs.h>
#include <kfs/mirror_bd.h>
#include <kfs/table_classifier_cfs.h>
#include <kfs/fidprotector_cfs.h>
#include <kfs/fidcloser_cfs.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/modman.h>
#include <kfs/sched.h>
#include <kfs/kfsd.h>
#include <kfs/debug.h>
#include <kfs/kfsd_init.h>

#define USE_THIRD_LEG 0 // 1 -> mount josfs_cfs at '/'

int construct_uhfses(BD_t * bd, uint32_t cache_nblks, vector_t * uhfses);
BD_t * construct_cacheing(BD_t * bd, size_t cache_nblks);

static const char * fspaths[] = {
#if !USE_THIRD_LEG
"/",
#endif
"/k0", "/k1", "/k2", "/k3"};

//#define USE_MIRROR
#define USE_WB_CACHE
#ifndef USE_WB_CACHE
#define wb_cache_bd wt_cache_bd
#endif

int kfsd_init(void)
{
	const bool use_disk_0 = 1;
	const bool use_disk_1 = 0;
	const bool use_net    = 0;
	vector_t * uhfses = NULL;

	CFS_t * table_class = NULL;
	CFS_t * fidprotector = NULL;
	CFS_t * fidcloser = NULL;
	int r;

	if((r = KFS_DEBUG_INIT()) < 0)
	{
		fprintf(STDERR_FILENO, "kfs_debug_init: %e\n", r);
		kfsd_shutdown();
	}
	KFS_DEBUG_COMMAND(KFS_DEBUG_DISABLE, KDB_MODULE_BDESC);

	if((r = modman_init()) < 0)
	{
		fprintf(STDERR_FILENO, "modman_init: %e\n", r);
		kfsd_shutdown();
	}

	if (!cfs_ipc_serve_init())
	{
		fprintf(STDERR_FILENO, "cfs_ipc_serve_init failed\n");
		kfsd_shutdown();
	}

	if ((r = sched_init()) < 0)
	{
		fprintf(STDERR_FILENO, "sched_init: %e\n", r);
		kfsd_shutdown();
	}

	if ((r = bdesc_autorelease_pool_push()) < 0)
	{
		fprintf(STDERR_FILENO, "bdesc_autorelease_pool_push: %e\n");
		kfsd_shutdown();
	}
	
	//
	// Setup uhfses

	if (! (uhfses = vector_create()) )
	{
		fprintf(STDERR_FILENO, "OOM, vector_create\n");
		kfsd_shutdown();
	}

	if (use_net)
	{
		BD_t * bd;

		/* delay kfsd startup slightly for netd to start */
		sleep(200);

		if (! (bd = nbd_bd("192.168.0.2", 2492)) )
			fprintf(STDERR_FILENO, "nbd_bd failed\n");

		if (bd && (r = construct_uhfses(bd, 512, uhfses)) < 0)
			kfsd_shutdown();
	}

	if (use_disk_0)
	{
		BD_t * bd;

		if (! (bd = ide_pio_bd(0, 0, 0)) )
			fprintf(STDERR_FILENO, "ide_pio_bd(0, 0, 0) failed\n");
		if (bd)
			OBJFLAGS(bd) |= OBJ_PERSISTENT;

		if (bd && (r = construct_uhfses(bd, 128, uhfses)) < 0)
			kfsd_shutdown();
	}

	if (use_disk_1)
	{
		BD_t * bd;

		if (! (bd = ide_pio_bd(0, 1, 0)) )
			fprintf(STDERR_FILENO, "ide_pio_bd(0, 1, 0) failed\n");
		if (bd)
			OBJFLAGS(bd) |= OBJ_PERSISTENT;

		if (bd && (r = construct_uhfses(bd, 128, uhfses)) < 0)		
			kfsd_shutdown();
	}

	//
	// Mount uhfses

	if (! (table_class = table_classifier_cfs()) )
		kfsd_shutdown();
	assert(!get_frontend_cfs());
	set_frontend_cfs(table_class);
#if USE_THIRD_LEG
	CFS_t * josfscfs = josfs_cfs();
	r = table_classifier_cfs_add(table_class, "/", josfscfs);
	if (r < 0)
		kfsd_shutdown();
#endif
	{
		const size_t uhfses_size = vector_size(uhfses);
		size_t i;
		for (i=0; i < uhfses_size; i++)
		{
			r = table_classifier_cfs_add(table_class, fspaths[i], vector_elt(uhfses, i));
			if (r < 0)
			{
				fprintf(STDERR_FILENO, "table_classifier_cfs_add: %e\n", r);
				kfsd_shutdown();
			}
		}

		vector_destroy(uhfses);
		uhfses = NULL;
	}

	r = table_classifier_cfs_add(table_class, "/dev", modman_devfs);
	if (r < 0)
		kfsd_shutdown();

	//
	// fidfairies

	if (! (fidprotector = fidprotector_cfs(get_frontend_cfs())) )
		kfsd_shutdown();
	set_frontend_cfs(fidprotector);

	if (! (fidcloser = fidcloser_cfs(get_frontend_cfs())) )
		kfsd_shutdown();
	set_frontend_cfs(fidcloser);

	return 0;
}


// Bring up the filesystems for bd and add them to uhfses.
int construct_uhfses(BD_t * bd, uint32_t cache_nblks, vector_t * uhfses)
{
	const bool enable_fsck = 0;
	void * ptbl = NULL;
	BD_t * partitions[4] = {NULL};
	uint32_t i;

#ifdef USE_MIRROR
	bd = mirror_bd(bd, NULL, 4);
#endif

	/* discover partitions */
	ptbl = pc_ptable_init(bd);
	if (ptbl)
	{
		uint32_t max = pc_ptable_count(ptbl);
		printf("Found %d partitions.\n", max);
		for (i = 1; i <= max; i++)
		{
			uint8_t type = pc_ptable_type(ptbl, i);
			printf("Partition %d has type %02x\n", i, type);
			if (type == PTABLE_KUDOS_TYPE)
			{
				partitions[i-1] = pc_ptable_bd(ptbl, i);
				if (partitions[i-1])
					OBJFLAGS(partitions[i-1]) |= OBJ_PERSISTENT;
			}
		}
		pc_ptable_free(ptbl);
			
		if (!partitions[0] && !partitions[1] && !partitions[2] && !partitions[3])
		{
			printf("No KudOS partition found!\n");
			//kfsd_shutdown();
		}
	}
	else
	{
		printf("Using whole disk.\n");
		partitions[0] = bd;
	}

	// HACK
	if (!partitions[0] && !partitions[1] && !partitions[2] && !partitions[3])
		partitions[0] = bd;

	/* setup each partition's cache, basefs, and uhfs */
	for (i=0; i < 4; i++)
	{
		BD_t * cache;
		BD_t * resizer;
		LFS_t * josfs_lfs;
		LFS_t * lfs;
		CFS_t * u;
			
		if (!partitions[i])
			continue;

		if (4096 != CALL(partitions[i], get_atomicsize))
		{
			/* create a resizer */
			if (! (resizer = block_resizer_bd(partitions[i], 4096)) )
				kfsd_shutdown();

			/* create a cache above the resizer */
			if (! (cache = wb_cache_bd(resizer, cache_nblks)) )
				kfsd_shutdown();
		}
		else
		{
			if (! (cache = wb_cache_bd(partitions[i], cache_nblks)) )
				kfsd_shutdown();
		}

		lfs = josfs_lfs = josfs(cache);

		if (josfs_lfs && enable_fsck)
		{
			printf("Fscking... ");
			int r = josfs_fsck(josfs_lfs);
			if (r == 0)
				printf("done.\n");
			else if (r > 0)
				printf("found %d errors\n", r);
			else
				printf("critical error: %e\n", r);
		}

		if (lfs)
			printf("Using josfs");
		else if ((lfs = wholedisk(cache)))
			printf("Using wholedisk");
		else
		{
			fprintf(STDERR_FILENO, "\nlfs creation failed\n");
			kfsd_shutdown();
		}

		if (i == 0 && partitions[0] == bd)
			printf(" on disk.\n");
		else
			printf(" on partition %d.\n", i);

		if (! (u = uhfs(lfs)) )
		{
			fprintf(STDERR_FILENO, "uhfs() failed\n");
			kfsd_shutdown();
		}
		if (vector_push_back(uhfses, u) < 0)
		{
			fprintf(STDERR_FILENO, "vector_push_back() failed\n");
			kfsd_shutdown();
		}
	}

	return 0;
}

BD_t * construct_cacheing(BD_t * bd, size_t cache_nblks)
{
	if (4096 != CALL(bd, get_blocksize))
	{
		/* create a resizer */
		if (! (bd = block_resizer_bd(bd, 4096)) )
			kfsd_shutdown();

		/* create a cache above the resizer */
		if (! (bd = wt_cache_bd(bd, cache_nblks)) )
			kfsd_shutdown();
	}
	else
	{
		if (! (bd = wb_cache_bd(bd, cache_nblks)) )
			kfsd_shutdown();
	}

	return bd;
}
