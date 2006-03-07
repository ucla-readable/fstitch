#ifdef KUDOS
#include <inc/lib.h>
#endif
#include <lib/kdprintf.h>
#include <lib/vector.h>
#include <lib/partition.h>
#include <lib/sleep.h>
#include <lib/jiffies.h>
#include <lib/disklabel.h>
#include <assert.h>

#include <kfs/ide_pio_bd.h>
#include <kfs/pc_ptable.h>
#include <kfs/bsd_ptable.h>
#include <kfs/wt_cache_bd.h>
#include <kfs/wb_cache_bd.h>
#include <kfs/elevator_cache_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/nbd_bd.h>
#include <kfs/loop_bd.h>
#ifdef UNIXUSER
#include <kfs/unix_file_bd.h>
#endif
#include <kfs/journal_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/ufs_base.h>
#include <kfs/opgroup_lfs.h>
#include <kfs/uhfs.h>
#include <kfs/mirror_bd.h>
#ifdef KUDOS
#include <kfs/mount_selector_cfs.h>
#include <kfs/cfs_ipc_opgroup.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/ipc_serve.h>
#endif
#ifdef UNIXUSER
#include <kfs/fuse_serve.h>
#endif
#include <kfs/modman.h>
#include <kfs/sched.h>
#include <kfs/kfsd.h>
#include <kfs/debug.h>
#include <kfs/kfsd_init.h>

int construct_uhfses(BD_t * bd, uint32_t cache_nblks, bool allow_journal, vector_t * uhfses);
BD_t * construct_cacheing(BD_t * bd, uint32_t cache_nblks, uint32_t bs);
void handle_bsd_partitions(void * bsdtbl, vector_t * partitions);

static const char * fspaths[] = {"/", "/k0", "/k1", "/k2", "/k3"};

struct kfsd_partition {
	BD_t * bd;
	uint16_t type;
	uint16_t subtype;
	char description[32];
};
typedef struct kfsd_partition kfsd_partition_t;

//#define USE_MIRROR
#define USE_WB_CACHE
#ifndef USE_WB_CACHE
#define wb_cache_bd wt_cache_bd
#endif

int kfsd_init(int argc, char ** argv)
{
	const bool allow_journal = 1;
	const bool use_disk_0 = 1;
#ifdef KUDOS
	const bool use_disk_1 = 0;
#endif
#ifdef UNIXUSER
	const bool use_disk_1 = 1;
#endif
	const bool use_net    = 0;
	vector_t * uhfses = NULL;

#ifdef KUDOS
	CFS_t * table_class = NULL;
	CFS_t * opgroupscope_tracker = NULL;
#endif
	int r;

	if((r = KFS_DEBUG_INIT()) < 0)
	{
		kdprintf(STDERR_FILENO, "kfs_debug_init: %i\n", r);
#ifdef KUDOS
		while(r != 'y' && r != 'n')
		{
			kdprintf(STDERR_FILENO, "Start anyway? [Y/n] ");
			r = 0;
			while(r <= 0)
				r = sys_cgetc_nb();
			if(r == '\n')
				r = 'y';
			kdprintf(STDERR_FILENO, "%c\n", r);
		}
		if(r == 'n')
#endif
			kfsd_shutdown();
	}
	KFS_DEBUG_COMMAND(KFS_DEBUG_DISABLE, KDB_MODULE_BDESC);

	if((r = modman_init()) < 0)
	{
		kdprintf(STDERR_FILENO, "modman_init: %i\n", r);
		kfsd_shutdown();
	}

#if defined(KUDOS)
	if ((r = ipc_serve_init()) < 0)
	{
		kdprintf(STDERR_FILENO, "ipc_serve_init: %i\n", r);
		kfsd_shutdown();
	}

	if (!cfs_ipc_serve_init())
	{
		kdprintf(STDERR_FILENO, "cfs_ipc_serve_init failed\n");
		kfsd_shutdown();
	}
#elif defined(UNIXUSER)
	if ((r = fuse_serve_init(argc, argv)) < 0)
	{
		kdprintf(STDERR_FILENO, "fuse_serve_init: %d\n", r);
		kfsd_shutdown();
	}
#endif

	if ((r = sched_init()) < 0)
	{
		kdprintf(STDERR_FILENO, "sched_init: %i\n", r);
		kfsd_shutdown();
	}

	if ((r = bdesc_autorelease_pool_push()) < 0)
	{
		kdprintf(STDERR_FILENO, "bdesc_autorelease_pool_push: %i\n");
		kfsd_shutdown();
	}
	
	//
	// Setup uhfses

	if (! (uhfses = vector_create()) )
	{
		kdprintf(STDERR_FILENO, "OOM, vector_create\n");
		kfsd_shutdown();
	}

	if (use_net)
	{
		BD_t * bd;

		/* delay kfsd startup slightly for netd to start */
		jsleep(2 * HZ);

		if (! (bd = nbd_bd("192.168.1.2", 2492)) )
			kdprintf(STDERR_FILENO, "nbd_bd failed\n");
		if (bd && (r = construct_uhfses(bd, 512, allow_journal, uhfses)) < 0)
			kfsd_shutdown();
	}

	if (use_disk_0)
	{
		BD_t * bd;

#ifdef KUDOS
		if (! (bd = ide_pio_bd(0, 0, 80)) )
			kdprintf(STDERR_FILENO, "ide_pio_bd(0, 0, 0) failed\n");
#endif
#ifdef UNIXUSER
		const char file[] = "obj/unix-user/fs/fs.img";
		if (! (bd = unix_file_bd(file, 512)) )
			kdprintf(STDERR_FILENO, "unix_file_bd(\"%s\", 512) failed\n", file);
#endif
		if (bd)
		{
			OBJFLAGS(bd) |= OBJ_PERSISTENT;
			printf("Using elevator scheduler on disk %s.\n", modman_name_bd(bd));
			bd = elevator_cache_bd(bd, 128, 64, 3);
			if (!bd)
				kfsd_shutdown();
			if ((r = construct_uhfses(bd, 128, allow_journal, uhfses)) < 0)
				kfsd_shutdown();
		}
	}

	if (use_disk_1)
	{
		BD_t * bd;

#ifdef KUDOS
		if (! (bd = ide_pio_bd(0, 1, 0)) )
			kdprintf(STDERR_FILENO, "ide_pio_bd(0, 1, 0) failed\n");
#endif
#ifdef UNIXUSER
		const char file[] = "obj/unix-user/fs/ufs.img";
		if (! (bd = unix_file_bd(file, 512)) )
			kdprintf(STDERR_FILENO, "unix_file_bd(\"%s\", 512) failed\n", file);
#endif
		if (bd)
			OBJFLAGS(bd) |= OBJ_PERSISTENT;

		if (bd && (r = construct_uhfses(bd, 128, allow_journal, uhfses)) < 0)		
			kfsd_shutdown();
	}

	//
	// Mount uhfses

#ifdef KUDOS
	if (! (table_class = mount_selector_cfs()) )
		kfsd_shutdown();
	assert(!get_frontend_cfs());
	set_frontend_cfs(table_class);
#endif
	{
		const size_t uhfses_size = vector_size(uhfses);
		size_t i;
		for (i=0; i < uhfses_size; i++)
		{
			r = kfsd_add_mount(fspaths[i], vector_elt(uhfses, i));
			if (r < 0)
			{
				kdprintf(STDERR_FILENO, "kfsd_add_mount: %i\n", r);
				kfsd_shutdown();
			}
		}

		vector_destroy(uhfses);
		uhfses = NULL;
	}

	r = kfsd_add_mount("/dev", modman_devfs);
	if (r < 0)
		kfsd_shutdown();

#ifdef KUDOS
	if (! (opgroupscope_tracker = opgroupscope_tracker_cfs(get_frontend_cfs())) )
		kfsd_shutdown();
	set_frontend_cfs(opgroupscope_tracker);
#endif

	return 0;
}


// Bring up the filesystems for bd and add them to uhfses.
int construct_uhfses(BD_t * bd, uint32_t cache_nblks, bool allow_journal, vector_t * uhfses)
{
	const bool enable_fsck = 0;
	void * ptbl = NULL;
	void * bsdtbl = NULL;
	vector_t * partitions = NULL;
	kfsd_partition_t * part = NULL;
	uint32_t i;

	if (! (partitions = vector_create()) )
	{
		kdprintf(STDERR_FILENO, "OOM, vector_create\n");
		kfsd_shutdown();
	}

#ifdef USE_MIRROR
	bd = mirror_bd(bd, NULL, 4);
#endif

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
			if (type == PTABLE_KUDOS_TYPE)
			{
				if (! (part = malloc(sizeof(kfsd_partition_t))) )
				{
					kdprintf(STDERR_FILENO, "OOM, malloc\n");
					kfsd_shutdown();
				}
				part->bd = pc_ptable_bd(ptbl, i);
				if (part->bd)
				{
					OBJFLAGS(part->bd) |= OBJ_PERSISTENT;
					part->type = PTABLE_KUDOS_TYPE;
					part->subtype = 0;
					snprintf(part->description, 32, "Partition %d", i);
					if (vector_push_back(partitions, part))
					{
						kdprintf(STDERR_FILENO, "OOM, vector_push_back\n");
						kfsd_shutdown();
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
						handle_bsd_partitions(bsdtbl, partitions);
					}
					bsd_ptable_free(bsdtbl);
				}
			}
			else {
				printf("Unknown partition type %x\n", type);
			}
		}
		pc_ptable_free(ptbl);

		if (vector_size(partitions) <= 0)
		{
			printf("No partition found!\n");
			//kfsd_shutdown();
		}
	}
	else
	{
		printf("Using whole disk.\n");
		if (! (part = malloc(sizeof(kfsd_partition_t))) )
		{
			kdprintf(STDERR_FILENO, "OOM, malloc\n");
			kfsd_shutdown();
		}
		// No partition table, make it look like a KudOS partition...
		part->bd = bd;
		part->type = PTABLE_KUDOS_TYPE;
		part->subtype = 0;
		snprintf(part->description, 32, "Whole disk");
		if (vector_push_back(partitions, part))
		{
			kdprintf(STDERR_FILENO, "OOM, vector_push_back\n");
			kfsd_shutdown();
		}
	}

	/* setup each partition's cache, basefs, and uhfs */
	for (i = 0; i < vector_size(partitions); i++)
	{
		BD_t * cache, * journal;
		LFS_t * josfs_lfs;
		LFS_t * lfs = NULL;
		CFS_t * u;
		bool is_journaled = 0;
			
		part = vector_elt(partitions, i);
		if (!part)
			continue;

		if (part->type == PTABLE_KUDOS_TYPE)
		{
			cache = construct_cacheing(part->bd, cache_nblks, 4096);

			if (allow_journal)
			{
				journal = journal_bd(cache);
				if (journal)
					is_journaled = 1;
				else
				{
					kdprintf(STDERR_FILENO, "journal_bd failed, not journaling\n");
					journal = cache;
				}
			}
			else
				journal = cache;

			lfs = josfs_lfs = josfs(journal);

			if (is_journaled)
			{
				BD_t * journalbd = NULL;
				if (josfs_lfs)
				{
					inode_t root_ino, journal_ino;
					int r;

					r = CALL(josfs_lfs, get_root, &root_ino);
					if (r < 0)
					{
						kdprintf(STDERR_FILENO, "get_root: %i\n", r);
						kfsd_shutdown();
					}
					r = CALL(josfs_lfs, lookup_name, root_ino, ".journal", &journal_ino);
					if (r < 0)
					{
						kdprintf(STDERR_FILENO, "No journal file\n");
						goto disable_journal;
					}

					journalbd = loop_bd(josfs_lfs, journal_ino);
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
					(void) DESTROY(journal);
					if (journalbd)
						(void) DESTROY(journalbd);
					lfs = josfs_lfs;
					journal = cache;
					is_journaled = 0;
				}
			}

			if (josfs_lfs && enable_fsck)
			{
				printf("Fscking... ");
				int r = josfs_fsck(josfs_lfs);
				if (r == 0)
					printf("done.\n");
				else if (r > 0)
					printf("found %d errors\n", r);
				else
					printf("critical error: %i\n", r);
			}

			if (lfs)
				printf("Using josfs on %s", part->description);
			else if ((lfs = wholedisk(cache)))
				printf("Using wholedisk on %s", part->description);
			else
			{
				kdprintf(STDERR_FILENO, "\nlfs creation failed\n");
				kfsd_shutdown();
			}
			if (is_journaled)
				printf(" (journaled)");
			else
				printf(" (not journaled)");
			printf("\n");
		}
		else if (part->type == PTABLE_FREEBSD_TYPE)
		{
			// TODO handle 1K fragment size in UFS?
			cache = construct_cacheing(part->bd, cache_nblks, 2048);
			lfs = ufs(cache);

			if (lfs)
				printf("Using ufs on %s\n", part->description);
			else
			{
				kdprintf(STDERR_FILENO, "\nlfs creation failed\n");
				continue;
			}
		}
		else
		{
			printf("Unknown partition type %x\n", part->type);
			continue;
		}
		if (! (lfs = opgroup_lfs(lfs)))
			kfsd_shutdown();

		if (! (u = uhfs(lfs)) )
		{
			kdprintf(STDERR_FILENO, "uhfs() failed\n");
			kfsd_shutdown();
		}
		if (vector_push_back(uhfses, u) < 0)
		{
			kdprintf(STDERR_FILENO, "vector_push_back() failed\n");
			kfsd_shutdown();
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
			kfsd_shutdown();

		/* create a cache above the resizer */
		if (! (bd = wb_cache_bd(bd, cache_nblks)) )
			kfsd_shutdown();
	}
	else
	{
		if (! (bd = wb_cache_bd(bd, cache_nblks)) )
			kfsd_shutdown();
	}

	return bd;
}

void handle_bsd_partitions(void * bsdtbl, vector_t * partitions)
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
				kfsd_shutdown();
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
					kfsd_shutdown();
				}
			}
		}
	}
}
