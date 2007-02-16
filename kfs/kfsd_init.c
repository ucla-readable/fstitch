/* config.h gets us RELEASE_NAME */
#include <lib/config.h>
#include <lib/error.h>
#include <lib/assert.h>
#include <lib/kdprintf.h>
#include <lib/vector.h>
#include <lib/partition.h>
#include <lib/sleep.h>
#include <lib/jiffies.h>
#include <lib/disklabel.h>
#include <lib/stdio.h>

#include <kfs/pc_ptable.h>
#include <kfs/bsd_ptable.h>
#include <kfs/wt_cache_bd.h>
#include <kfs/wb_cache_bd.h>
#include <kfs/wb2_cache_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/mem_bd.h>
#include <kfs/loop_bd.h>

#include <kfs/ext2_base.h>
#include <kfs/journal_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/ufs_base.h>
#include <kfs/opgroup_lfs.h>
#include <kfs/uhfs.h>
#include <kfs/icase_cfs.h>
#include <kfs/revision.h>
#include <kfs/modman.h>
#include <kfs/sched.h>
#include <kfs/kfsd.h>
#include <kfs/debug.h>
#include <kfs/kfsd_init.h>

#include <kfs/linux_bd.h>
#include <kfs/kernel_serve.h>
#include <kfs/kernel_opgroup_ops.h>
#include <kfs/kernel_opgroup_scopes.h>

#define LINUX_BD_TIMING_TEST 0

int construct_uhfses(BD_t * bd, uint32_t cache_nblks, vector_t * uhfses);
BD_t * construct_cacheing(BD_t * bd, uint32_t cache_nblks, uint32_t bs);
int handle_bsd_partitions(void * bsdtbl, vector_t * partitions);

static const char * fspaths[] = {"/", "/k0", "/k1", "/k2", "/k3"};

struct kfsd_partition {
	BD_t * bd;
	uint16_t type;
	uint16_t subtype;
	char description[32];
};
typedef struct kfsd_partition kfsd_partition_t;

#define USE_ICASE 0
#define USE_JOURNAL 0

#define USE_WB_CACHE 2
#ifndef USE_WB_CACHE
#define wb2_cache_bd(bd, dblocks, blocks) wt_cache_bd(bd, dblocks)
#elif USE_WB_CACHE != 2
#define wb2_cache_bd(bd, dblocks, blocks) wb_cache_bd(bd, dblocks)
#endif

#if USE_WB_CACHE != 2 && USE_JOURNAL
#error The journal requires a wb2_cache to function
#endif

int kfsd_init(int nwbblocks)
{
	const bool use_disk_1 = 1;
	const bool use_disk_2 = 1;
	const bool use_mem_bd = 0;
	vector_t * uhfses = NULL;
	int r;

	printf("kfsd (%s) starting\n", RELEASE_NAME);

	/* we do kfsd_sched_init() before KFS_DEBUG_INIT() because the debugger
	 * registers a periodic callback... but aside from this exception, the
	 * debugger should be initialized first so we don't miss any interesting
	 * events by accident */
	if ((r = kfsd_sched_init()) < 0)
	{
		kdprintf(STDERR_FILENO, "sched_init: %i\n", r);
		return r;
	}

	if((r = KFS_DEBUG_INIT()) < 0)
	{
		kdprintf(STDERR_FILENO, "kfs_debug_init: %i\n", r);
		return r;
	}
	KFS_DEBUG_COMMAND(KFS_DEBUG_DISABLE, KDB_MODULE_BDESC);

	if((r = modman_init()) < 0)
	{
		kdprintf(STDERR_FILENO, "modman_init: %i\n", r);
		return r;
	}

	if ((r = kernel_serve_init()) < 0)
	{
		kdprintf(STDERR_FILENO, "kernel_serve_init: %d\n", r);
		return r;
	}
	if ((r = kernel_opgroup_ops_init()) < 0)
	{
		kdprintf(STDERR_FILENO, "kernel_opgroup_ops_init: %d\n", r);
		return r;
	}
	if ((r = kernel_opgroup_scopes_init()) < 0)
	{
		kdprintf(STDERR_FILENO, "kernel_opgroup_scopes_init: %d\n", r);
		return r;
	}

	if ((r = bdesc_autorelease_pool_push()) < 0)
	{
		kdprintf(STDERR_FILENO, "bdesc_autorelease_pool_push: %i\n");
		return r;
	}
	
	printf("kfsd basic initialization complete!\n");
	
	printf("kfsd: default write back cache size = %d\n", nwbblocks);

	//
	// Setup uhfses

	if (! (uhfses = vector_create()) )
	{
		kdprintf(STDERR_FILENO, "OOM, vector_create\n");
		return -E_NO_MEM;
	}

	if (use_disk_1)
	{
		BD_t * bd = NULL;
		extern char * linux_device;
		if (linux_device)
		{
			printf("Using device %s\n", linux_device);
			if (! (bd = linux_bd(linux_device)) )
				kdprintf(STDERR_FILENO, "linux_bd(\"%s\") failed\n", linux_device);
#if LINUX_BD_TIMING_TEST
			int jiffies = jiffy_time();
			uint32_t number;
			bdesc_autorelease_pool_push();
			printf("Timing test: writing 200000 sequential 8-sector blocks\n");
			for(number = 0; number < 200000; number++)
			{
				bdesc_t * block;
				chdesc_t * init = NULL;
				block = CALL(bd, synthetic_read_block, number * 8, 8);
				chdesc_create_init(block, bd, &init);
				CALL(bd, write_block, block);
				if(number % 10000 == 9999)
				{
					printf("Timing test: %d blocks written\n", number + 1);
					revision_tail_process_landing_requests();
					chdesc_reclaim_written();
					bdesc_autorelease_pool_pop();
					bdesc_autorelease_pool_push();
				}
			}
			chdesc_reclaim_written();
			bdesc_autorelease_pool_pop();
			jiffies = jiffy_time() - jiffies;
			printf("Timing test complete! Total time: %d.%02d seconds\n", jiffies / HZ, (jiffies % HZ) * 100 / HZ);
			DESTROY(bd);
			bd = NULL;
#endif
		}
		if (bd)
		{
			OBJFLAGS(bd) |= OBJ_PERSISTENT;
			if ((r = construct_uhfses(bd, nwbblocks, uhfses)) < 0)
				return r;
		}
	}
	if (use_disk_2)
	{
		BD_t * bd = NULL;
#if 0
		extern char * linux_device;
		if (linux_device)
		{
			printf("Using device %s\n", linux_device);
			if (! (bd = linux_bd(linux_device)) )
				kdprintf(STDERR_FILENO, "linux_bd(\"%s\") failed\n", linux_device);
		}
#endif
		if (bd)
		{
			OBJFLAGS(bd) |= OBJ_PERSISTENT;
			printf("Using disk 2\n");
			if ((r = construct_uhfses(bd, nwbblocks, uhfses)) < 0)
				return r;
		}
	}
	if (use_mem_bd)
	{
		BD_t * bd;

		if (! (bd = mem_bd(1024, 4096)) )
			kdprintf(STDERR_FILENO, "mem_bd(1024, 4096) failed\n");
		if (bd)
		{
			OBJFLAGS(bd) |= OBJ_PERSISTENT;
			if ((r = construct_uhfses(bd, nwbblocks, uhfses)) < 0)
				return r;
		}
	}

	//
	// Mount uhfses

	{
		const size_t uhfses_size = vector_size(uhfses);
		size_t i;
		for (i=0; i < uhfses_size; i++)
		{
			r = kfsd_add_mount(fspaths[i], vector_elt(uhfses, i));
			if (r < 0)
			{
				kdprintf(STDERR_FILENO, "kfsd_add_mount: %i\n", r);
				return r;
			}
		}

		vector_destroy(uhfses);
		uhfses = NULL;
	}

	r = kfsd_add_mount("/dev", modman_devfs);
	if (r < 0)
	{
		kdprintf(STDERR_FILENO, "kfsd_add_mount: %i\n", r);
		return r;
	}

	return 0;
}

static LFS_t * construct_lfs(kfsd_partition_t * part, uint32_t cache_nblks, LFS_t * (*fs)(BD_t *), const char * name, uint16_t blocksize)
{
	LFS_t * plain_lfs;
	LFS_t * lfs = NULL;
	
	bool is_journaled = 0;
	BD_t * journal;
	BD_t * cache = construct_cacheing(part->bd, cache_nblks, blocksize);
	if (!cache)
		return NULL;

#if USE_JOURNAL
	journal = journal_bd(cache);
	if (journal)
		is_journaled = 1;
	else
	{
		kdprintf(STDERR_FILENO, "journal_bd failed, not journaling\n");
		journal = cache;
	}
#else
	journal = cache;
#endif

	lfs = plain_lfs = fs(journal);

	if (is_journaled)
	{
		BD_t * journalbd = NULL;
		if (plain_lfs)
		{
			inode_t root_ino, journal_ino;
			int r;

			r = CALL(plain_lfs, get_root, &root_ino);
			if (r < 0)
			{
				kdprintf(STDERR_FILENO, "get_root: %i\n", r);
				return NULL;
			}
			r = CALL(plain_lfs, lookup_name, root_ino, ".journal", &journal_ino);
			if (r < 0)
			{
				kdprintf(STDERR_FILENO, "No journal file; restarting modules\n");
				goto disable_journal;
			}

			journalbd = loop_bd(plain_lfs, journal_ino);
			if (!journalbd)
			{
				kdprintf(STDERR_FILENO, "loop_bd failed\n");
				goto disable_journal;
			}
			r = journal_bd_set_journal(journal, journalbd);
			if (r < 0)
			{
				kdprintf(STDERR_FILENO, "journal_bd_set_journal: %i\n");
				goto disable_journal;
			}
		}
		else
		{
		  disable_journal:
			if (journalbd)
				(void) DESTROY(journalbd);
			if (plain_lfs)
				(void) DESTROY(plain_lfs);
			(void) DESTROY(journal);
			journal = cache;
			lfs = plain_lfs = fs(cache);
			is_journaled = 0;
		}
	}

	if (lfs)
		printf("Using %s on %s", name, part->description);
	else if ((lfs = wholedisk(cache)))
		printf("Using wholedisk on %s", part->description);
	else
	{
		kdprintf(STDERR_FILENO, "\nlfs creation failed\n");
		return NULL;
	}
	if (is_journaled)
		printf(" (journaled)");
	else
		printf(" (not journaled)");
	printf("\n");

	return lfs;
}
#define construct_lfs(part, cache_nblks, fs, blocksize) construct_lfs(part, cache_nblks, fs, #fs, blocksize)

// Bring up the filesystems for bd and add them to uhfses.
int construct_uhfses(BD_t * bd, uint32_t cache_nblks, vector_t * uhfses)
{
	void * ptbl = NULL;
	void * bsdtbl = NULL;
	vector_t * partitions = NULL;
	kfsd_partition_t * part = NULL;
	uint32_t i;

	if (! (partitions = vector_create()) )
	{
		kdprintf(STDERR_FILENO, "OOM, vector_create\n");
		return -E_NO_MEM;
	}

	/* discover pc partitions */
	ptbl = pc_ptable_init(bd);
	if (ptbl)
	{
		uint32_t max = pc_ptable_count(ptbl);
		printf("Found %d PC partitions.\n", max);
		for (i = 1; i <= max; i++)
		{
			uint8_t type = pc_ptable_type(ptbl, i);
			printf("Partition %d has type %02x\n", i, type);
			if (type == PTABLE_KUDOS_TYPE || type == PTABLE_LINUX_TYPE)
			{
				if (! (part = malloc(sizeof(kfsd_partition_t))) )
				{
					kdprintf(STDERR_FILENO, "OOM, malloc\n");
					return -E_NO_MEM;
				}
				part->bd = pc_ptable_bd(ptbl, i);
				if (part->bd)
				{
					OBJFLAGS(part->bd) |= OBJ_PERSISTENT;
					part->type = type;
					part->subtype = 0;
					snprintf(part->description, 32, "Partition %d", i);
					if (vector_push_back(partitions, part))
					{
						kdprintf(STDERR_FILENO, "OOM, vector_push_back\n");
						return -E_NO_MEM;
					}
				}
			}
			else if (type == PTABLE_FREEBSD_TYPE)
			{
				BD_t * tmppart;
				tmppart = pc_ptable_bd(ptbl, i);
				if (tmppart)
				{
					OBJFLAGS(tmppart) |= OBJ_PERSISTENT;
					bsdtbl = bsd_ptable_init(tmppart);
					if (bsdtbl)
					{
						int r = handle_bsd_partitions(bsdtbl, partitions);
						if (r < 0)
							return r;
					}
					bsd_ptable_free(bsdtbl);
				}
			}
			else
				printf("Unknown partition type %x\n", type);
		}
		pc_ptable_free(ptbl);

		if (vector_size(partitions) <= 0)
			printf("No partition found!\n");
	}
	else
	{
		printf("Using whole disk.\n");
		if (! (part = malloc(sizeof(kfsd_partition_t))) )
		{
			kdprintf(STDERR_FILENO, "OOM, malloc\n");
			return -E_NO_MEM;
		}
		// No partition table, make it look like a KudOS partition...
		part->bd = bd;
		part->type = PTABLE_KUDOS_TYPE;
		part->subtype = 0;
		snprintf(part->description, 32, "<entire disk>");
		if (vector_push_back(partitions, part))
		{
			kdprintf(STDERR_FILENO, "OOM, vector_push_back\n");
			return -E_NO_MEM;
		}
	}

	/* setup each partition's cache, basefs, and uhfs */
	for (i = 0; i < vector_size(partitions); i++)
	{
		LFS_t * lfs;
		CFS_t * u;
		
		part = vector_elt(partitions, i);
		if (!part)
			continue;

		if (part->type == PTABLE_KUDOS_TYPE)
		{
			lfs = construct_lfs(part, cache_nblks, josfs, 4096);
		}
		else if (part->type == PTABLE_FREEBSD_TYPE)
		{
			// TODO handle 1K fragment size in UFS?
			lfs = construct_lfs(part, cache_nblks, ufs, 2048);
		}
		else if (part->type == PTABLE_LINUX_TYPE)
		{
			// TODO handle differnt block sizes
			lfs = construct_lfs(part, cache_nblks, ext2, 4096);
		}
		else
		{
			printf("Unknown partition type %x\n", part->type);
			lfs = NULL;
		}
		if(!lfs)
			continue;

		if (! (lfs = opgroup_lfs(lfs)))
			return -E_UNSPECIFIED;
		if (! (u = uhfs(lfs)) )
		{
			kdprintf(STDERR_FILENO, "uhfs() failed\n");
			return -E_UNSPECIFIED;
		}
#if USE_ICASE
		if (! (u = icase_cfs(u)) )
		{
			kdprintf(STDERR_FILENO, "icase_cfs() failed\n");
			return -E_UNSPECIFIED;
		}
#endif
		if (vector_push_back(uhfses, u) < 0)
		{
			kdprintf(STDERR_FILENO, "vector_push_back() failed\n");
			return -E_UNSPECIFIED;
		}
	}

	for (i=0; i < vector_size(partitions); i++)
		free(vector_elt(partitions, i));

	vector_destroy(partitions);
	partitions = NULL;

	return 0;
}

BD_t * construct_cacheing(BD_t * bd, uint32_t cache_nblks, uint32_t bs)
{
	if (bs != CALL(bd, get_blocksize))
	{
		/* create a resizer */
		if (! (bd = block_resizer_bd(bd, bs)) )
			return NULL;

		if (! (bd = wt_cache_bd(bd, 16384)) )
			return NULL;

		/* create a cache above the resizer */
		if (! (bd = wb2_cache_bd(bd, cache_nblks, cache_nblks * 4)) )
			return NULL;
	}
	else
	{
		if (! (bd = wb2_cache_bd(bd, cache_nblks, cache_nblks * 4)) )
			return NULL;
	}

	return bd;
}

int handle_bsd_partitions(void * bsdtbl, vector_t * partitions)
{
	uint32_t j, bsd_max = bsd_ptable_count(bsdtbl);
	uint8_t fstype;
	kfsd_partition_t * part = NULL;

	for (j = 1; j <= bsd_max; j++)
	{
		fstype = bsd_ptable_type(bsdtbl, j);
		if (fstype != BSDLABEL_FS_UNUSED)
		{
			if (! (part = malloc(sizeof(kfsd_partition_t))) )
			{
				kdprintf(STDERR_FILENO, "OOM, malloc\n");
				return -E_NO_MEM;
			}
			part->bd = bsd_ptable_bd(bsdtbl, j);
			if (part->bd)
			{
				OBJFLAGS(part->bd) |= OBJ_PERSISTENT;
				part->type = PTABLE_FREEBSD_TYPE;
				part->subtype = fstype;
				snprintf(part->description, 32, "BSD Partition %d", j);
				if (vector_push_back(partitions, part))
				{
					kdprintf(STDERR_FILENO, "OOM, vector_push_back\n");
					return -E_NO_MEM;
				}
			}
		}
	}
	return 0;
}
