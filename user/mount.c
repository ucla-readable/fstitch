#include <kfs/ide_pio_bd.h>
#include <kfs/nbd_bd.h>
#include <kfs/loop_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/wt_cache_bd.h>
#include <kfs/wb_cache_bd.h>
#include <kfs/journal_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/uhfs.h>
#include <kfs/mount_selector_cfs.h>
#include <kfs/modman.h>
#include <kfs/mem_bd.h>

#include <inc/cfs_ipc_client.h>
#include <inc/kfs_ipc_client.h>
#include <inc/kfs_uses.h>
#include <arch/simple.h>
#include <inc/stdio.h>

static bool verbose = 0;

#define WB_CACHE 0
#define WT_CACHE 1
#define NO_CACHE 2

static struct Scfs_metadata md;

static CFS_t * build_uhfs(BD_t * bd, bool enable_journal, LFS_t * journal_lfs, const char * journal_lfs_file, int cache_type, uint32_t cache_nblks)
{
//	void * ptbl = NULL;
	BD_t * partitions[4] = {NULL};
	uint32_t i;

	/* discover partitions */
/* // pc_ptable_init not yet supported by kfs rpc
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
*/
	{
		//printf("Using whole disk.\n");
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
		LFS_t * josfs_lfs;
		LFS_t * lfs;
		bool journaling = 0;
		CFS_t * u;
			
		if (!partitions[i])
			continue;

		switch (cache_type)
		{
			case WB_CACHE:
				cache = wb_cache_bd(partitions[i], cache_nblks);
				if (!cache)
				{
					kdprintf(STDERR_FILENO, "wb_cache_bd() failed\n");
					exit(0);
				}
				break;
			case WT_CACHE:
				cache = wt_cache_bd(partitions[i], cache_nblks);
				if (!cache)
				{
					kdprintf(STDERR_FILENO, "wt_cache_bd() failed\n");
					exit(0);
				}
				break;
			case NO_CACHE:
				cache = partitions[i];
				break;
			default:
				kdprintf(STDERR_FILENO, "%s() does not know about cache type %d\n", __FUNCTION__, cache_type);
				exit(0);
				cache = NULL; // satisfy compiler
		}

		/* create a resizer if needed */
		if ((resizer = block_resizer_bd(cache, JOSFS_BLKSIZE)) && cache_type != NO_CACHE)
		{
			/* create a cache above the resizer */
			if (! (cache = wt_cache_bd(resizer, 16)) )
			{
				kdprintf(STDERR_FILENO, "wt_cache_bd() failed\n");
				exit(0);
			}
		}

		if (enable_journal)
		{
			BD_t * journal;
			BD_t * journalbd;
			int r;

			if (! (journal = journal_bd(cache)) )
			{
				kdprintf(STDERR_FILENO, "journal_bd() failed\n");
				exit(0);
			}

			lfs = josfs_lfs = josfs(journal);
			if (!lfs)
			{
				(void) DESTROY(journal);
				kdprintf(STDERR_FILENO, "%s: josfs() failed\n", __FUNCTION__);
				return NULL;
			}

			if (!journal_lfs) // internal journal
				journal_lfs = lfs;

			journalbd = loop_bd(journal_lfs, journal_lfs_file);
			if (!journalbd)
			{
				(void) DESTROY(lfs);
				(void) DESTROY(journal);
				kdprintf(STDERR_FILENO, "%s: loop_bd(journal_lfs, \"%s\") failed\n", __FUNCTION__, journal_lfs_file);
				return NULL;
			}
			if ((r = journal_bd_set_journal(journal, journalbd)) >= 0)
				journaling = 1;
			else
			{
				(void) DESTROY(lfs);
				(void) DESTROY(journal);
				kdprintf(STDERR_FILENO, "%s: journal_bd_set_journal() failed: %i\n", __FUNCTION__, r);
				return NULL;
			}
		}
		else
			lfs = josfs_lfs = josfs(cache);

		if (lfs)
			printf("Using josfs");
		else if ((lfs = wholedisk(cache)))
			printf("Using wholedisk");
		else
		{
			kdprintf(STDERR_FILENO, "lfs creation failed\n");
			exit(0);
		}

		if (journaling)
			printf(" [journaled%s]", (lfs == journal_lfs) ? "" : " external");

		if (i == 0 && partitions[0] == bd)
			printf(" on disk.\n");
		else
			printf(" on partition %d.\n", i);

		if (! (u = uhfs(lfs)) )
		{
			kdprintf(STDERR_FILENO, "uhfs() failed\n");
			exit(0);
		}

		return u;
	}

	return NULL;
}

static void print_usage(const char * bin)
{
	printf("Usage:\n");
	printf("%s -d <device> -m <mount_point> [-v]\n", bin);
	printf("    [-j <on|<extern_file>|off*>]\n");
	printf("    [-$ <num_blocks>] [-c <wb*|wt|none>]\n");
	printf("  <device> is one of:\n");
	printf("    ide  <controllerno> <diskno> <readahead>\n");
	printf("    nbd  <host> [-p <port>]\n");
	printf("    loop <file>\n");
	printf("    bd   <bd_name>\n");
	printf("    mem  <blocksize> <blockcount>\n");
}

static void parse_options(int argc, const char ** argv, bool * journal, LFS_t ** journal_lfs, const char ** journal_lfs_file, int * cache_type, uint32_t * cache_num_blocks)
{
	const char * journal_str;
	const char * cache_type_str;
	const char * cache_num_blocks_str;

	if (get_arg_idx(argc, argv, "-v"))
		verbose = 1;

	if ((journal_str = get_arg_val(argc, argv, "-j")))
	{
		if (!strcmp("off", journal_str))
		{
			*journal = 0;
			*journal_lfs = NULL;
			*journal_lfs_file = NULL;
		}
		else if (!strcmp("on", journal_str)) 
		{
			*journal = 1;
			*journal_lfs = NULL;
			*journal_lfs_file = "/.journal";
		}
		else
		{
			int r;

			// Find the lfs for journal_str
			// TODO: support devfs_cfs's files (for now now, cfs_get_metadata(KFS_feature_file_lfs) will fail)
			memset(&md, 0, sizeof(md));
			r = cfs_get_metadata(journal_str, KFS_feature_file_lfs.id, &md);
			if (r < 0)
			{
				kdprintf(STDERR_FILENO, "get_metadata(%s, KFS_feature_file_lfs): %i\n", journal_str, r);
				exit(0);
			}
			*journal_lfs = create_lfs(*(uint32_t *) md.data);
			if (!*journal_lfs)
			{
				kdprintf(STDERR_FILENO, "Unable to find the LFS for external journal file %s\n", journal_str);
				exit(0);
			}

			*journal_lfs_file = journal_str;
			*journal = 1;
		}
	}

	if ((cache_type_str = get_arg_val(argc, argv, "-c")))
	{
		if (!strcmp("wb", cache_type_str))
			*cache_type = WB_CACHE;
		else if (!strcmp("wt", cache_type_str))
			*cache_type = WT_CACHE;
		else if (!strcmp("none", cache_type_str))
			*cache_type = NO_CACHE;
		else
		{
			kdprintf(STDERR_FILENO, "Illegal -c option \"%s\"\n", cache_type_str);
			print_usage(argv[0]);
			exit(0);
		}
	}

	if ((cache_num_blocks_str = get_arg_val(argc, argv, "-$")))
		*cache_num_blocks = strtol(cache_num_blocks_str, NULL, 10);
}

static BD_t * create_disk(int argc, const char ** argv)
{
	int device_index;
	BD_t * disk = NULL;

	device_index = get_arg_idx(argc, argv, "-d");
	if (device_index <= 0)
	{
		kdprintf(STDERR_FILENO, "No -d parameter\n");
		print_usage(argv[0]);
		exit(0);
	}

	if (++device_index >= argc)
	{
		kdprintf(STDERR_FILENO, "No parameters passed with -d\n");
		print_usage(argv[0]);
		exit(0);
	}

	if (!strcmp("ide", argv[device_index]))
	{
		uint8_t controllerno, diskno, readahead;

		if (device_index + 3 >= argc)
		{
			kdprintf(STDERR_FILENO, "Insufficient parameters for ide\n");
			print_usage(argv[0]);
			exit(0);
		}

		controllerno = strtol(argv[device_index+1], NULL, 10);
		diskno = strtol(argv[device_index+2], NULL, 10);
		readahead = strtol(argv[device_index+3], NULL, 10);

		if (! (disk = ide_pio_bd(controllerno, diskno, readahead)) )
		{
			kdprintf(STDERR_FILENO, "ide_pio_bd(%d, %d, %d) failed\n", controllerno, diskno);
			return NULL;
		}
	}
	else if (!strcmp("nbd", argv[device_index]))
	{
		const char * host;
		uint16_t port = 2492;
		const char * port_str;

		if (device_index + 1 >= argc)
		{
			kdprintf(STDERR_FILENO, "Insufficient parameters for nbd\n");
			print_usage(argv[0]);
			exit(0);
		}

		host = argv[device_index+1];
		if ((port_str = get_arg_val(argc, argv, "-p")))
			port = strtol(port_str, NULL, 10);

		if (! (disk = nbd_bd(host, port)) )
		{
			kdprintf(STDERR_FILENO, "nbd_bd(%s, %d) failed\n", host, port);
			return NULL;
		}
	}
	else if (!strcmp("mem", argv[device_index]))
	{
		uint32_t block_count;
		const char * block_count_str;
		uint16_t blocksize;
		const char * blocksize_str;

		if (device_index + 2 >= argc)
		{
			kdprintf(STDERR_FILENO, "Insufficient parameters for mem\n");
			print_usage(argv[0]);
			exit(0);
		}

		blocksize_str = argv[device_index + 1];
		blocksize = strtol(blocksize_str, NULL, 10);
		if (blocksize == 0) {
			kdprintf(STDERR_FILENO, "Bad block size for mem\n");
		}

		block_count_str = argv[device_index + 2];
		block_count = strtol(block_count_str, NULL, 10);
		if (block_count == 0) {
			kdprintf(STDERR_FILENO, "Bad block count for mem\n");
		}

		if (! (disk = mem_bd(block_count, blocksize)) )
		{
			kdprintf(STDERR_FILENO, "mem_bd(%d, %d) failed\n", block_count, blocksize);
			return NULL;
		}
	}
	else if (!strcmp("loop", argv[device_index]))
	{
		const char * filename;
		LFS_t * lfs;
		int r;

		if (device_index + 1 >= argc)
		{
			kdprintf(STDERR_FILENO, "Insufficient parameters for loop\n");
			print_usage(argv[0]);
			exit(0);
		}

		filename = argv[device_index+1];

		// Find the lfs for filename
		memset(&md, 0, sizeof(md));
		r = cfs_get_metadata(filename, KFS_feature_file_lfs.id, &md);
		if (r < 0)
		{
			kdprintf(STDERR_FILENO, "get_metadata(%s, KFS_feature_file_lfs): %i\n", filename, r);
			exit(0);
		}
		lfs = create_lfs(*(uint32_t *) md.data);
		if (!lfs)
		{
			kdprintf(STDERR_FILENO, "Unable to find the LFS for file %s\n", filename);
			exit(0);
		}

		// Create loop_bd
		if (! (disk = loop_bd(lfs, filename)) )
		{
			kdprintf(STDERR_FILENO, "loop_bd(%s, %s) failed\n", modman_name_lfs(lfs), filename);
			return NULL;
		}
	}
	else if (!strcmp("bd", argv[device_index]))
	{
		const char * bd_name;
		modman_it_t it;
		int r;

		if (device_index + 1 >= argc)
		{
			kdprintf(STDERR_FILENO, "Insufficient parameters for bd\n");
			print_usage(argv[0]);
			exit(0);
		}

		bd_name = argv[device_index+1];

		r = modman_it_init_bd(&it);
		assert(r >= 0);
		while ((disk = modman_it_next_bd(&it)))
			if (!strcmp(bd_name, modman_name_bd(disk)))
				break;
		modman_it_destroy(&it);

		if (!disk)
		{
			kdprintf(STDERR_FILENO, "Unable to find BD %s\n", bd_name);
			return NULL;
		}
	}
	else
	{
		kdprintf(STDERR_FILENO, "Unknown device type \"%s\"\n", argv[device_index]);
			print_usage(argv[0]);
			exit(0);
	}

	assert(disk);

	return disk;
}


void umain(int argc, const char ** argv)
{
	const char * mount_point;
	BD_t * disk;
	LFS_t * journal_lfs = NULL;
	const char * journal_lfs_file = NULL;
	CFS_t * cfs;
	bool journal = 0;
	int cache_type = WB_CACHE;
	uint32_t cache_num_blocks = 128;
	CFS_t * mselect;
	int r;

	if (get_arg_idx(argc, argv, "-h"))
	{
		print_usage(argv[0]);
		exit(0);
	}

	mount_point = get_arg_val(argc, argv, "-m");
	if (!mount_point)
	{
		kdprintf(STDERR_FILENO, "No mount specified\n");
		print_usage(argv[0]);
		exit(0);
	}

	parse_options(argc, argv, &journal, &journal_lfs, &journal_lfs_file, &cache_type, &cache_num_blocks);

	disk = create_disk(argc, argv);
	if (!disk)
		exit(0);

	cfs = build_uhfs(disk, journal, journal_lfs, journal_lfs_file, cache_type, cache_num_blocks);
	if (!cfs)
		exit(0);

	mselect = get_mount_selector();
	if (!mselect)
		exit(0);

	r = mount_selector_cfs_add(mselect, mount_point, cfs);
	if (r < 0)
	{
		kdprintf(STDERR_FILENO, "mount_selector_cfs_add(): %i\n", r);
		exit(0);
	}
}
