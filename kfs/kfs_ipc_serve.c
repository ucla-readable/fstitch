#include <inc/lib.h>

#include <kfs/modman.h>
#include <inc/serial_kfs.h>
#include <kfs/kfs_ipc_serve.h>

#define USE_THIRD_LEG 1

#define KIS_DEBUG 0

#if KIS_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


#define RETURN_IPC_INVAL do { val = -E_INVAL; goto exit; } while(0)
#define RETURN_IPC       exit: ipc_send(whom, (uint32_t) val, NULL, 0, NULL)

static uint8_t ipc_page[2*PGSIZE];


//
// Destructors

static void kis_destroy_cfs(envid_t whom, const Skfs_destroy_cfs_t * pg)
{
	CFS_t * cfs = (CFS_t *) pg->cfs;
	int val;

	if (!modman_name_cfs(cfs))
		RETURN_IPC_INVAL;

	val = DESTROY(cfs);

	RETURN_IPC;
}

static void kis_destroy_lfs(envid_t whom, const Skfs_destroy_lfs_t * pg)
{
	LFS_t * lfs = (LFS_t *) pg->lfs;
	int val;

	if (!modman_name_lfs(lfs))
		RETURN_IPC_INVAL;

	val = DESTROY(lfs);

	RETURN_IPC;
}

static void kis_destroy_bd(envid_t whom, const Skfs_destroy_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	int val;

	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = DESTROY(bd);

	RETURN_IPC;
}


//
// OBJ

static void kis_request_flags_magic(envid_t whom,  const Skfs_request_flags_magic_t * pg)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, pg->id);
	Skfs_return_flags_magic_t * rfm = (Skfs_return_flags_magic_t *) ROUNDUP32(ipc_page, PGSIZE);

	if (!modman_name_cfs((CFS_t *) pg->id)
		&& !modman_name_lfs((LFS_t *) pg->id)
		&& !modman_name_bd((BD_t *) pg->id))
		ipc_send(whom, 0, NULL, 0, NULL);

	rfm->id = pg->id;
	rfm->flags = OBJFLAGS((object_t *) pg->id);
	rfm->magic = OBJMAGIC((object_t *) pg->id);

	ipc_send(whom, 0, rfm, PTE_P|PTE_U, NULL);
}

static void kis_request_config_status(envid_t whom, const Skfs_request_config_status_t * pg)
{
	Dprintf("%s(0x%08x, %d, %d)\n", __FUNCTION__, pg->id, pg->level, pg->config_status);
	Skfs_return_config_status_t * rcs = (Skfs_return_config_status_t *) ROUNDUP32(ipc_page, PGSIZE);
	int r;

	if (!modman_name_cfs((CFS_t *) pg->id)
		&& !modman_name_lfs((LFS_t *) pg->id)
		&& !modman_name_bd((BD_t *) pg->id))
		ipc_send(whom, 0, NULL, 0, NULL);

	rcs->id = pg->id;
	rcs->level = pg->level;
	rcs->config_status = pg->config_status;
	if (pg->config_status == 0)
		r = OBJCALL((object_t *) pg->id, get_config, pg->level, rcs->string, sizeof(rcs->string));
	else if (pg->config_status == 1)
		r = OBJCALL((object_t *) pg->id, get_status, pg->level, rcs->string, sizeof(rcs->string));
	else
		r = -E_INVAL;

	ipc_send(whom, r, rcs, PTE_P|PTE_U, NULL);
}


//
// CFS

// table_classifier_cfs
#include <kfs/table_classifier_cfs.h>

static void kis_table_classifier_cfs(envid_t whom, const Skfs_table_classifier_cfs_t * pg)
{
	uint32_t val = (uint32_t) table_classifier_cfs();
	Dprintf("%s = 0x%08x\n", __FUNCTION__, val);
	ipc_send(whom, val, NULL, 0, NULL);
}

static void kis_table_classifier_cfs_add(envid_t whom, const Skfs_table_classifier_cfs_add_t * pg)
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

static void kis_table_classifier_cfs_remove(envid_t whom, const Skfs_table_classifier_cfs_remove_t * pg)
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

static void kis_uhfs(envid_t whom, const Skfs_uhfs_t * pg)
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
#if !USE_THIRD_LEG
// journal_lfs
#include <kfs/journal_lfs.h>

static void kis_journal_lfs(envid_t whom, const Skfs_journal_lfs_t * pg)
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

static void kis_journal_lfs_max_bandwidth(envid_t whom, const Skfs_journal_lfs_max_bandwidth_t * pg)
{
	LFS_t * journal = (LFS_t *) pg->journal_lfs;
	uint32_t val;

	if (!modman_name_lfs(journal))
		RETURN_IPC_INVAL;

	val = (uint32_t) journal_lfs_max_bandwidth(journal);

	RETURN_IPC;
}
#endif
// josfs_base
#include <kfs/josfs_base.h>

static void kis_josfs_base(envid_t whom, const Skfs_josfs_base_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;

	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) josfs(bd);

	RETURN_IPC;
}

static void kis_josfs_fsck(envid_t whom, const Skfs_josfs_fsck_t * pg)
{
	LFS_t * lfs = (LFS_t *) pg->lfs;
	int32_t val;

	if (!modman_name_lfs(lfs))
		RETURN_IPC_INVAL;

	val = josfs_fsck(lfs);

	RETURN_IPC;
}

// wholedisk
#include <kfs/wholedisk_lfs.h>

static void kis_wholedisk(envid_t whom, const Skfs_wholedisk_t * pg)
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
static void kis_loop_bd(envid_t whom, const Skfs_loop_bd_t * pg)
{
	LFS_t * lfs = (LFS_t *) pg->lfs;
	uint32_t val;

	if (!modman_name_lfs(lfs))
		RETURN_IPC_INVAL;

	val = (uint32_t) loop_bd(lfs, pg->file);

	RETURN_IPC;
}

#include <kfs/nbd_bd.h>
static void kis_nbd_bd(envid_t whom, const Skfs_nbd_bd_t * pg)
{
	uint32_t val = (uint32_t) nbd_bd(pg->address, pg->port);
	ipc_send(whom, val, NULL, 0, NULL);
}
#include <kfs/mem_bd.h>
static void kis_mem_bd(envid_t whom, const Skfs_mem_bd_t * pg)
{
	uint32_t val = (uint32_t) mem_bd(pg->blocks, pg->blocksize);
	ipc_send(whom, val, NULL, 0, NULL);
}
#if !USE_THIRD_LEG
#include <kfs/journal_queue_bd.h>
static void kis_journal_queue_bd(envid_t whom, const Skfs_journal_queue_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;

	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) journal_queue_bd(bd);

	RETURN_IPC;
}

#include <kfs/order_preserver_bd.h>
static void kis_order_preserver_bd(envid_t whom, const Skfs_order_preserver_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;
	
	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) order_preserver_bd(bd);

	RETURN_IPC;
}

#include <kfs/wb_cache_bd.h>
static void kis_wb_cache_bd(envid_t whom, const Skfs_wb_cache_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;
	
	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) wb_cache_bd(bd, pg->blocks);

	RETURN_IPC;
}
#endif
#include <kfs/wt_cache_bd.h>
static void kis_wt_cache_bd(envid_t whom, const Skfs_wt_cache_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;
	
	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) wt_cache_bd(bd, pg->blocks);

	RETURN_IPC;
}

#include <kfs/block_resizer_bd.h>
static void kis_block_resizer_bd(envid_t whom, const Skfs_block_resizer_bd_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	uint32_t val;
	
	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) block_resizer_bd(bd, pg->blocksize);

	RETURN_IPC;
}
#if !USE_THIRD_LEG
#include <kfs/md_bd.h>
static void kis_md_bd(envid_t whom, const Skfs_md_bd_t * pg)
{
	BD_t * disk0 = (BD_t *) pg->disk0;
	BD_t * disk1 = (BD_t *) pg->disk1;
	uint32_t val;
	
	if (!modman_name_bd(disk0) || !modman_name_bd(disk1))
		RETURN_IPC_INVAL;

	val = (uint32_t) md_bd(disk0, disk1);

	RETURN_IPC;
}

#include <kfs/mirror_bd.h>
static void kis_mirror_bd(envid_t whom, const Skfs_mirror_bd_t * pg)
{
	BD_t * disk0 = (BD_t *) pg->disk0;
	BD_t * disk1 = (BD_t *) pg->disk1;
	uint32_t val;
	
	if (!modman_name_bd(disk0) || !modman_name_bd(disk1))
		RETURN_IPC_INVAL;

	val = (uint32_t) mirror_bd(disk0, disk1, pg->stride);

	RETURN_IPC;
}

static void kis_mirror_bd_add(envid_t whom, const Skfs_mirror_bd_add_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	BD_t * newdevice = (BD_t *) pg->newdevice;
	uint32_t val;
	
	if (!modman_name_bd(bd) || !modman_name_bd(newdevice))
		RETURN_IPC_INVAL;

	val = (uint32_t) mirror_bd_add_device(bd, newdevice);

	RETURN_IPC;
}

static void kis_mirror_bd_remove(envid_t whom, const Skfs_mirror_bd_remove_t * pg)
{
	BD_t * bd = (BD_t *) pg->bd;
	int diskno = pg->diskno;
	uint32_t val;
	
	if (!modman_name_bd(bd))
		RETURN_IPC_INVAL;

	val = (uint32_t) mirror_bd_remove_device(bd, diskno);

	RETURN_IPC;
}
#endif
#include <kfs/ide_pio_bd.h>
static void kis_ide_pio_bd(envid_t whom, const Skfs_ide_pio_bd_t * pg)
{
	uint32_t val = (uint32_t) ide_pio_bd(pg->controller, pg->disk, pg->readahead);
	ipc_send(whom, val, NULL, 0, NULL);
}


//
// modman

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
		{																\
			int len = strlen(me->name);									\
			if (len+1 > SKFS_MAX_NAMELEN)								\
			{															\
				len = SKFS_MAX_NAMELEN;									\
				fprintf(STDERR_FILENO, "%s(): serial kfs support limiting name \"%s\" to %u chars\n", __FUNCTION__, me->name, len); \
			}															\
			strncpy(rl->name, me->name, len);							\
			rl->name[len] = 0;											\
		}																\
																		\
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
			{															\
				int len = strlen(use_name);								\
				if (len+1 > SKFS_MAX_NAMELEN)							\
				{														\
					len = SKFS_MAX_NAMELEN;								\
					fprintf(STDERR_FILENO, "%s(): serial kfs support limiting use name \"%s\" to %u chars\n", __FUNCTION__, use_name, len); \
				}														\
				strncpy(ru->use_name, use_name, len);					\
				ru->use_name[len] = 0;									\
			}															\
																		\
			ipc_send(whom, users_remaining, ru, PTE_P|PTE_U, NULL);		\
		}																\
	} while(0)

static void kis_modman_request_lookup(envid_t whom, const Skfs_modman_request_lookup_t * pg)
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
		modman_it_t it;													\
		typeu##_t * t;													\
		Skfs_modman_return_it_t * ri = (Skfs_modman_return_it_t *) ROUNDUP32(ipc_page, PGSIZE);	\
		int r = modman_it_init_##typel(&it);							\
		assert(r >= 0);													\
																		\
		/* Send a page for each iterator */								\
		while ((t = modman_it_next_##typel(&it)))						\
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
		modman_it_destroy(&it);											\
	} while (0)
		
static void kis_modman_request_its(envid_t whom, const Skfs_modman_request_its_t * pg)
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
// Perf testing

static char test_data[4096];
int perf_test_cfs(const Skfs_perf_test_t * pg)
{
	modman_it_t it;
	CFS_t * cfs;
	int fid;
	int time_start, time_end;
	int s, size, r;

	r = modman_it_init_cfs(&it);
	assert(r >= 0);
	while ((cfs = modman_it_next_cfs(&it)))
		if (!strncmp("table_classifier_cfs-", modman_name_cfs(cfs), strlen("table_classifier_cfs-")))
			break;
	modman_it_destroy(&it);
	assert(cfs);

	fid = CALL(cfs, open, pg->file, O_CREAT|O_WRONLY);
	if(fid < 0)
	{
		fprintf(STDERR_FILENO, "%s(): open %s: %e\n", __FUNCTION__, pg->file, fid);
		return fid;
	}

	time_start = env->env_jiffies;
	for(size = 0; size + sizeof(test_data) < pg->size; )
	{
		s = CALL(cfs, write, fid, test_data, size, sizeof(test_data));
		if (s < 0)
		{
			fprintf(STDERR_FILENO, "%s(): write: %e\n", __FUNCTION__, s);
			CALL(cfs, close, fid);
			return s;
		}
		size += s;
	}
	time_end = env->env_jiffies;

	r = CALL(cfs, close, fid);
	if (r < 0)
		fprintf(STDERR_FILENO, "%s(): CALL(cfs, close): %e\n", __FUNCTION__, r);

	return time_end - time_start;
}

static void kis_perf_test(envid_t whom, const Skfs_perf_test_t * pg)
{
	int val;

	if (pg->cfs_bd == 0)
		val = perf_test_cfs(pg);
	else
		val = -E_INVAL;

	ipc_send(whom, val, NULL, 0, NULL);
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

		// OBJ

		SERVE(REQUEST_FLAGS_MAGIC, request_flags_magic);
		SERVE(REQUEST_CONFIG_STATUS, request_config_status);

		// CFS

		SERVE(TABLE_CLASSIFIER_CFS,        table_classifier_cfs);
		SERVE(TABLE_CLASSIFIER_CFS_ADD,    table_classifier_cfs_add);
		SERVE(TABLE_CLASSIFIER_CFS_REMOVE, table_classifier_cfs_remove);
		SERVE(UHFS, uhfs);

		// LFS
#if !USE_THIRD_LEG
		SERVE(JOURNAL_LFS,               journal_lfs);
		SERVE(JOURNAL_LFS_MAX_BANDWIDTH, journal_lfs_max_bandwidth);
#endif

		SERVE(JOSFS_BASE, josfs_base);
		SERVE(JOSFS_FSCK, josfs_fsck);

		SERVE(WHOLEDISK, wholedisk);

		// BD

		SERVE(LOOP_BD,            loop_bd);
		SERVE(NBD_BD,             nbd_bd);
		SERVE(MEM_BD,             mem_bd);
#if !USE_THIRD_LEG
		SERVE(JOURNAL_QUEUE_BD,   journal_queue_bd);
		SERVE(ORDER_PRESERVER_BD, order_preserver_bd);
		SERVE(WB_CACHE_BD,        wb_cache_bd);
#endif
		SERVE(WT_CACHE_BD,        wt_cache_bd);
		SERVE(BLOCK_RESIZER_BD,   block_resizer_bd);
#if !USE_THIRD_LEG
		SERVE(MD_BD,              md_bd);
		SERVE(MIRROR_BD,          mirror_bd);
		SERVE(MIRROR_BD_ADD,          mirror_bd_add);
		SERVE(MIRROR_BD_REMOVE,          mirror_bd_remove);
#endif
		SERVE(IDE_PIO_BD,         ide_pio_bd);

		// modman

		SERVE(MODMAN_REQUEST_LOOKUP, modman_request_lookup);
		SERVE(MODMAN_REQUEST_ITS,    modman_request_its);

		SERVE(PERF_TEST, perf_test);

		default:
			fprintf(STDERR_FILENO, "kfs_ipc_serve: Unknown type %d\n", type);
			return; // Just leave hanging...
	}
}
