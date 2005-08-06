#include <inc/vector.h>
#include <inc/lib.h>
#include <inc/partition.h>

#include <kfs/ide_pio_bd.h>
#include <kfs/pc_ptable.h>
#include <kfs/wt_cache_bd.h>
#include <kfs/wb_cache_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/nbd_bd.h>
#include <kfs/journal_queue_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/journal_lfs.h>
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
CFS_t * construct_journaled_uhfs(BD_t * j_bd, BD_t * data_bd, LFS_t ** journal);
BD_t * construct_cacheing(BD_t * bd, size_t cache_nblks);

static const char * fspaths[] = {
#if !USE_THIRD_LEG
"/",
#endif
"/k0", "/k1", "/k2", "/k3"};

//#define USE_MIRROR
//#define USE_WB_CACHE
#ifndef USE_WB_CACHE
#define wb_cache_bd wt_cache_bd
#endif

int kfsd_init(void)
{
	const bool use_disk_0_extern_journal = 0;
	const bool use_disk_0 = 1;
	const bool use_disk_1 = 0;
	const bool use_net    = 0;
#ifdef __OPTIMIZE__
	/* Ensure the below tautology. When compiling without opts gcc will not
	 * allow the consts to propagate, so only check when compiling with opts.
	 */
	static_assert(!(use_disk_0_extern_journal && use_disk_0));
#endif
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

	if (use_disk_0_extern_journal)
	{
		const int jbd = 1; // 0 for nbd, 1 for ide(0, 1)

		BD_t * j_bd;
		BD_t * data_bd;
		LFS_t * journal;
		CFS_t * u;

		if (jbd == 0)
		{
			/* delay kfsd startup slightly for netd to start */
			sleep(200);

			if (! (j_bd = nbd_bd("192.168.2.1", 2492)) )
			{
				fprintf(STDERR_FILENO, "nbd_bd failed\n");
				kfsd_shutdown();
			}
		}
		else
		{
			if (! (j_bd = ide_pio_bd(0, 1, 0)) )
			{
				fprintf(STDERR_FILENO, "ide_pio_bd(0, 1) failed\n");
				kfsd_shutdown();
			}
			if (j_bd)
				OBJFLAGS(j_bd) |= OBJ_PERSISTENT;
		}

		if (! (j_bd = construct_cacheing(j_bd, 128)) )
			kfsd_shutdown();

		if (! (data_bd = ide_pio_bd(0, 0, 0)) )
		{
			fprintf(STDERR_FILENO, "ide_pio_bd(0, 0) failed\n");
			kfsd_shutdown();
		}
		if (data_bd)
			OBJFLAGS(data_bd) |= OBJ_PERSISTENT;
		if (! (data_bd = construct_cacheing(data_bd, 32)) )
			kfsd_shutdown();

		if (! (u = construct_journaled_uhfs(j_bd, data_bd, &journal)) )
			kfsd_shutdown();
		r = vector_push_back(uhfses, u);
		assert(r >= 0);

		//printf("Using josfs [journaled on disk 1, %u kB/s max avg] on disk 0.\n", journal_lfs_max_bandwidth(journal));
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
	const bool enable_internal_journaling = 0;
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
		bool journaling = 0;
		LFS_t * journal = NULL;
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

#if 0
		if (enable_internal_journaling)
		{
			BD_t * journal_queue;

			if (! (journal_queue = journal_queue_bd(cache)) )
				kfsd_shutdown();
			if ((lfs = josfs_lfs = josfs(journal_queue)))
			{
				if ((journal = journal_lfs(lfs, lfs, journal_queue)))
				{
					lfs = journal;
					journaling = 1;
				}
				else
				{
					(void) DESTROY(lfs);
					(void) DESTROY(journal_queue);
				}
			}
			else
				(void) DESTROY(journal_queue);			
		}
		else
#endif
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
		//if (journaling)
		//	printf(" [journaled, %u kB/s max avg]", journal_lfs_max_bandwidth(journal));

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

CFS_t * construct_journaled_uhfs(BD_t * j_bd, BD_t * data_bd, LFS_t ** journal)
{
	BD_t * journal_queue;
	LFS_t * j_lfs;
	LFS_t * data_lfs;
	CFS_t * u;

	if (! (j_lfs = wholedisk(j_bd)) )
		kfsd_shutdown();

	if (! (journal_queue = data_bd = journal_queue_bd(data_bd)) )
		kfsd_shutdown();
	if (! (data_lfs = josfs(data_bd)) )
		kfsd_shutdown();

//	if (! (data_lfs = journal_lfs(j_lfs, data_lfs, journal_queue)) )
//		kfsd_shutdown();

	if (! (u = uhfs(data_lfs)) )
		kfsd_shutdown();

	*journal = data_lfs;
	return u;
}
