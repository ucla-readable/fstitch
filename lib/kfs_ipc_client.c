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


//
// destructors

static int kic_cfs_destroy(CFS_t * cfs)
{
	envid_t fsid = find_fs();
	int r;

	Skfs_destroy_cfs_t * pg = (Skfs_destroy_cfs_t *) ROUNDUP32(ipcpage, PGSIZE);
	memset(pg, 0, PGSIZE);

	pg->skfs_type = SKFS_DESTROY_CFS;
	pg->cfs = (uint32_t) cfs->instance;

	ipc_send(fsid, SKFS_VAL, pg, PTE_P|PTE_U, NULL);
	if ((r = (int) ipc_recv(fsid, NULL, 0, NULL, NULL, 0)) < 0)
		return r;

	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}

static int kic_lfs_destroy(LFS_t * lfs)
{
}

static int kic_bd_destroy(BD_t * bd)
{
}


//
// constructors

static CFS_t * create_cfs(uint32_t x)
{
	CFS_t * cfs;

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

	cfs->instance = (void *) x;

	return cfs;
}


//
// table_classifier_cfs

#include <kfs/table_classifier_cfs.h>

CFS_t * table_classifier_cfs(void)
{
	envid_t fsid = find_fs();
	uint32_t kis_cfs;

	Skfs_table_classifier_cfs_t * pg = (Skfs_table_classifier_cfs_t *) ROUNDUP32(ipcpage, PGSIZE);
	memset(pg, 0, PGSIZE);

	pg->skfs_type = SKFS_TABLE_CLASSIFIER_CFS;

	ipc_send(fsid, SKFS_VAL, pg, PTE_P|PTE_U, NULL);
	kis_cfs = ipc_recv(fsid, NULL, 0, NULL, NULL, 0);

	return create_cfs(kis_cfs);
}

int table_classifier_cfs_add(CFS_t * cfs, const char * path, CFS_t * path_cfs)
{
	envid_t fsid = find_fs();

	Skfs_table_classifier_cfs_add_t * pg = (Skfs_table_classifier_cfs_add_t *) ROUNDUP32(ipcpage, PGSIZE);
	memset(pg, 0, PGSIZE);

	pg->skfs_type = SKFS_TABLE_CLASSIFIER_CFS_ADD;
	pg->cfs = (uint32_t) cfs->instance;
	pg->path_cfs = (uint32_t) path_cfs->instance;
	strncpy(pg->path, path, MIN(SKFS_MAX_NAMELEN, strlen(path)));

	ipc_send(fsid, SKFS_VAL, pg, PTE_P|PTE_U, NULL);

	return ipc_recv(fsid, NULL, 0, NULL, NULL, 0);
}

CFS_t * table_classifier_cfs_remove(CFS_t * cfs, const char * path)
{
	envid_t fsid = find_fs();
	uint32_t kis_cfs;

	Skfs_table_classifier_cfs_add_t * pg = (Skfs_table_classifier_cfs_add_t *) ROUNDUP32(ipcpage, PGSIZE);
	memset(pg, 0, PGSIZE);

	pg->skfs_type = SKFS_TABLE_CLASSIFIER_CFS_REMOVE;
	pg->cfs = (uint32_t) cfs;
	strncpy(pg->path, path, MIN(SKFS_MAX_NAMELEN, strlen(path)));

	ipc_send(fsid, SKFS_VAL, pg, PTE_P|PTE_U, NULL);
	kis_cfs = ipc_recv(fsid, NULL, 0, NULL, NULL, 0);

	return create_cfs(kis_cfs);
}
