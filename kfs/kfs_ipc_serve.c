#include <inc/lib.h>

#include <kfs/modman.h>
#include <inc/serial_kfs.h>
#include <kfs/kfs_ipc_serve.h>

#define KIS_DEBUG 0

#if KIS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


#define RETURN_IPC_INVAL do { val = -E_INVAL; goto exit; } while(0)
#define RETURN_IPC       exit: ipc_send(whom, (uint32_t) val, NULL, 0, NULL)


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
	Dprintf("%s = 0x%08x\n", __FUNCTION__, val);
	ipc_send(whom, val, NULL, 0, NULL);
}

void kis_table_classifier_cfs_add(envid_t whom, const Skfs_table_classifier_cfs_add_t * pg)
{
	CFS_t * cfs = (CFS_t *) pg->cfs;
	CFS_t * path_cfs = (CFS_t *) pg->path_cfs;
	uint32_t val;

	Dprintf("%s(0x%08x, %s, 0x%08x)\n", __FUNCTION__, cfs, pg->path, path_cfs);
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

// wholedisk
#include <kfs/wholedisk_lfs.h>

void kis_wholedisk(envid_t whom, const Skfs_wholedisk_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;

	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) wholedisk(bd);

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
// modman

static uint8_t ipc_page[2*PGSIZE];

#define LOOKUP_REQEST_RETURN(typel, typeu)								\
	do {																\
		Skfs_modman_return_lookup_t * rl = (Skfs_modman_return_lookup_t *) ROUNDUP32(ipc_page, PGSIZE); \
		Skfs_modman_return_lookup_user_t * ru = (Skfs_modman_return_lookup_user_t *) ROUNDUP32(ipc_page, PGSIZE); \
		const modman_entry_##typel##_t * me;							\
		int users_remaining, i;											\
																		\
		/* Check that the request object exists */						\
		me = modman_lookup_##typel((typeu##_t *) pg->id);				\
		if (!me)														\
		{																\
			ipc_send(whom, 0, NULL, 0, NULL);							\
			return;														\
		}																\
																		\
		/* Send the return_lookup page */								\
		memset(rl, 0, PGSIZE);											\
		rl->skfs_type = SKFS_MODMAN_RETURN_LOOKUP;						\
		rl->type = pg->type;											\
		rl->id = (uint32_t) me->typel;									\
		rl->usage = me->usage;											\
		if (!me->name)													\
			rl->name[0] = 0;											\
		else															\
			strncpy(rl->name, me->name, MIN(SKFS_MAX_NAMELEN, strlen(me->name))); \
		assert(vector_size(me->users) == vector_size(me->use_names));	\
		users_remaining = vector_size((vector_t *) me->users);			\
																		\
		ipc_send(whom, users_remaining, rl, PTE_P|PTE_U, NULL);			\
																		\
		/* Send a return_lookup_user page for each user */				\
		users_remaining--; /* users_remaining indicates sends to go */	\
		for (i=0; i < vector_size(me->users); i++, users_remaining--)	\
		{																\
			const void * t = (void *) vector_elt((vector_t *) me->users, i); \
			const char * use_name = (const char *) vector_elt((vector_t *) me->use_names, i); \
																		\
			const char * user_name = NULL;								\
			if (!user_name) user_name = modman_name_bd((void *) t);		\
			if (!user_name) user_name = modman_name_lfs((void *) t);	\
			if (!user_name) user_name = modman_name_cfs((void *) t);	\
																		\
			memset(ru, 0, PGSIZE);										\
			ru->skfs_type = SKFS_MODMAN_RETURN_LOOKUP_USER;				\
																		\
			if (modman_lookup_cfs((void *) t))							\
				ru->type = 0;											\
			else if (modman_lookup_lfs((void *) t))						\
				ru->type = 1;											\
			else if (modman_lookup_bd((void *) t))						\
				ru->type = 2;											\
			else														\
				assert(0);												\
																		\
			ru->id = (uint32_t) t;										\
																		\
			if (!use_name)												\
				ru->use_name[0] = 0;									\
			else														\
				strncpy(ru->use_name, use_name, MIN(SKFS_MAX_NAMELEN, strlen(use_name))); \
																		\
			ipc_send(whom, users_remaining, ru, PTE_P|PTE_U, NULL);		\
		}																\
	} while(0)

void kis_modman_request_lookup(envid_t whom, const Skfs_modman_request_lookup_t * pg)
{
	switch (pg->type)
	{
		case 0: LOOKUP_REQEST_RETURN(cfs, CFS); break;
		case 1: LOOKUP_REQEST_RETURN(lfs, LFS); break;
		case 2: LOOKUP_REQEST_RETURN(bd,  BD);  break;
		default:
			// Leave requester hanging...
			fprintf(STDERR_FILENO, "%s(): Unknown type %d\n", __FUNCTION__, pg->type);
	}
}


#define ITS_REQUEST_RETURN(typel, typeu)								\
	do {																\
		modman_it_t * it = modman_it_create_##typel();					\
		typeu##_t * t;													\
		Skfs_modman_return_it_t * ri = (Skfs_modman_return_it_t *) ROUNDUP32(ipc_page, PGSIZE);	\
		assert(it);														\
																		\
		/* Send a page for each iterator */								\
		while ((t = modman_it_next_##typel(it)))						\
		{																\
			memset(ri, 0, PGSIZE);										\
			ri->skfs_type = SKFS_MODMAN_RETURN_IT;						\
			ri->id = (uint32_t) t;										\
																		\
			ipc_send(whom, 1, ri, PTE_P|PTE_U, NULL);					\
		}																\
																		\
		/* Note end of iteration */										\
		memset(ri, 0, PGSIZE);											\
		ri->skfs_type = SKFS_MODMAN_RETURN_IT;							\
		ri->type = pg->type;											\
		ri->id = 0;														\
																		\
		ipc_send(whom, 0, ri, PTE_P|PTE_U, NULL);						\
	} while (0)
		
void kis_modman_request_its(envid_t whom, const Skfs_modman_request_its_t * pg)
{
	Dprintf("%s(): type = %d\n", __FUNCTION__, pg->type);
	switch (pg->type)
	{
		case 0: ITS_REQUEST_RETURN(cfs, CFS); break;
		case 1: ITS_REQUEST_RETURN(lfs, LFS); break;
		case 2: ITS_REQUEST_RETURN(bd,  BD);  break;
		default:
			// Leave requester hanging...
			fprintf(STDERR_FILENO, "%s(): Unknown type %d\n", __FUNCTION__, pg->type);
	}
}


//
// kfs_ipc_serve

#define SERVE(type, function) case SKFS_##type: kis_##function(whom, pg); break

void kfs_ipc_serve_run(envid_t whom, const void * pg, int perm, uint32_t cur_cappa)
{
	int type;

	// All requests must contain an argument page
	if (! ((perm & PTE_P) && (perm & PTE_U)) )
	{
		fprintf(STDERR_FILENO, "Invalid serial kfs request from %08x: no argument page\n", whom);
		return; // Just leave it hanging...
	}
	assert(pg);

	type = *(int*) pg;

	Dprintf("%s(): type = %d\n", __FUNCTION__, type);

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

		SERVE(WHOLEDISK, wholedisk);

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

		// modman

		SERVE(MODMAN_REQUEST_LOOKUP, modman_request_lookup);
		SERVE(MODMAN_REQUEST_ITS,    modman_request_its);

		default:
			fprintf(STDERR_FILENO, "kfs_ipc_serve: Unknown type %d\n", type);
			return; // Just leave hanging...
	}
}
