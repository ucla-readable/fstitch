#include <inc/vector.h>
#include <inc/lib.h>
#include <inc/partition.h>

#include <kfs/ide_pio_bd.h>
#include <kfs/pc_ptable_bd.h>
#include <kfs/chdesc_stripper_bd.h>
#include <kfs/wt_cache_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/nbd_bd.h>
#include <kfs/journal_queue_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/journal_lfs.h>
#include <kfs/uhfs.h>
#include <kfs/josfs_cfs.h>
#include <kfs/table_classifier_cfs.h>
#include <kfs/fidprotector_cfs.h>
#include <kfs/fidfairy_cfs.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/depman.h>
#include <kfs/sched.h>
#include <kfs/kfsd.h>
#include <kfs/kfsd_init.h>

int construct_uhfses(BD_t * bd, uint32_t cache_nblks, vector_t * uhfses);

static const char * fspaths[] = {"/", "/k0", "/k1", "/k2", "/k3"};

// Init kfsd modules.
int kfsd_init(void)
{
	const bool use_disk_0 = 1;
	const bool use_disk_1 = 0;
	const bool use_net    = 0;

	vector_t * uhfses = NULL;
	CFS_t * table_class = NULL;
	CFS_t * fidprotector = NULL;
	CFS_t * fidfairy = NULL;
	int r;

	if((r = depman_init()) < 0)
	{
		fprintf(STDERR_FILENO, "depman_init: %e\n", r);
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

		if (! (bd = nbd_bd("192.168.2.1", 2492)) )
		{
			fprintf(STDERR_FILENO, "nbd_bd failed\n");
			kfsd_shutdown();
		}

		if ((r = construct_uhfses(bd, 400, uhfses)) < 0)
			kfsd_shutdown();
	}

	if (use_disk_0)
	{
		BD_t * bd;

		if (! (bd = ide_pio_bd(0)) )
		{
			fprintf(STDERR_FILENO, "ide_pio_bd(0) failed\n");
			kfsd_shutdown();
		}

		if ((r = construct_uhfses(bd, 32, uhfses)) < 0)
			kfsd_shutdown();
	}

	if (use_disk_1)
	{
		BD_t * bd;

		if (! (bd = ide_pio_bd(1)) )
		{
			fprintf(STDERR_FILENO, "ide_pio_bd(1) failed\n");
			kfsd_shutdown();
		}

		if ((r = construct_uhfses(bd, 32, uhfses)) < 0)
			kfsd_shutdown();
	}

	//
	// Mount uhfses

	if (! (table_class = table_classifier_cfs(NULL, NULL, 0)) )
		kfsd_shutdown();
	assert(!get_frontend_cfs());
	set_frontend_cfs(table_class);

	{
		const size_t uhfses_size = vector_size(uhfses);
		size_t i;
		for (i=0; i < uhfses_size; i++)
		{
			r = table_classifier_cfs_add(table_class, fspaths[i], vector_elt(uhfses, i));
			if (r < 0)
				kfsd_shutdown();
		}

		vector_destroy(uhfses);
		uhfses = NULL;
	}

	//
	// fidfairies

	if (! (fidprotector = fidprotector_cfs(get_frontend_cfs())) )
		kfsd_shutdown();
	// Uncomment when cfs_ipc_client supports capabilities:
	//set_frontend_cfs(fidprotector);

	if (! (fidfairy = fidfairy_cfs(get_frontend_cfs())) )
		kfsd_shutdown();
	set_frontend_cfs(fidfairy);

	return 0;
}


// Bring up the filesystems for bd and add them to uhfses.
int construct_uhfses(BD_t * bd, uint32_t cache_nblks, vector_t * uhfses)
{
	const bool enable_journaling = 0;
	void * ptbl = NULL;
	BD_t * partitions[4] = {NULL};
	uint32_t i;

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
				partitions[i-1] = pc_ptable_bd(ptbl, i);
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
	if (!partitions[0])
		partitions[0] = bd;

	/* setup each partition's cache, basefs, and uhfs */
	for (i=0; i < 4; i++)
	{
		BD_t * cache;
		BD_t * resizer;
		LFS_t * lfs;
		bool journaling = 0;
		CFS_t * u;
			
		if (!partitions[i])
			continue;

		if (4096 != CALL(partitions[i], get_atomicsize))
		{
			/* create a cache below the resizer */
			if (! (cache = wt_cache_bd(partitions[i], cache_nblks)) )
				kfsd_shutdown();

			/* create a resizer */
			if (! (resizer = block_resizer_bd(cache, 4096)) )
				kfsd_shutdown();

			/* create a cache above the resizer */
			if (! (cache = wt_cache_bd(resizer, 4)) )
				kfsd_shutdown();
		}
		else
		{
			if (! (cache = wt_cache_bd(partitions[i], cache_nblks)) )
				kfsd_shutdown();
		}

		if (! (cache = chdesc_stripper_bd(cache)) )
			kfsd_shutdown();

		if (enable_journaling)
		{
			BD_t * journal_queue;
			LFS_t * journal;

			if (! (journal_queue = journal_queue_bd(cache)) )
				kfsd_shutdown();
			if ((lfs = josfs(journal_queue)))
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
			lfs = josfs(cache);

		if (lfs)
			printf("Using josfs");
		else if ((lfs = wholedisk(cache)))
			printf("Using wholedisk");
		else
			kfsd_shutdown();

		if (journaling)
			printf(" [journaled]");

		if (i == 0 && partitions[0] == bd)
			printf(" on disk.\n");
		else
			printf(" on partition %d.\n", i);

		if (! (u = uhfs(lfs)) )
			kfsd_shutdown();
		if (vector_push_back(uhfses, u) < 0)
			kfsd_shutdown();
	}

	return 0;
}
