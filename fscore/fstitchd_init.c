/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

/* config.h gets us RELEASE_NAME */
#include <lib/config.h>
#include <lib/platform.h>
#include <lib/vector.h>
#include <lib/partition.h>
#include <lib/sleep.h>
#include <lib/jiffies.h>
#include <lib/disklabel.h>

#include <fscore/revision.h>
#include <fscore/modman.h>
#include <fscore/sched.h>
#include <fscore/fstitchd.h>
#include <fscore/debug.h>
#include <fscore/pc_ptable.h>
#include <fscore/bsd_ptable.h>

#include <modules/wt_cache_bd.h>
#include <modules/wb_cache_bd.h>
#include <modules/wb2_cache_bd.h>
#include <modules/wbr_cache_bd.h>
#include <modules/block_resizer_bd.h>
#include <modules/crashsim_bd.h>
#include <modules/mem_bd.h>
#include <modules/loop_bd.h>
#include <modules/ext2_lfs.h>
#include <modules/journal_bd.h>
#include <modules/unlink_bd.h>
#include <modules/wholedisk_lfs.h>
#include <modules/josfs_lfs.h>
#include <modules/ufs_lfs.h>
#include <modules/patchgroup_lfs.h>
#include <modules/uhfs_cfs.h>
#include <modules/icase_cfs.h>

#include <fscore/fstitchd_init.h>

#ifdef __KERNEL__
#include <modules/linux_bd.h>
#include <fscore/kernel_serve.h>
#include <fscore/kernel_patchgroup_ops.h>
#include <fscore/kernel_patchgroup_scopes.h>
#elif defined(UNIXUSER)
#include <modules/unix_file_bd.h>
#include <fscore/fuse_serve.h>
#endif

#define LINUX_BD_TIMING_TEST 0

int construct_uhfses(BD_t * bd, uint32_t cache_nblks, vector_t * uhfses);
BD_t * construct_cacheing(BD_t * bd, uint32_t cache_nblks, uint32_t bs);
int handle_bsd_partitions(void * bsdtbl, vector_t * partitions);

static const char * fspaths[] = {"/", "/k0", "/k1", "/k2", "/k3"};

struct fstitchd_partition {
	BD_t * bd;
	uint16_t type;
	uint16_t subtype;
	char description[32];
};
typedef struct fstitchd_partition fstitchd_partition_t;

#define USE_ICASE 0

#define USE_WB_CACHE 2
#ifndef USE_WB_CACHE
#define wb2_cache_bd(bd, dblocks, blocks) wt_cache_bd(bd, dblocks)
#elif USE_WB_CACHE != 2
#define wb2_cache_bd(bd, dblocks, blocks) wb_cache_bd(bd, dblocks)
#endif

#define USE_WBR_CACHE 0
#if USE_WBR_CACHE
#undef wb2_cache_bd
#define wb2_cache_bd wbr_cache_bd
#endif

#if USE_WB_CACHE != 2 && ALLOW_JOURNAL
#error The journal requires a wb2_cache to function
#endif

int fstitchd_init(int nwbblocks)
{
	const bool use_disk_1 = 1;
	const bool use_disk_2 = 1;
	const bool use_mem_bd = 0;
	vector_t * uhfses = NULL;
	int r;

#if ALLOW_JOURNAL && ALLOW_UNLINK
	extern int use_journal, use_unlink;
	if(use_journal && use_unlink)
	{
		printf("%s(): use_journal and use_unlink are not compatible\n", __FUNCTION__);
		return -1;
	}
#endif

	printf("fstitchd (%s) starting\n", RELEASE_NAME);

	/* we do fstitchd_sched_init() before FSTITCH_DEBUG_INIT() because the debugger
	 * registers a periodic callback... but aside from this exception, the
	 * debugger should be initialized first so we don't miss any interesting
	 * events by accident */
	if ((r = fstitchd_sched_init()) < 0)
	{
		fprintf(stderr, "sched_init: %i\n", r);
		return r;
	}

	if((r = FSTITCH_DEBUG_INIT()) < 0)
	{
		fprintf(stderr, "fstitch_debug_init: %i\n", r);
		return r;
	}
	FSTITCH_DEBUG_COMMAND(FSTITCH_DEBUG_DISABLE, FDB_MODULE_BDESC);

	if ((r = hash_map_init()) < 0)
	{
		fprintf(stderr, "hash_map_init: %i\n", r);
		return r;
	}
	if ((r = bdesc_init()) < 0)
	{
		fprintf(stderr, "bdesc_init: %i\n", r);
		return r;
	}
	if ((r = patch_init()) < 0)
	{
		fprintf(stderr, "patch_init: %i\n", r);
		return r;
	}
	if ((r = revision_init()) < 0)
	{
		fprintf(stderr, "revision_init: %i\n", r);
		return r;
	}

	if((r = modman_init()) < 0)
	{
		fprintf(stderr, "modman_init: %i\n", r);
		return r;
	}

#ifdef __KERNEL__
	if ((r = kernel_serve_init()) < 0)
	{
		fprintf(stderr, "kernel_serve_init: %d\n", r);
		return r;
	}
	if ((r = kernel_patchgroup_ops_init()) < 0)
	{
		fprintf(stderr, "kernel_patchgroup_ops_init: %d\n", r);
		return r;
	}
	if ((r = kernel_patchgroup_scopes_init()) < 0)
	{
		fprintf(stderr, "kernel_patchgroup_scopes_init: %d\n", r);
		return r;
	}
#elif defined(UNIXUSER)
	extern int fstitchd_argc;
	extern char ** fstitchd_argv;
	if ((r = fuse_serve_init(fstitchd_argc, fstitchd_argv)) < 0)
	{
		fprintf(stderr, "fuse_serve_init: %d\n", r);
		return r;
	}
#endif

	if ((r = bdesc_autorelease_pool_push()) < 0)
	{
		fprintf(stderr, "bdesc_autorelease_pool_push: %i\n", r);
		return r;
	}
	
	printf("fstitchd basic initialization complete!\n");
	
	printf("fstitchd: default write back cache size = %d\n", nwbblocks);

	//
	// Setup uhfses

	if (! (uhfses = vector_create()) )
	{
		fprintf(stderr, "OOM, vector_create\n");
		return -ENOMEM;
	}

	if (use_disk_1)
	{
		BD_t * bd = NULL;
#ifdef __KERNEL__
		extern char * linux_device;
		if (linux_device)
		{
			extern int use_unsafe_disk_cache;
			printf("Using device %s\n", linux_device);
			if (! (bd = linux_bd(linux_device, use_unsafe_disk_cache)) )
				fprintf(stderr, "linux_bd(\"%s\") failed\n", linux_device);
#if LINUX_BD_TIMING_TEST
			const uint32_t block_numbers[4][30] = {
				{10, 12, 14, 16, 18, 20, 22, 24,
				 26, 28, 30, 32, 34, 36, 38,
				 10000000, 10000002, 10000004, 10000006,
				 10000008, 10000010, 10000012, 10000014,
				 10000016, 10000018, 10000020, 10000022,
				 10000024, 10000026, 10000028},
				{10, 10000000, 12, 10000002, 14, 10000004, 16, 10000006,
				 18, 10000008, 20, 10000010, 22, 10000012, 24, 10000014,
				 26, 10000016, 28, 10000018, 30, 10000020, 32, 10000022,
				 34, 10000024, 36, 10000026, 38, 10000028},
				{10, 12, 14, 16, 18, 20, 22, 24,
				 26, 28, 30, 32, 34, 36, 38, 40,
				 42, 44, 46, 48, 50, 52, 54, 56,
				 58, 60, 62, 64, 66, 68},
				{68, 66, 64, 62, 60, 58, 56, 54,
				 52, 50, 48, 46, 44, 42, 40, 38,
				 36, 34, 32, 30, 28, 26, 24, 22,
				 20, 18, 16, 14, 12, 10},
			};
			int seq, jiffies = jiffy_time();
			printf("Timing test: running...\n");
			for(seq = 0; seq < 75; seq++)
			{
				uint32_t number;
				for(number = 0; number < 30; number++)
				{
					bdesc_t * block;
					patch_t * init = NULL;
					block = CALL(bd, synthetic_read_block, block_numbers[0][number] * 8, 8);
					patch_create_init(block, bd, &init);
					CALL(bd, write_block, block);
				}
				while(revision_tail_flights_exist())
				{
					revision_tail_wait_for_landing_requests();
					revision_tail_process_landing_requests();
				}
			}
			jiffies = jiffy_time() - jiffies;
			printf("Timing test complete! Total time: %d.%02d seconds\n", jiffies / HZ, (jiffies % HZ) * 100 / HZ);
			DESTROY(bd);
			bd = NULL;
#endif
#elif defined(UNIXUSER)
		extern char * unix_file;
		if (unix_file)
		{
			printf("Using file '%s'\n", unix_file);
			if (! (bd = unix_file_bd(unix_file, 512)) )
				fprintf(stderr, "unix_file_bd(\"%s\") failed\n", unix_file);
#endif
		}
#if ALLOW_CRASHSIM
		extern int use_crashsim;
		if (use_crashsim && bd)
		{
			if (use_crashsim == 1)
				use_crashsim = 100000;
			if (! (bd = crashsim_bd(bd, use_crashsim)) )
				fprintf(stderr, "crashsim_bd(%d) failed\n", use_crashsim);
		}
#endif
		if (bd)
		{
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
				fprintf(stderr, "linux_bd(\"%s\") failed\n", linux_device);
		}
#endif
		if (bd)
		{
			printf("Using disk 2\n");
			if ((r = construct_uhfses(bd, nwbblocks, uhfses)) < 0)
				return r;
		}
	}
	if (use_mem_bd)
	{
		BD_t * bd;

		if (! (bd = mem_bd(1024, 4096)) )
			fprintf(stderr, "mem_bd(1024, 4096) failed\n");
		if (bd)
		{
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
			r = fstitchd_add_mount(fspaths[i], vector_elt(uhfses, i));
			if (r < 0)
			{
				fprintf(stderr, "fstitchd_add_mount: %i\n", r);
				return r;
			}
		}

		vector_destroy(uhfses);
		uhfses = NULL;
	}

	r = fstitchd_add_mount("/dev", modman_devfs);
	if (r < 0)
	{
		fprintf(stderr, "fstitchd_add_mount: %i\n", r);
		return r;
	}

	return 0;
}

static LFS_t * construct_lfs(fstitchd_partition_t * part, uint32_t cache_nblks, LFS_t * (*fs)(BD_t *), const char * name, uint16_t blocksize)
{
	LFS_t * plain_lfs;
	LFS_t * lfs = NULL;
	
	bool is_journaled = 0;
	BD_t * journal;
	BD_t * cache = construct_cacheing(part->bd, cache_nblks, blocksize);
	if (!cache)
		return NULL;

#if ALLOW_JOURNAL
	extern int use_journal;
	if (use_journal)
	{
		journal = journal_bd(cache, use_journal < 2);
		if (journal)
			is_journaled = 1;
		else
		{
			fprintf(stderr, "journal_bd failed, not journaling\n");
			journal = cache;
		}
	}
	else
#endif
		journal = cache;

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
				fprintf(stderr, "get_root: %i\n", r);
				return NULL;
			}
			r = CALL(plain_lfs, lookup_name, root_ino, ".journal", &journal_ino);
			if (r < 0)
			{
				fprintf(stderr, "No journal file; restarting modules\n");
				goto disable_journal;
			}

			journalbd = loop_bd(plain_lfs, journal_ino);
			if (!journalbd)
			{
				fprintf(stderr, "loop_bd failed\n");
				goto disable_journal;
			}
			r = journal_bd_set_journal(journal, journalbd);
			if (r < 0)
			{
				fprintf(stderr, "journal_bd_set_journal: error %d\n", -r);
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
		fprintf(stderr, "\nlfs creation failed\n");
		return NULL;
	}
#if ALLOW_JOURNAL
	if (is_journaled)
		printf(" (journaled; meta: %d)", use_journal < 2);
	else
#endif
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
	fstitchd_partition_t * part = NULL;
	uint32_t i;

	if (! (partitions = vector_create()) )
	{
		fprintf(stderr, "OOM, vector_create\n");
		return -ENOMEM;
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
			if (type == PTABLE_JOS_TYPE || type == PTABLE_LINUX_TYPE)
			{
				if (! (part = malloc(sizeof(fstitchd_partition_t))) )
				{
					fprintf(stderr, "OOM, malloc\n");
					return -ENOMEM;
				}
				part->bd = pc_ptable_bd(ptbl, i);
				if (part->bd)
				{
					part->type = type;
					part->subtype = 0;
					snprintf(part->description, 32, "Partition %d", i);
					if (vector_push_back(partitions, part))
					{
						fprintf(stderr, "OOM, vector_push_back\n");
						return -ENOMEM;
					}
				}
			}
			else if (type == PTABLE_FREEBSD_TYPE)
			{
				BD_t * tmppart;
				tmppart = pc_ptable_bd(ptbl, i);
				if (tmppart)
				{
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
		if (! (part = malloc(sizeof(fstitchd_partition_t))) )
		{
			fprintf(stderr, "OOM, malloc\n");
			return -ENOMEM;
		}
		// No partition table, make it look like a JOS partition...
		part->bd = bd;
		part->type = PTABLE_JOS_TYPE;
		part->subtype = 0;
		snprintf(part->description, 32, "<entire disk>");
		if (vector_push_back(partitions, part))
		{
			fprintf(stderr, "OOM, vector_push_back\n");
			return -ENOMEM;
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

		if (part->type == PTABLE_JOS_TYPE)
		{
			lfs = construct_lfs(part, cache_nblks, josfs_lfs, 4096);
		}
		else if (part->type == PTABLE_FREEBSD_TYPE)
		{
			// TODO handle 1K fragment size in UFS?
			lfs = construct_lfs(part, cache_nblks, ufs_lfs, 2048);
		}
		else if (part->type == PTABLE_LINUX_TYPE)
		{
			// TODO handle differnt block sizes
			lfs = construct_lfs(part, cache_nblks, ext2_lfs, 4096);
		}
		else
		{
			printf("Unknown partition type %x\n", part->type);
			lfs = NULL;
		}
		if(!lfs)
			continue;

		if (! (lfs = patchgroup_lfs(lfs)))
			return -1;
		if (! (u = uhfs_cfs(lfs)) )
		{
			fprintf(stderr, "uhfs_cfs() failed\n");
			return -1;
		}
#if USE_ICASE
		if (! (u = icase_cfs(u)) )
		{
			fprintf(stderr, "icase_cfs() failed\n");
			return -1;
		}
#endif
		if (vector_push_back(uhfses, u) < 0)
		{
			fprintf(stderr, "vector_push_back() failed\n");
			return -1;
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
	if (bs != bd->blocksize)
	{
		/* create a resizer */
		if (! (bd = block_resizer_bd(bd, bs)) )
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

#if ALLOW_UNLINK
	extern int use_unlink;
	if (use_unlink)
	{
		printf("Initializing unlink device.\n");
		if (! (bd = unlink_bd(bd)) )
			return NULL;
	}
#endif

	return bd;
}

int handle_bsd_partitions(void * bsdtbl, vector_t * partitions)
{
	uint32_t j, bsd_max = bsd_ptable_count(bsdtbl);
	uint8_t fstype;
	fstitchd_partition_t * part = NULL;

	for (j = 1; j <= bsd_max; j++)
	{
		fstype = bsd_ptable_type(bsdtbl, j);
		if (fstype != BSDLABEL_FS_UNUSED)
		{
			if (! (part = malloc(sizeof(fstitchd_partition_t))) )
			{
				fprintf(stderr, "OOM, malloc\n");
				return -ENOMEM;
			}
			part->bd = bsd_ptable_bd(bsdtbl, j);
			if (part->bd)
			{
				part->type = PTABLE_FREEBSD_TYPE;
				part->subtype = fstype;
				snprintf(part->description, 32, "BSD Partition %d", j);
				if (vector_push_back(partitions, part))
				{
					fprintf(stderr, "OOM, vector_push_back\n");
					return -ENOMEM;
				}
			}
		}
	}
	return 0;
}
