#include <inc/string.h>
#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/vector.h>
#include <inc/hash_map.h>

#include <inc/cfs_ipc_client.h>
#include <inc/serial_kfs.h>

#include <kfs/cfs.h>
#include <kfs/lfs.h>
#include <kfs/bd.h>

#define KIC_DEBUG 0

#if KIC_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

static uint8_t ipcpage[2*PGSIZE];

#define INIT_PG(typem, type)											\
	Skfs_##type##_t * pg = (Skfs_##type##_t *) ROUNDUP32(ipcpage, PGSIZE); \
	memset(pg, 0, PGSIZE);												\
	pg->skfs_type = SKFS_##typem

#define SEND_PG() ipc_send(fsid, SKFS_VAL, pg, PTE_P|PTE_U, NULL)
#define RECV_PG() ipc_recv(fsid, NULL, 0, NULL, NULL, 0)

//
// Object id management

typedef struct {
	uint32_t id;
	void * ptr;
} obj_entry_t;

static hash_map_t * objs = NULL; // hash_map_t of obj_entry_t

int ensure_objs_exists()
{
	if (objs)
		return 0;

	objs = hash_map_create();
	if (objs)
		return 0;
	else
		return -E_NO_MEM;
}

void * get_obj_ptr(uint32_t id)
{
	obj_entry_t * oe;
	int r;
	if ((r = ensure_objs_exists()) < 0)
		panic("%s(): ensure_objs_exists: %e", __FUNCTION__, r); // TODO: handle error

	oe = hash_map_find_val(objs, (void *) id);
	if (!oe)
		return NULL;
	return oe->ptr;
}

void add_obj(uint32_t id, void * ptr)
{
	obj_entry_t * oe;
	int r;
	if ((r = ensure_objs_exists()) < 0)
		panic("%s(): ensure_objs_exists: %e", __FUNCTION__, r); // TODO: handle error

	oe = malloc(sizeof(*oe));
	assert(oe); // TODO: handle error
	oe->id  = id;
	oe->ptr = ptr;

	r = hash_map_insert(objs, (void *) id, oe);
	assert(r >= 0); // TODO: handle error
}

void delete_obj(uint32_t id)
{
	obj_entry_t * oe;
	int r;
	if ((r = ensure_objs_exists()) < 0)
		panic("%s(): ensure_objs_exists: %e", __FUNCTION__, r); // TODO: handle error

	oe = hash_map_erase(objs, (void *) id);
	if (!oe)
		return;
	memset(oe, 0, sizeof(*oe));
	free(oe);
}


//
// Destructors

static int kic_cfs_destroy(CFS_t * cfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, cfs);
	const envid_t fsid = find_fs();
	const uint32_t id = (uint32_t) cfs->instance;
	int r;

	INIT_PG(DESTROY_CFS, destroy_cfs);

	pg->cfs = id;

	SEND_PG();
	if ((r = (int) RECV_PG()) < 0)
		return r;

	delete_obj(id);

	memset(cfs, 0, sizeof(*cfs));
	free(cfs);
	return 0;
}

static int kic_lfs_destroy(LFS_t * lfs)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, lfs);
	const envid_t fsid = find_fs();
	const uint32_t id = (uint32_t) lfs->instance;
	int r;

	INIT_PG(DESTROY_LFS, destroy_lfs);

	pg->lfs = id;

	SEND_PG();
	if ((r = (int) RECV_PG()) < 0)
		return r;

	delete_obj(id);

	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	return 0;
}

static int kic_bd_destroy(BD_t * bd)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, bd);
	const envid_t fsid = find_fs();
	const uint32_t id = (uint32_t) bd->instance;
	int r;

	INIT_PG(DESTROY_BD, destroy_bd);

	pg->bd = id;

	SEND_PG();
	if ((r = (int) RECV_PG()) < 0)
		return r;

	delete_obj(id);

	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}


//
// Constructors

static CFS_t * create_cfs(uint32_t id)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, id);
	CFS_t * cfs;

	if (!id)
		return NULL;

	cfs = get_obj_ptr(id);
	if (cfs)
	{
		assert((uint32_t) cfs->instance == id);
		return cfs;
	}

	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;
	Dprintf("new 0x%08x\n", cfs);

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
	add_obj(id, cfs);

	return cfs;
}

LFS_t * create_lfs(uint32_t id)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, id);
	LFS_t * lfs;

	if (!id)
		return NULL;

	lfs = get_obj_ptr(id);
	if (lfs)
	{
		assert((uint32_t) lfs->instance == id);
		return lfs;
	}

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
	add_obj(id, lfs);

	return lfs;
}

BD_t * create_bd(uint32_t id)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, id);
	BD_t * bd;

	if (!id)
		return NULL;

	bd = get_obj_ptr(id);
	if (bd)
	{
		assert((uint32_t) bd->instance == id);
		return bd;
	}

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
	add_obj(id, bd);

	return bd;
}


//
// CFS

#include <kfs/table_classifier_cfs.h>

CFS_t * table_classifier_cfs(void)
{
	const envid_t fsid = find_fs();
	uint32_t cfs_id;

	INIT_PG(TABLE_CLASSIFIER_CFS, table_classifier_cfs);

	SEND_PG();
	cfs_id = RECV_PG();

	return create_cfs(cfs_id);
}

int table_classifier_cfs_add(CFS_t * cfs, const char * path, CFS_t * path_cfs)
{
	const envid_t fsid = find_fs();

	INIT_PG(TABLE_CLASSIFIER_CFS_ADD, table_classifier_cfs_add);

	pg->cfs = (uint32_t) cfs->instance;
	pg->path_cfs = (uint32_t) path_cfs->instance;
	strncpy(pg->path, path, MIN(SKFS_MAX_NAMELEN, strlen(path)));

	SEND_PG();

	return RECV_PG();
}

CFS_t * table_classifier_cfs_remove(CFS_t * cfs, const char * path)
{
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();

	INIT_PG(JOURNAL_LFS_MAX_BANDWIDTH, journal_lfs_max_bandwidth);

	pg->journal_lfs = (uint32_t) journal->instance;

	SEND_PG();
	return RECV_PG();
}

#include <kfs/josfs_base.h>
LFS_t * josfs(BD_t * block_device, int * do_fsck)
{
	const envid_t fsid = find_fs();
	uint32_t lfs_id;

	INIT_PG(JOSFS_BASE, josfs_base);

	pg->bd = (uint32_t) block_device->instance;
	pg->do_fsck = do_fsck ? *do_fsck : 0;

	SEND_PG();
	lfs_id = RECV_PG();

	return create_lfs(lfs_id);
}

#include <kfs/wholedisk_lfs.h>
LFS_t * wholedisk(BD_t * bd)
{
	const envid_t fsid = find_fs();
	uint32_t lfs_id;

	INIT_PG(WHOLEDISK, wholedisk);

	pg->bd = (uint32_t) bd->instance;

	SEND_PG();
	lfs_id = RECV_PG();

	return create_lfs(lfs_id);
}


//
// BD

#include <kfs/loop_bd.h>
BD_t * loop_bd(LFS_t * lfs, const char * file)
{
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();
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
	const envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(IDE_PIO_BD, ide_pio_bd);

	pg->controller = controller;
	pg->disk = disk;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}


//
// modman
//
// Supported: lookup, name, it_create, it_next, and it_destroy.
// Not supported: init, add, add_anon, inc, dec, and rem.

#include <kfs/modman.h>

uint8_t ipc_recv_page[2*PGSIZE];

// FIXME: Memory leak:
// In kfsd one does not free a modman_entry_t* when done with it.
// The way MODMAN_LOOKUP() is implemented, the user *does* need to, and
// so modman_lookup_*() outside of kfsd leaks this memory.

#define MODMAN_LOOKUP(typel, typeu, skfs_modman_type)					\
const modman_entry_##typel##_t * modman_lookup_##typel(typeu##_t * t)	\
{																		\
	Dprintf("%s(0x%08x, id 0x%08x)\n", __FUNCTION__, t, t->instance);	\
	const envid_t fsid = find_fs();										\
	Skfs_modman_return_lookup_t * lookup = (Skfs_modman_return_lookup_t *) ROUNDUP32(ipc_recv_page, PGSIZE); \
	Skfs_modman_return_lookup_user_t * lookup_user = (Skfs_modman_return_lookup_user_t *) ROUNDUP32(ipc_recv_page, PGSIZE);	\
	int perm;															\
	int users_remaining, ur;											\
	modman_entry_##typel##_t * me;										\
	int r;																\
																		\
	/* setup the modman_entry */										\
	me = malloc(sizeof(*me));											\
	if (!me)															\
		return NULL;													\
	me->users = vector_create();										\
	if (!me->users)														\
		goto error_me;													\
	me->use_names = vector_create();									\
	if (!me->use_names)													\
		goto error_users;												\
																		\
	/* request the lookup */											\
	INIT_PG(MODMAN_REQUEST_LOOKUP, modman_request_lookup);				\
	pg->type = skfs_modman_type;										\
	pg->id = (uint32_t) t->instance;									\
	SEND_PG();															\
																		\
	/* receive the lookup page */										\
	users_remaining = (int) ipc_recv(fsid, NULL, lookup, &perm, NULL, 0); \
	if (!perm)															\
		goto error_use_names;											\
	me->typel = create_##typel(lookup->id);								\
	*(int *) &me->usage = lookup->usage; /* '*(int*) &' to work around const member */ \
	me->name = strdup(lookup->name);									\
	Dprintf("%s(): looked up \"%s\", %d users, %d ur\n", __FUNCTION__, me->name, me->usage, users_remaining); \
																		\
	/* receive a lookup_user page for each user */						\
	while (users_remaining--)											\
	{																	\
		ur = ipc_recv(fsid, NULL, lookup_user, &perm, NULL, 0);			\
		assert(ur == users_remaining);									\
																		\
		void * ut;														\
		switch (lookup_user->type)										\
		{																\
			case 0: ut = create_cfs(lookup_user->id); break;			\
			case 1: ut = create_lfs(lookup_user->id); break;			\
			case 2: ut = create_bd (lookup_user->id); break;			\
			default: assert(0);											\
		}																\
		if (!ut)														\
			goto error_use_names;										\
		r = vector_push_back((vector_t *) me->users, ut);				\
		if (r < 0)														\
			goto error_use_names;										\
																		\
		r = vector_push_back((vector_t *) me->use_names, strdup(lookup_user->use_name)); \
		if (r < 0)														\
			goto error_use_names;										\
		Dprintf("%s(): added user %s, %d users_remaining\n", __FUNCTION__, lookup_user->use_name, users_remaining); \
	}																	\
																		\
	return me;															\
																		\
  error_use_names:														\
	panic("TODO: free each name in the vector me->use_names");			\
	vector_destroy((vector_t *) me->use_names);							\
  error_users:															\
	panic("TODO: destroy each t in the vector me->users? (what if they already existed?"); \
	vector_destroy((vector_t *) me->users);								\
  error_me:																\
	free(me);															\
	return NULL;														\
}

MODMAN_LOOKUP(cfs, CFS, 0);
MODMAN_LOOKUP(lfs, LFS, 1);
MODMAN_LOOKUP(bd,  BD,  2);


#define MODMAN_NAME(typel, typeu)									\
const char * modman_name_##typel(typeu##_t * t)						\
{																	\
	const modman_entry_##typel##_t * me = modman_lookup_##typel(t);	\
	if (!me)														\
		return NULL;												\
	return me->name;												\
}

MODMAN_NAME(cfs, CFS);
MODMAN_NAME(lfs, LFS);
MODMAN_NAME(bd,  BD);


// modman iterators

struct modman_it {
	vector_t * v; // vector_t of uint32_t ids
	size_t next;
};


modman_it_t * kic_modman_it_create(void)
{
	modman_it_t * it = malloc(sizeof(*it));
	if (!it)
		return NULL;
	it->v = vector_create();
	if (!it->v)
	{
		free(it);
		return NULL;
	}
	it->next = 0;
	return it;
}

void kic_modman_it_destroy(modman_it_t * it)
{
	vector_destroy(it->v);
	memset(it, 0, sizeof(*it));
	free(it);
}


#define MODMAN_IT_CREATE(typel, typeu, skfs_modman_type)				\
modman_it_t * modman_it_create_##typel(void)							\
{																		\
	const envid_t fsid = find_fs();										\
	uint32_t perm;														\
	uint32_t more_its;													\
	Skfs_modman_return_it_t * rit = (Skfs_modman_return_it_t *) ROUNDUP32(ipc_recv_page, PGSIZE); \
	modman_it_t * it;													\
	int r;																\
																		\
	it = kic_modman_it_create();										\
	if (!it)															\
		return NULL;													\
																		\
	/* request the iterators */											\
	INIT_PG(MODMAN_REQUEST_ITS, modman_request_its);					\
	pg->type = skfs_modman_type;										\
																		\
	SEND_PG();															\
																		\
	/* receive a page for each iterator */								\
	while ((more_its = ipc_recv(fsid, NULL, rit, &perm, NULL, 0)))		\
	{																	\
		r = vector_push_back(it->v, (void *) rit->id);					\
		assert(r >= 0); /* TODO: handle error */						\
	}																	\
																		\
	return it;															\
}

MODMAN_IT_CREATE(cfs, CFS, 0);
MODMAN_IT_CREATE(lfs, LFS, 1);
MODMAN_IT_CREATE(bd,  BD,  2);


void modman_it_destroy(modman_it_t * it)
{
	// TODO: it would be nice to free any CFS/LFS/BD objects created
	// during iteration that are no longer in use.
	kic_modman_it_destroy(it);
}


#define MODMAN_IT_NEXT(typel, typeu)									\
typeu##_t * modman_it_next_##typel(modman_it_t * it)					\
{																		\
	if (it->next >= vector_size(it->v))									\
		return NULL;													\
	return create_##typel((uint32_t) vector_elt(it->v, it->next++));	\
}

MODMAN_IT_NEXT(cfs, CFS);
MODMAN_IT_NEXT(lfs, LFS);
MODMAN_IT_NEXT(bd,  BD);
