#include <inc/lib.h>

#include <kfs/modman.h>
#include <inc/serial_kfs.h>
#include <kfs/kfs_ipc_serve.h>


//
// destructors

void kis_destroy_cfs(envid_t whom, Skfs_destroy_cfs_t * pg)
{
	CFS_t * cfs;
	int val;

	cfs = (CFS_t *) pg->cfs;
	if (!modman_query_cfs(cfs))
	{
		val = -E_INVAL;
		goto exit;
	}

	val = DESTROY(cfs);

  exit:
	ipc_send(whom, (uint32_t) val, NULL, 0, NULL);
}

void kis_destroy_lfs(envid_t whom, Skfs_destroy_lfs_t * pg)
{
	LFS_t * lfs;
	int val;

	lfs = (LFS_t *) pg->lfs;
	if (!modman_query_lfs(lfs))
	{
		val = -E_INVAL;
		goto exit;
	}

	val = DESTROY(lfs);

  exit:
	ipc_send(whom, (uint32_t) val, NULL, 0, NULL);
}

void kis_destroy_bd(envid_t whom, Skfs_destroy_bd_t * pg)
{
	BD_t * bd;
	int val;

	bd = (BD_t *) pg->bd;
	if (!modman_query_bd(bd))
	{
		val = -E_INVAL;
		goto exit;
	}

	val = DESTROY(bd);

  exit:
	ipc_send(whom, (uint32_t) val, NULL, 0, NULL);
}


//
// table_classifier_cfs

#include <kfs/table_classifier_cfs.h>

void kis_table_classifier_cfs(envid_t whom, Skfs_table_classifier_cfs_t * pg)
{
	uint32_t val;
	val = (uint32_t) table_classifier_cfs();
	ipc_send(whom, val, NULL, 0, NULL);
}

void kis_table_classifier_cfs_add(envid_t whom, Skfs_table_classifier_cfs_add_t * pg)
{
	CFS_t * cfs;
	CFS_t * path_cfs;
	uint32_t val;

	cfs = (CFS_t *) pg->cfs;
	path_cfs = (CFS_t *) pg->path_cfs;
	if (!modman_query_cfs(cfs) || !modman_query_cfs(path_cfs))
	{
		val = -E_INVAL;
		goto exit;
	}

	// TODO: check cfs pointer
	val = table_classifier_cfs_add(cfs, pg->path, path_cfs);

  exit:
	ipc_send(whom, val, NULL, 0, NULL);
}

void kis_table_classifier_cfs_remove(envid_t whom, Skfs_table_classifier_cfs_remove_t * pg)
{
	CFS_t * cfs;
	uint32_t val;

	cfs = (CFS_t *) pg->cfs;
	if (!modman_query_cfs(cfs))
	{
		val = -E_INVAL;
		goto exit;
	}

	val = (uint32_t) table_classifier_cfs_remove(cfs, pg->path);

  exit:
	ipc_send(whom, val, NULL, 0, NULL);
}


//
// kfs_ipc_serve

#define SERVE(type, function) case SKFS_##type: kis_##function(whom, pg); break

void kfs_ipc_serve_run(envid_t whom, void * pg, int perm, uint32_t cur_cappa)
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
		SERVE(DESTROY_CFS, destroy_cfs);
		SERVE(DESTROY_LFS, destroy_lfs);
		SERVE(DESTROY_BD,  destroy_bd);

		SERVE(TABLE_CLASSIFIER_CFS,        table_classifier_cfs);
		SERVE(TABLE_CLASSIFIER_CFS_ADD,    table_classifier_cfs_add);
		SERVE(TABLE_CLASSIFIER_CFS_REMOVE, table_classifier_cfs_remove);

		default:
			fprintf(STDERR_FILENO, "kfs_ipc_serve: Unknown type %d\n", type);
			return;
	}
}
