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
#ifdef UNIXUSER
#include <kfs/unix_file_bd.h>
#endif
#include <kfs/journal_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/ufs_base.h>
#include <kfs/uhfs.h>
#include <kfs/josfs_cfs.h>
#include <kfs/mirror_bd.h>
#include <kfs/table_classifier_cfs.h>
#include <kfs/fidprotector_cfs.h>
#include <kfs/fidcloser_cfs.h>
#ifdef KUDOS
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

#define USE_THIRD_LEG 0 // 1 -> mount josfs_cfs at '/'

int construct_uhfses(BD_t * bd, uint32_t cache_nblks, vector_t * uhfses);
BD_t * construct_cacheing(BD_t * bd, uint32_t cache_nblks, uint32_t bs);
void handle_bsd_partitions(void * bsdtbl, vector_t * partitions);

static const char * fspaths[] = {
#if !USE_THIRD_LEG
"/",
#endif
"/k0", "/k1", "/k2", "/k3"};

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
	const bool use_disk_0 = 1;
	const bool use_disk_1 = 0;
	const bool use_net    = 0;
	vector_t * uhfses = NULL;

	CFS_t * table_class = NULL;
#ifdef KUDOS
	CFS_t * fidprotector = NULL;
	CFS_t * fidcloser = NULL;
#endif
	int r;

	if((r = KFS_DEBUG_INIT()) < 0)
	{
		kdprintf(STDERR_FILENO, "kfs_debug_init: %e\n", r);
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
		kdprintf(STDERR_FILENO, "modman_init: %e\n", r);
		kfsd_shutdown();
	}

#if defined(KUDOS)
	if ((r = ipc_serve_init()) < 0)
	{
		kdprintf(STDERR_FILENO, "ipc_serve_init: %e\n", r);
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
		kdprintf(STDERR_FILENO, "sched_init: %e\n", r);
		kfsd_shutdown();
	}

	if ((r = bdesc_autorelease_pool_push()) < 0)
	{
		kdprintf(STDERR_FILENO, "bdesc_autorelease_pool_push: %e\n");
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

#ifdef KUDOS
		if (! (bd = nbd_bd("192.168.0.2", 2492)) )
			kdprintf(STDERR_FILENO, "nbd_bd failed\n");
#else
		bd = NULL;
#endif

		if (bd && (r = construct_uhfses(bd, 512, uhfses)) < 0)
			kfsd_shutdown();
	}

	if (use_disk_0)
	{
		BD_t * bd;

#ifdef KUDOS
		if (! (bd = ide_pio_bd(0, 0, 0)) )
			kdprintf(STDERR_FILENO, "ide_pio_bd(0, 0, 0) failed\n");
#endif
#ifdef UNIXUSER
		if (! (bd = unix_file_bd("obj/unix-user/fs/fs.img", 512)) )
			kdprintf(STDERR_FILENO, "unix_file_bd(...) failed\n");
#endif
		if (bd)
		{
			OBJFLAGS(bd) |= OBJ_PERSISTENT;
			printf("Using elevator scheduler on disk %s.\n", modman_name_bd(bd));
			bd = elevator_cache_bd(bd, 32);
			if (!bd)
				kfsd_shutdown();
			if ((r = construct_uhfses(bd, 128, uhfses)) < 0)
				kfsd_shutdown();
		}
	}

	if (use_disk_1)
	{
		BD_t * bd;

#ifdef KUDOS
		if (! (bd = ide_pio_bd(0, 1, 0)) )
			kdprintf(STDERR_FILENO, "ide_pio_bd(0, 1, 0) failed\n");
#else
		bd = NULL;
#endif
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
				kdprintf(STDERR_FILENO, "table_classifier_cfs_add: %e\n", r);
				kfsd_shutdown();
			}
		}

		vector_destroy(uhfses);
		uhfses = NULL;
	}

	r = table_classifier_cfs_add(table_class, "/dev", modman_devfs);
	if (r < 0)
		kfsd_shutdown();

#ifdef KUDOS
	//
	// fidfairies

	if (! (fidprotector = fidprotector_cfs(get_frontend_cfs())) )
		kfsd_shutdown();
	set_frontend_cfs(fidprotector);

	if (! (fidcloser = fidcloser_cfs(get_frontend_cfs())) )
		kfsd_shutdown();
	set_frontend_cfs(fidcloser);
#endif

	return 0;
}


// Bring up the filesystems for bd and add them to uhfses.
int construct_uhfses(BD_t * bd, uint32_t cache_nblks, vector_t * uhfses)
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
		// Using type 0, subtype 1 to represent whole disk
		part->bd = bd;
		part->type = 0;
		part->subtype = 1;
		snprintf(part->description, 32, "Wholedisk");
		if (vector_push_back(partitions, part))
		{
			kdprintf(STDERR_FILENO, "OOM, vector_push_back\n");
			kfsd_shutdown();
		}
	}

	/* setup each partition's cache, basefs, and uhfs */
	for (i = 0; i < vector_size(partitions); i++)
	{
		BD_t * cache;
		LFS_t * josfs_lfs;
		LFS_t * lfs = NULL;
		CFS_t * u;
			
		part = vector_elt(partitions, i);
		if (!part)
			continue;

		if (part->type == PTABLE_KUDOS_TYPE)
		{
			cache = construct_cacheing(part->bd, cache_nblks, 4096);
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
				printf("Using josfs on %s\n", part->description);
			else if ((lfs = wholedisk(cache)))
				printf("Using wholedisk on %s\n", part->description);
			else
			{
				kdprintf(STDERR_FILENO, "\nlfs creation failed\n");
				kfsd_shutdown();
			}
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
