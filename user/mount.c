#include <kfs/ide_pio_bd.h>
#include <kfs/nbd_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/wt_cache_bd.h>
#include <kfs/chdesc_stripper_bd.h>
#include <kfs/journal_queue_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/journal_lfs.h>
#include <kfs/uhfs.h>
#include <kfs/table_classifier_cfs.h>
#include <kfs/modman.h>

#include <inc/kfs_uses.h>
#include <arch/simple.h>
#include <inc/stdio.h>

static bool verbose = 0;


static CFS_t * build_uhfs(BD_t * bd, bool enable_journal, uint32_t cache_nblks)
{
//	void * ptbl = NULL;
	BD_t * partitions[4] = {NULL};
	uint32_t i;
	int josfs_fsck = !enable_journal;

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
		LFS_t * lfs;
		bool journaling = 0;
		LFS_t * journal = NULL;
		CFS_t * u;
			
		if (!partitions[i])
			continue;

		if (! (cache = wt_cache_bd(partitions[i], cache_nblks)) )
		{
			fprintf(STDERR_FILENO, "wt_cache_bd() failed\n");
			exit();
		}

		/* create a resizer if needed */
		if ((resizer = block_resizer_bd(cache, 4096)) )
		{
			/* create a cache above the resizer */
			if (! (cache = wt_cache_bd(resizer, 4)) )
			{
				fprintf(STDERR_FILENO, "wt_cache_bd() failed\n");
				exit();
			}
		}

		if (! (cache = chdesc_stripper_bd(cache)) )
		{
			fprintf(STDERR_FILENO, "chdesc_stripper_bd() failed\n");
			exit();
		}

		if (enable_journal)
		{
			BD_t * journal_queue;

			if (! (journal_queue = journal_queue_bd(cache)) )
			{
				fprintf(STDERR_FILENO, "journal_queue_bd() failed\n");
				exit();
			}

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
					printf("%s: journal_lfs() failed\n", __FUNCTION__);
					return NULL;
				}
			}
			else
			{
				(void) DESTROY(journal_queue);
				printf("%s: josfs() failed\n", __FUNCTION__);
				return NULL;
			}
		}
		else
			lfs = josfs(cache, &josfs_fsck);

		if (lfs)
			printf("Using josfs");
		else if ((lfs = wholedisk(cache)))
			printf("Using wholedisk");
		else
		{
			fprintf(STDERR_FILENO, "lfs creation failed\n");
			exit();
		}

		if (journaling)
			printf(" [journaled, %u kB/s max avg]", journal_lfs_max_bandwidth(journal));

		if (i == 0 && partitions[0] == bd)
			printf(" on disk.\n");
		else
			printf(" on partition %d.\n", i);

		if (! (u = uhfs(lfs)) )
		{
			fprintf(STDERR_FILENO, "uhfs() failed\n");
			exit();
		}

		if (verbose)
			printf("uhfs made, u->instance = 0x%08x\n", u->instance);
		return u;
	}

	return NULL;
}

static void print_usage(const char * bin)
{
	printf("Usage: %s -d <device> -m <mount_point> [-j <on|off>] [-$ <num_blocks>] [-v]\n", bin);
	printf("  <device> is one of:\n");
	printf("    ide <controller> <diskno>\n");
	printf("    nbd <host> [-p <port>]\n");
}

static void parse_options(int argc, const char ** argv, bool * journal, uint32_t * cache_num_blocks)
{
	const char * journal_str;
	const char * cache_num_blocks_str;

	if (get_arg_idx(argc, argv, "-v"))
		verbose = 1;

	if ((journal_str = get_arg_val(argc, argv, "-j")))
	{
		if (!strcmp("on", journal_str))
			*journal = 1;
		else if (!strcmp("off", journal_str))
			*journal = 0;
		else
		{
			fprintf(STDERR_FILENO, "Illegal -j option \"%s\"\n", journal_str);
			print_usage(argv[0]);
			exit();
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
		fprintf(STDERR_FILENO, "No -d parameter\n");
		print_usage(argv[0]);
		exit();
	}

	if (++device_index >= argc)
	{
		fprintf(STDERR_FILENO, "No parameters passed with -d\n");
		print_usage(argv[0]);
		exit();
	}

	if (!strcmp("ide", argv[device_index]))
	{
		uint8_t controllerno, diskno;

		if (device_index + 2 >= argc)
		{
			fprintf(STDERR_FILENO, "Insufficient parameters for ide\n");
			print_usage(argv[0]);
			exit();
		}

		controllerno = strtol(argv[device_index+1], NULL, 10);
		diskno = strtol(argv[device_index+2], NULL, 10);

		if (! (disk = ide_pio_bd(controllerno, diskno)) )
		{
			fprintf(STDERR_FILENO, "ide_pio_bd(%d, %d) failed\n", controllerno, diskno);
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
			fprintf(STDERR_FILENO, "Insufficient parameters for nbd\n");
			print_usage(argv[0]);
			exit();
		}

		host = argv[device_index+1];
		if ((port_str = get_arg_val(argc, argv, "-p")))
			port = strtol(port_str, NULL, 10);

		if (! (disk = nbd_bd(host, port)) )
		{
			fprintf(STDERR_FILENO, "nbd_bd(%s, %d) failed\n", host, port);
			return NULL;
		}
	}
	else
	{
		fprintf(STDERR_FILENO, "Unknown device type \"%s\"\n", argv[device_index]);
			print_usage(argv[0]);
			exit();
	}

	assert(disk);

	return disk;
}


void umain(int argc, const char ** argv)
{
	const char * mount_point;
	BD_t * disk;
	CFS_t * cfs;
	bool journal = 0;
	uint32_t cache_num_blocks = 128;
	CFS_t * tclass;
	int r;

	if (get_arg_idx(argc, argv, "-h"))
	{
		print_usage(argv[0]);
		exit();
	}

	mount_point = get_arg_val(argc, argv, "-m");
	if (!mount_point)
	{
		fprintf(STDERR_FILENO, "No mount specified\n");
		print_usage(argv[0]);
		exit();
	}

	parse_options(argc, argv, &journal, &cache_num_blocks);

	disk = create_disk(argc, argv);
	if (!disk)
		exit();

	cfs = build_uhfs(disk, journal, cache_num_blocks);
	if (!cfs)
		exit();

	tclass = get_table_classifier();
	if (!tclass)
		exit();

	r = table_classifier_cfs_add(tclass, mount_point, cfs);
	if (r < 0)
	{
		fprintf(STDERR_FILENO, "table_classifier_cfs_add(): %e\n", r);
		exit();
	}
}
