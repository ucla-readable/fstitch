#include <inc/string.h>
#include <inc/lib.h>
#include <inc/malloc.h>

#include <inc/cfs_ipc_client.h>
#include <inc/serial_kfs.h>

#include <kfs/cfs.h>
#include <kfs/lfs.h>
#include <kfs/bd.h>

#define REQVA (0x10000000 - PGSIZE)

static uint8_t ipcpage[2*PGSIZE];

#define INIT_PG(typem, type)											\
	Skfs_##type##_t * pg = (Skfs_##type##_t *) ROUNDUP32(ipcpage, PGSIZE); \
	memset(pg, 0, PGSIZE);												\
	pg->skfs_type = SKFS_##typem

#define SEND_PG() ipc_send(fsid, SKFS_VAL, pg, PTE_P|PTE_U, NULL)
#define RECV_PG() ipc_recv(fsid, NULL, 0, NULL, NULL, 0)


//
// Destructors

static int kic_cfs_destroy(CFS_t * cfs)
{
	envid_t fsid = find_fs();
	int r;

	INIT_PG(DESTROY_CFS, destroy_cfs);

	pg->cfs = (uint32_t) cfs->instance;

	SEND_PG();
	if ((r = (int) RECV_PG()) < 0)
		return r;

	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}

static int kic_lfs_destroy(LFS_t * lfs)
{
	envid_t fsid = find_fs();
	int r;

	INIT_PG(DESTROY_LFS, destroy_lfs);

	pg->lfs = (uint32_t) lfs->instance;

	SEND_PG();
	if ((r = (int) RECV_PG()) < 0)
		return r;

	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	return 0;
}

static int kic_bd_destroy(BD_t * bd)
{
	envid_t fsid = find_fs();
	int r;

	INIT_PG(DESTROY_BD, destroy_bd);

	pg->bd = (uint32_t) bd->instance;

	SEND_PG();
	if ((r = (int) RECV_PG()) < 0)
		return r;

	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}


//
// Constructors

static CFS_t * create_cfs(uint32_t id)
{
	CFS_t * cfs;

	if (!id)
		return NULL;

	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;

	memset(cfs, 0, sizeof(*cfs));

	/* not supported by kic */
	/*
	ASSIGN(cfs, table_classifier, open);
	ASSIGN(cfs, table_classifier, close);
	ASSIGN(cfs, table_classifier, read);
	ASSIGN(cfs, table_classifier, write);
	ASSIGN(cfs, table_classifier, getdirentries);
	ASSIGN(cfs, table_classifier, truncate);
	ASSIGN(cfs, table_classifier, unlink);
	ASSIGN(cfs, table_classifier, link);
	ASSIGN(cfs, table_classifier, rename);
	ASSIGN(cfs, table_classifier, mkdir);
	ASSIGN(cfs, table_classifier, rmdir);
	ASSIGN(cfs, table_classifier, get_num_features);
	ASSIGN(cfs, table_classifier, get_feature);
	ASSIGN(cfs, table_classifier, get_metadata);
	ASSIGN(cfs, table_classifier, set_metadata);
	ASSIGN(cfs, table_classifier, sync);
	*/

	ASSIGN_DESTROY(cfs, kic_cfs, destroy);

	cfs->instance = (void *) id;

	return cfs;
}

LFS_t * create_lfs(uint32_t id)
{
	LFS_t * lfs;

	if (!id)
		return NULL;

	lfs = malloc(sizeof(*lfs));
	if (!lfs)
		return NULL;

	memset(lfs, 0, sizeof(*lfs));

	/* not supported by kic */
	/*
	ASSIGN(lfs, journal, get_blocksize);
	ASSIGN(lfs, journal, get_blockdev);
	ASSIGN(lfs, journal, allocate_block);
	ASSIGN(lfs, journal, lookup_block);
	ASSIGN(lfs, journal, lookup_name);
	ASSIGN(lfs, journal, free_fdesc);
	ASSIGN(lfs, journal, get_file_numblocks);
	ASSIGN(lfs, journal, get_file_block_num);
	ASSIGN(lfs, journal, get_file_block);
	ASSIGN(lfs, journal, get_dirent);
	ASSIGN(lfs, journal, append_file_block);
	ASSIGN(lfs, journal, allocate_name);
	ASSIGN(lfs, journal, rename);
	ASSIGN(lfs, journal, truncate_file_block);
	ASSIGN(lfs, journal, free_block);
	ASSIGN(lfs, journal, remove_name);
	ASSIGN(lfs, journal, write_block);
	ASSIGN(lfs, journal, get_num_features);
	ASSIGN(lfs, journal, get_feature);
	ASSIGN(lfs, journal, get_metadata_name);
	ASSIGN(lfs, journal, get_metadata_fdesc);
	ASSIGN(lfs, journal, set_metadata_name);
	ASSIGN(lfs, journal, set_metadata_fdesc);
	ASSIGN(lfs, journal, sync);
	*/

	ASSIGN_DESTROY(lfs, kic_lfs, destroy);

	lfs->instance = (void *) id;

	return lfs;
}

BD_t * create_bd(uint32_t id)
{
	BD_t * bd;

	if (!id)
		return NULL;

	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;

	memset(bd, 0, sizeof(*bd));

	/* not supported by kic */
	/*
	ASSIGN(bd, wt_cache_bd, get_numblocks);
	ASSIGN(bd, wt_cache_bd, get_blocksize);
	ASSIGN(bd, wt_cache_bd, get_atomicsize);
	ASSIGN(bd, wt_cache_bd, read_block);
	ASSIGN(bd, wt_cache_bd, write_block);
	ASSIGN(bd, wt_cache_bd, sync);
	*/

	ASSIGN_DESTROY(bd, kic_bd, destroy);

	bd->instance = (void *) id;

	return bd;
}


//
// CFS

#include <kfs/table_classifier_cfs.h>

CFS_t * table_classifier_cfs(void)
{
	envid_t fsid = find_fs();
	uint32_t cfs_id;

	INIT_PG(TABLE_CLASSIFIER_CFS, table_classifier_cfs);

	SEND_PG();
	cfs_id = RECV_PG();

	return create_cfs(cfs_id);
}

int table_classifier_cfs_add(CFS_t * cfs, const char * path, CFS_t * path_cfs)
{
	envid_t fsid = find_fs();

	INIT_PG(TABLE_CLASSIFIER_CFS_ADD, table_classifier_cfs_add);

	pg->cfs = (uint32_t) cfs->instance;
	pg->path_cfs = (uint32_t) path_cfs->instance;
	strncpy(pg->path, path, MIN(SKFS_MAX_NAMELEN, strlen(path)));

	SEND_PG();

	return RECV_PG();
}

CFS_t * table_classifier_cfs_remove(CFS_t * cfs, const char * path)
{
	envid_t fsid = find_fs();
	uint32_t cfs_id;

	INIT_PG(TABLE_CLASSIFIER_CFS_REMOVE, table_classifier_cfs_remove);

	pg->cfs = (uint32_t) cfs->instance;
	strncpy(pg->path, path, MIN(SKFS_MAX_NAMELEN, strlen(path)));

	SEND_PG();
	cfs_id = RECV_PG();

	return create_cfs(cfs_id);
}

#include <kfs/uhfs.h>
CFS_t * uhfs(LFS_t * lfs)
{
	envid_t fsid = find_fs();
	uint32_t cfs_id;

	INIT_PG(UHFS, uhfs);

	pg->lfs = (uint32_t) lfs->instance;

	SEND_PG();
	cfs_id = RECV_PG();

	return create_cfs(cfs_id);
}


//
// LFS

#include <kfs/journal_lfs.h>

LFS_t * journal_lfs(LFS_t * journal, LFS_t * fs, BD_t * fs_queue)
{
	envid_t fsid = find_fs();
	uint32_t lfs_id;

	INIT_PG(JOURNAL_LFS, journal_lfs);

	pg->journal_lfs = (uint32_t) journal->instance;
	pg->fs_lfs = (uint32_t) fs->instance;
	pg->fs_queue_bd = (uint32_t) fs_queue->instance;

	SEND_PG();
	lfs_id = RECV_PG();

	return create_lfs(lfs_id);
}

size_t journal_lfs_max_bandwidth(const LFS_t * journal)
{
	envid_t fsid = find_fs();

	INIT_PG(JOURNAL_LFS_MAX_BANDWIDTH, journal_lfs_max_bandwidth);

	pg->journal_lfs = (uint32_t) journal->instance;

	SEND_PG();
	return RECV_PG();
}

#include <kfs/josfs_base.h>
LFS_t * josfs(BD_t * block_device, int * do_fsck)
{
	envid_t fsid = find_fs();
	uint32_t lfs_id;

	INIT_PG(JOSFS_BASE, josfs_base);

	pg->bd = (uint32_t) block_device->instance;
	pg->do_fsck = do_fsck ? *do_fsck : 0;

	SEND_PG();
	lfs_id = RECV_PG();

	return create_lfs(lfs_id);
}


//
// BD

#include <kfs/loop_bd.h>
BD_t * loop_bd(LFS_t * lfs, const char * file)
{
	envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(LOOP_BD, loop_bd);

	pg->lfs = (uint32_t) lfs->instance;
	strncpy(pg->file, file, MIN(SKFS_MAX_NAMELEN, strlen(file)));

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/nbd_bd.h>
BD_t * nbd_bd(const char * address, uint16_t port)
{
	envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(NBD_BD, nbd_bd);

	strncpy(pg->address, address, MIN(SKFS_MAX_NAMELEN, strlen(address)));
	pg->port = port;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/journal_queue_bd.h>
BD_t * journal_queue_bd(BD_t * disk)
{
	envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(JOURNAL_QUEUE_BD, journal_queue_bd);

	pg->bd = (uint32_t) disk->instance;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/order_preserver_bd.h>
BD_t * order_preserver_bd(BD_t * disk)
{
	envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(ORDER_PRESERVER_BD, order_preserver_bd);

	pg->bd = (uint32_t) disk->instance;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/chdesc_stripper_bd.h>
BD_t * chdesc_stripper_bd(BD_t * disk)
{
	envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(CHDESC_STRIPPER_BD, chdesc_stripper_bd);

	pg->bd = (uint32_t) disk->instance;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/wb_cache_bd.h>
BD_t * wb_cache_bd(BD_t * disk, uint32_t blocks)
{
	envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(WB_CACHE_BD, wb_cache_bd);

	pg->bd = (uint32_t) disk->instance;
	pg->blocks = blocks;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/wt_cache_bd.h>
BD_t * wt_cache_bd(BD_t * disk, uint32_t blocks)
{
	envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(WT_CACHE_BD, wt_cache_bd);

	pg->bd = (uint32_t) disk->instance;
	pg->blocks = blocks;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/block_resizer_bd.h>
BD_t * block_resizer_bd(BD_t * disk, uint16_t blocksize)
{
	envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(BLOCK_RESIZER_BD, block_resizer_bd);

	pg->bd = (uint32_t) disk->instance;
	pg->blocksize = blocksize;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/ide_pio_bd.h>
BD_t * ide_pio_bd(uint8_t controller, uint8_t disk)
{
	envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(IDE_PIO_BD, ide_pio_bd);

	pg->controller = controller;
	pg->disk = disk;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}
