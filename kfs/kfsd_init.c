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
#include <kfs/fidcloser_cfs.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/depman.h>
#include <kfs/modman.h>
#include <kfs/sched.h>
#include <kfs/kfsd.h>
#include <kfs/kfsd_init.h>

int construct_uhfses(BD_t * bd, uint32_t cache_nblks, vector_t * uhfses);
CFS_t * construct_journaled_uhfs(BD_t * j_bd, BD_t * data_bd, LFS_t ** journal);
BD_t * construct_cacheing(BD_t * bd, size_t cache_nblks);

static const char * fspaths[] = {"/", "/k0", "/k1", "/k2", "/k3"};

// Init kfsd modules.
int kfsd_init(void)
{
	const bool use_disk_0_extern_journal = 0;
	const bool use_disk_0 = 1;
	const bool use_disk_1 = 0;
	const bool use_net    = 0;

	static_assert(!(use_disk_0_extern_journal && use_disk_0));

	vector_t * uhfses = NULL;
	CFS_t * table_class = NULL;
	CFS_t * fidprotector = NULL;
	CFS_t * fidcloser = NULL;
	int r;

	if((r = depman_init()) < 0)
	{
		fprintf(STDERR_FILENO, "depman_init: %e\n", r);
		kfsd_shutdown();
	}

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

	//
	// Setup uhfses

	if (! (uhfses = vector_create()) )
	{
		fprintf(STDERR_FILENO, "OOM, vector_create\n");
		kfsd_shutdown();
	}


	if (use_disk_0_extern_journal)
	{
		const int jbd = 1; // 0 for nbd, 1 for ide(1)

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
			if (! (j_bd = ide_pio_bd(0, 1)) )
			{
				fprintf(STDERR_FILENO, "ide_pio_bd(0) failed\n");
				kfsd_shutdown();
			}
		}

		if (! (j_bd = construct_cacheing(j_bd, 32)) )
			kfsd_shutdown();
		if (! (j_bd = chdesc_stripper_bd(j_bd)) )
			kfsd_shutdown();

		if (! (data_bd = ide_pio_bd(0, 0)) )
		{
			fprintf(STDERR_FILENO, "ide_pio_bd(0) failed\n");
			kfsd_shutdown();
		}
		if (! (data_bd = construct_cacheing(data_bd, 32)) )
			kfsd_shutdown();
		if (! (data_bd = chdesc_stripper_bd(data_bd)) )
			kfsd_shutdown();

		if (! (u = construct_journaled_uhfs(j_bd, data_bd, &journal)) )
			kfsd_shutdown();
		r = vector_push_back(uhfses, u);
		assert(r >= 0);

		printf("Using josfs [journaled on disk 1, %u kB/s max avg] on disk 0.\n", journal_lfs_max_bandwidth(journal));
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

		if (! (bd = ide_pio_bd(0, 0)) )
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

		if (! (bd = ide_pio_bd(0, 1)) )
		{
			fprintf(STDERR_FILENO, "ide_pio_bd(1) failed\n");
			kfsd_shutdown();
		}

		if ((r = construct_uhfses(bd, 32, uhfses)) < 0)		
			kfsd_shutdown();
	}

	//
	// Mount uhfses

	if (! (table_class = table_classifier_cfs()) )
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
	void * ptbl = NULL;
	BD_t * partitions[4] = {NULL};
	uint32_t i;
	int josfs_fsck = 1;

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
		LFS_t * journal = NULL;
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

		if (enable_internal_journaling)
		{
			BD_t * journal_queue;

			if (! (journal_queue = journal_queue_bd(cache)) )
				kfsd_shutdown();
			if ((lfs = josfs(journal_queue, &josfs_fsck)))
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
			lfs = josfs(cache, &josfs_fsck);

		if (lfs)
			printf("Using josfs");
		else if ((lfs = wholedisk(cache)))
			printf("Using wholedisk");
		else
			kfsd_shutdown();

		if (journaling)
			printf(" [journaled, %u kB/s max avg]", journal_lfs_max_bandwidth(journal));

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


BD_t * construct_cacheing(BD_t * bd, size_t cache_nblks)
{
	if (4096 != CALL(bd, get_blocksize))
	{
		/* create a cache below the resizer */
		if (! (bd = wt_cache_bd(bd, cache_nblks)) )
			kfsd_shutdown();

		/* create a resizer */
		if (! (bd = block_resizer_bd(bd, 4096)) )
			kfsd_shutdown();

		/* create a cache above the resizer */
		if (! (bd = wt_cache_bd(bd, 4)) )
			kfsd_shutdown();
	}
	else
	{
		if (! (bd = wt_cache_bd(bd, cache_nblks)) )
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
	int josfs_fsck = 1;

	if (! (j_lfs = wholedisk(j_bd)) )
		kfsd_shutdown();

	if (! (journal_queue = data_bd = journal_queue_bd(data_bd)) )
		kfsd_shutdown();
	if (! (data_lfs = josfs(data_bd, &josfs_fsck)) )
		kfsd_shutdown();

	if (! (data_lfs = journal_lfs(j_lfs, data_lfs, journal_queue)) )
		kfsd_shutdown();

	if (! (u = uhfs(data_lfs)) )
		kfsd_shutdown();

	*journal = data_lfs;
	return u;
}
