#include <inc/lib.h>

#include <kfs/ide_pio_bd.h>
#include <inc/partition.h>
#include <kfs/pc_ptable_bd.h>
#include <kfs/wt_cache_bd.h>
#include <kfs/block_resizer_bd.h>
#include <kfs/nbd_bd.h>
#include <kfs/wholedisk_lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/uhfs.h>
#include <kfs/josfs_cfs.h>
#include <kfs/table_classifier_cfs.h>
#include <kfs/fidfairy_cfs.h>
#include <kfs/cfs_ipc_serve.h>
#include <kfs/depman.h>
#include <kfs/kfsd.h>
#include <kfs/kfsd_init.h>


static const char * josfspath = "/";
static const char * fspaths[] = {"/k0", "/k1", "/k2", "/k4"};

// Init kfsd modules.
int kfsd_init(void)
{
	BD_t * bd_disk = NULL;
	bool use_disk_1 = 0, use_net = 0;
	void * ptbl = NULL;
	BD_t * partitions[4] = {NULL};
	CFS_t * uhfses[2] = {NULL};
	CFS_t * josfscfs = NULL;
	CFS_t * table_class = NULL;
	CFS_t * fidfairy = NULL;
	uint32_t i;
	int r;

	if((r = depman_init()) < 0)
	{
		printf("Failed to initialized DEP MAN: %e\n", r);
		return r;
	}

	if (!cfs_ipc_serve_init())
		kfsd_shutdown();


	if (use_disk_1)
	{
		if (! (bd_disk = ide_pio_bd(1)) )
			kfsd_shutdown();

		/* discover partitions */
		ptbl = pc_ptable_init(bd_disk);
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
				kfsd_shutdown();
			}
		}
		else
		{
			printf("Using whole disk.\n");
			partitions[0] = bd_disk;
		}

		/* setup each partition's cache, basefs, and uhfs */
		for (i=0; i < 4; i++)
		{
			BD_t * cache;
			BD_t * resizer;
			LFS_t * lfs;
			
			if (!partitions[i])
				continue;

			/* create a cache below the resizer */
			if (! (cache = wt_cache_bd(partitions[i], 32)) )
				kfsd_shutdown();

			/* create a resizer */
			if (! (resizer = block_resizer_bd(cache, 4096)) )
				kfsd_shutdown();

			/* create a cache above the resizer */
			if (! (cache = wt_cache_bd(resizer, 4)) )
				kfsd_shutdown();

			if (i == 1)
			{
				if (! (lfs = josfs(cache)) )
					kfsd_shutdown();
			}
			else
			{
				if (! (lfs = wholedisk(cache)) )
					kfsd_shutdown();
			}

			if (! (uhfses[i] = uhfs(lfs)) )
				kfsd_shutdown();
		}
	}

	// josfs_cfs
	if (! (josfscfs = josfs_cfs()) )
		kfsd_shutdown();

	// table_classifier_cfs
	if (! (table_class = table_classifier_cfs(NULL, NULL, 0)) )
		kfsd_shutdown();
	assert(!get_frontend_cfs());
	set_frontend_cfs(table_class);

	// Mount the uhfs modules
	for (i=0; i < sizeof(uhfses)/sizeof(uhfses[0]); i++)
	{
		if (!uhfses[i])
			continue;

		r = table_classifier_cfs_add(table_class, fspaths[i], uhfses[i]);
		if (r < 0)
			kfsd_shutdown();
	}
	
	if(use_net)
	{
		BD_t * nbd;
		BD_t * cache;
		LFS_t * wd;
		CFS_t * net;

		/* delay kfsd startup slightly for netd to start */
		sleep(200);

		nbd = nbd_bd("192.168.4.15", 2492);
		if(!nbd)
			goto a;
		cache = wt_cache_bd(nbd, 8);
		if(!cache)
			goto b;
		wd = wholedisk(cache);
		if(!wd)
			goto c;
		net = uhfs(wd);
		if(!net)
			goto d;

		if(table_classifier_cfs_add(table_class, "/net", net))
		{
			DESTROY(net);
			d: DESTROY(wd);
			c: DESTROY(cache);
			b: DESTROY(nbd);
			a: (void) 0;
		}
	}

	// mount josfs_cfs last, so that it gets priority over no other mounts
	r = table_classifier_cfs_add(table_class, josfspath, josfscfs);
	if (r < 0)
		kfsd_shutdown();

	// fidfairy
	if (! (fidfairy = fidfairy_cfs(get_frontend_cfs())) )
		kfsd_shutdown();
	set_frontend_cfs(fidfairy);

	return 0;
}
