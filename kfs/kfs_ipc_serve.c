#include <inc/lib.h>

#include <kfs/modman.h>
#include <inc/serial_kfs.h>
#include <kfs/kfs_ipc_serve.h>

#define RETURN_IPC_INVAL do { val = -E_INVAL; goto exit; } while(0)
#define RETURN_IPC exit: ipc_send(whom, (uint32_t) val, NULL, 0, NULL)


//
// Destructors

void kis_destroy_cfs(envid_t whom, const Skfs_destroy_cfs_t * pg)
{
	CFS_t * cfs = (CFS_t *) pg->cfs;
	int val;

	if (!modman_name_cfs(cfs))
		RETURN_IPC_INVAL;

	val = DESTROY(cfs);

	RETURN_IPC;
}

void kis_destroy_lfs(envid_t whom, const Skfs_destroy_lfs_t * pg)
{
	LFS_t * lfs = (LFS_t *) pg->lfs;
	int val;

	if (!modman_name_lfs(lfs))
		RETURN_IPC_INVAL;

	val = DESTROY(lfs);

	RETURN_IPC;
}

void kis_destroy_bd(envid_t whom, const Skfs_destroy_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	int val;

	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = DESTROY(bd);

	RETURN_IPC;
}


//
// CFS

// table_classifier_cfs
#include <kfs/table_classifier_cfs.h>

void kis_table_classifier_cfs(envid_t whom, const Skfs_table_classifier_cfs_t * pg)
{
	uint32_t val = (uint32_t) table_classifier_cfs();
	printf("%s = 0x%08x\n", __FUNCTION__, val);
	ipc_send(whom, val, NULL, 0, NULL);
}

void kis_table_classifier_cfs_add(envid_t whom, const Skfs_table_classifier_cfs_add_t * pg)
{
	CFS_t * cfs = (CFS_t *) pg->cfs;
	CFS_t * path_cfs = (CFS_t *) pg->path_cfs;
	uint32_t val;

	printf("%s(0x%08x, %s, 0x%08x)\n", __FUNCTION__, cfs, pg->path, path_cfs);
	if (!modman_name_cfs(cfs) || !modman_name_cfs(path_cfs))
		RETURN_IPC_INVAL;

	val = table_classifier_cfs_add(cfs, pg->path, path_cfs);

	RETURN_IPC;
}

void kis_table_classifier_cfs_remove(envid_t whom, const Skfs_table_classifier_cfs_remove_t * pg)
{
	CFS_t * cfs = (CFS_t *) pg->cfs;
	uint32_t val;

	if (!modman_name_cfs(cfs))
		RETURN_IPC_INVAL;

	val = (uint32_t) table_classifier_cfs_remove(cfs, pg->path);

	RETURN_IPC;
}

// uhfs
#include <kfs/uhfs.h>

void kis_uhfs(envid_t whom, const Skfs_uhfs_t * pg)
{
	LFS_t * lfs = (LFS_t *) pg->lfs;
	uint32_t val;

	if (!modman_name_lfs(lfs))
		RETURN_IPC_INVAL;

	val = (uint32_t) uhfs(lfs);

	RETURN_IPC;
}


//
// LFS

// journal_lfs
#include <kfs/journal_lfs.h>

void kis_journal_lfs(envid_t whom, const Skfs_journal_lfs_t * pg)
{
	LFS_t * journal = (LFS_t *) pg->journal_lfs;
	LFS_t * fs = (LFS_t *) pg->fs_lfs;
	BD_t * fs_queue = (BD_t *) pg->fs_queue_bd;
	uint32_t val;

	if (!modman_name_lfs(journal) || !modman_name_lfs(fs) || !modman_name_bd(fs_queue))
		RETURN_IPC_INVAL;

	val = (uint32_t) journal_lfs(journal, fs, fs_queue);

	RETURN_IPC;
}

void kis_journal_lfs_max_bandwidth(envid_t whom, const Skfs_journal_lfs_max_bandwidth_t * pg)
{
	LFS_t * journal = (LFS_t *) pg->journal_lfs;
	uint32_t val;

	if (!modman_name_lfs(journal))
		RETURN_IPC_INVAL;

	val = (uint32_t) journal_lfs_max_bandwidth(journal);

	RETURN_IPC;
}

// josfs_base
#include <kfs/josfs_base.h>

void kis_josfs_base(envid_t whom, const Skfs_josfs_base_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	int fsck = pg->do_fsck;
	uint32_t val;

	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) josfs(bd, &fsck);

	RETURN_IPC;
}


//
// BD

#include <kfs/loop_bd.h>
void kis_loop_bd(envid_t whom, const Skfs_loop_bd_t * pg)
{
	LFS_t * lfs = (LFS_t *) pg->lfs;
	uint32_t val;

	if (!modman_name_lfs(lfs))
		RETURN_IPC_INVAL;

	val = (uint32_t) loop_bd(lfs, pg->file);

	RETURN_IPC;
}

#include <kfs/nbd_bd.h>
void kis_nbd_bd(envid_t whom, const Skfs_nbd_bd_t * pg)
{
	uint32_t val = (uint32_t) nbd_bd(pg->address, pg->port);
	ipc_send(whom, val, NULL, 0, NULL);
}

#include <kfs/journal_queue_bd.h>
void kis_journal_queue_bd(envid_t whom, const Skfs_journal_queue_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;

	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) journal_queue_bd(bd);

	RETURN_IPC;
}

#include <kfs/order_preserver_bd.h>
void kis_order_preserver_bd(envid_t whom, const Skfs_order_preserver_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;
	
	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) order_preserver_bd(bd);

	RETURN_IPC;
}

#include <kfs/chdesc_stripper_bd.h>
void kis_chdesc_stripper_bd(envid_t whom, const Skfs_chdesc_stripper_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;
	
	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) chdesc_stripper_bd(bd);

	RETURN_IPC;
}

#include <kfs/wb_cache_bd.h>
void kis_wb_cache_bd(envid_t whom, const Skfs_wb_cache_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;
	
	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) wb_cache_bd(bd, pg->blocks);

	RETURN_IPC;
}

#include <kfs/wt_cache_bd.h>
void kis_wt_cache_bd(envid_t whom, const Skfs_wt_cache_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;
	
	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) wt_cache_bd(bd, pg->blocks);

	RETURN_IPC;
}

#include <kfs/block_resizer_bd.h>
void kis_block_resizer_bd(envid_t whom, const Skfs_block_resizer_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;
	
	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) block_resizer_bd(bd, pg->blocksize);

	RETURN_IPC;
}

#include <kfs/ide_pio_bd.h>

void kis_ide_pio_bd(envid_t whom, const Skfs_ide_pio_bd_t * pg)
{
	uint32_t val = (uint32_t) ide_pio_bd(pg->controller, pg->disk);
	ipc_send(whom, val, NULL, 0, NULL);
}

//
// kfs_ipc_serve

#define SERVE(type, function) case SKFS_##type: kis_##function(whom, pg); break

void kfs_ipc_serve_run(envid_t whom, const void * pg, int perm, uint32_t cur_cappa)
{
	int type;

	// All requests must contain an argument page
	if ((!perm & PTE_P))
	{
		fprintf(STDERR_FILENO, "Invalid serial kfs request from %08x: no argument page\n", whom);
		return; // just leave it hanging...
	}

	type = *(int*) pg;

	switch (type)
	{
		// Destructors

		SERVE(DESTROY_CFS, destroy_cfs);
		SERVE(DESTROY_LFS, destroy_lfs);
		SERVE(DESTROY_BD,  destroy_bd);

		// CFS

		SERVE(TABLE_CLASSIFIER_CFS,        table_classifier_cfs);
		SERVE(TABLE_CLASSIFIER_CFS_ADD,    table_classifier_cfs_add);
		SERVE(TABLE_CLASSIFIER_CFS_REMOVE, table_classifier_cfs_remove);

		SERVE(UHFS, uhfs);

		// LFS

		SERVE(JOURNAL_LFS,               journal_lfs);
		SERVE(JOURNAL_LFS_MAX_BANDWIDTH, journal_lfs_max_bandwidth);

		SERVE(JOSFS_BASE, josfs_base);

		// BD

		SERVE(LOOP_BD,            loop_bd);
		SERVE(NBD_BD,             nbd_bd);
		SERVE(JOURNAL_QUEUE_BD,   journal_queue_bd);
		SERVE(ORDER_PRESERVER_BD, order_preserver_bd);
		SERVE(CHDESC_STRIPPER_BD, chdesc_stripper_bd);
		SERVE(WB_CACHE_BD,        wb_cache_bd);
		SERVE(WT_CACHE_BD,        wt_cache_bd);
		SERVE(BLOCK_RESIZER_BD,   block_resizer_bd);
		SERVE(IDE_PIO_BD,         ide_pio_bd);


		default:
			fprintf(STDERR_FILENO, "kfs_ipc_serve: Unknown type %d\n", type);
			return;
	}
}
