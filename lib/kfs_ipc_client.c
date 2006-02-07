#include <string.h>
#include <stdlib.h>
#include <inc/lib.h>
#include <lib/vector.h>
#include <lib/hash_map.h>

#include <inc/cfs_ipc_client.h>
#include <lib/serial_kfs.h>
#include <inc/kfs_ipc_client.h>

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
static uint8_t ipc_recv_page[2*PGSIZE];

#define INIT_PG(typem, type)												\
	Skfs_##type##_t * pg = (Skfs_##type##_t *) ROUNDUP32(ipcpage, PGSIZE);	\
	memset(pg, 0, PGSIZE);													\
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
	const uint32_t id = (uint32_t) OBJLOCAL(cfs);
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
	const uint32_t id = (uint32_t) OBJLOCAL(lfs);
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
	const uint32_t id = (uint32_t) OBJLOCAL(bd);
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
// OBJ

static int kic_get_config_status(bool config_status, object_t * obj, int level, char * string, size_t length)
{
	const envid_t fsid = find_fs();
	Skfs_return_config_status_t * rcs = (Skfs_return_config_status_t *) ROUNDUP32(ipc_recv_page, PGSIZE);
	int perm, r;

	INIT_PG(REQUEST_CONFIG_STATUS, request_config_status);

	pg->id = (uint32_t) OBJLOCAL(obj);
	pg->level = level;
	pg->config_status = config_status;

	SEND_PG();
	r = (int) ipc_recv(fsid, NULL, rcs, &perm, NULL, 0);

	if (r >= 0)
	{
		strncpy(string, rcs->string, MIN(length, strlen(rcs->string)));
		string[MIN(length, strlen(rcs->string))] = 0;
	}

	return r;
}

static int kic_get_config(void * obj, int level, char * string, size_t length)
{
	return kic_get_config_status(0, obj, level, string, length);
}

static int kic_get_status(void * obj, int level, char * string, size_t length)
{
	return kic_get_config_status(1, obj, level, string, length);
}

static int kic_get_flags_magic(object_t * obj)
{
	const envid_t fsid = find_fs();
	Skfs_return_flags_magic_t * rfm = (Skfs_return_flags_magic_t *) ROUNDUP32(ipc_recv_page, PGSIZE);
	int perm, r;

	INIT_PG(REQUEST_FLAGS_MAGIC, request_flags_magic);

	pg->id = (uint32_t) OBJLOCAL(obj);
	if (!pg->id)
		assert(0);

	SEND_PG();
	r = (int) ipc_recv(fsid, NULL, rfm, &perm, NULL, 0);
	if (r < 0)
		return r;

	OBJFLAGS(obj) = rfm->flags;
	OBJMAGIC(obj) = rfm->magic;

	return 0;
}


//
// Constructors

CFS_t * create_cfs(uint32_t id)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, id);
	CFS_t * cfs;
	int r;

	if (!id)
		return NULL;

	cfs = get_obj_ptr(id);
	if (cfs)
	{
		assert((uint32_t) OBJLOCAL(cfs) == id);
		return cfs;
	}

	cfs = malloc(sizeof(*cfs));
	if (!cfs)
		return NULL;
	Dprintf("new 0x%08x\n", cfs);

	memset(cfs, 0, sizeof(*cfs));

	OBJLOCAL(cfs) = (void *) id;
	r = kic_get_flags_magic((object_t *) cfs);
	assert(r >= 0); // TODO: handle error
	OBJASSIGN(cfs, kic, get_config);
	OBJASSIGN(cfs, kic, get_status);
	DESTRUCTOR(cfs, kic_cfs, destroy);

	add_obj(id, cfs);

	return cfs;
}

LFS_t * create_lfs(uint32_t id)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, id);
	LFS_t * lfs;
	int r;

	if (!id)
		return NULL;

	lfs = get_obj_ptr(id);
	if (lfs)
	{
		assert((uint32_t) OBJLOCAL(lfs) == id);
		return lfs;
	}

	lfs = malloc(sizeof(*lfs));
	if (!lfs)
		return NULL;

	memset(lfs, 0, sizeof(*lfs));

	OBJLOCAL(lfs) = (void *) id;
	r = kic_get_flags_magic((object_t *) lfs);
	assert(r >= 0); // TODO: handle error
	OBJASSIGN(lfs, kic, get_config);
	OBJASSIGN(lfs, kic, get_status);
	DESTRUCTOR(lfs, kic_lfs, destroy);

	add_obj(id, lfs);

	return lfs;
}

BD_t * create_bd(uint32_t id)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, id);
	BD_t * bd;
	int r;

	if (!id)
		return NULL;

	bd = get_obj_ptr(id);
	if (bd)
	{
		assert((uint32_t) OBJLOCAL(bd) == id);
		return bd;
	}

	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;

	memset(bd, 0, sizeof(*bd));

	OBJLOCAL(bd) = (void *) id;
	r = kic_get_flags_magic((object_t *) bd);
	assert(r >= 0); // TODO: handle error
	OBJASSIGN(bd, kic, get_config);
	OBJASSIGN(bd, kic, get_status);
	DESTRUCTOR(bd, kic_bd, destroy);

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
	const int path_len = strlen(path)+1;

	if (path_len > SKFS_MAX_NAMELEN)
	{
		Dprintf("%s(): filename \"%s\" is too long for serial kfs (%u > %u)\n", __FUNCTION__, path, path_len, SKFS_MAX_NAMELEN);
		return -E_BAD_PATH;
	}

	INIT_PG(TABLE_CLASSIFIER_CFS_ADD, table_classifier_cfs_add);

	pg->cfs = (uint32_t) OBJLOCAL(cfs);
	pg->path_cfs = (uint32_t) OBJLOCAL(path_cfs);
	strncpy(pg->path, path, path_len);

	SEND_PG();

	return RECV_PG();
}

CFS_t * table_classifier_cfs_remove(CFS_t * cfs, const char * path)
{
	const envid_t fsid = find_fs();
	const int path_len = strlen(path)+1;
	uint32_t cfs_id;

	if (path_len > SKFS_MAX_NAMELEN)
	{
		Dprintf("%s(): filename \"%s\" is too long for serial kfs (%u > %u)\n", __FUNCTION__, path, path_len, SKFS_MAX_NAMELEN);
		return NULL;
	}

	INIT_PG(TABLE_CLASSIFIER_CFS_REMOVE, table_classifier_cfs_remove);

	pg->cfs = (uint32_t) OBJLOCAL(cfs);
	strncpy(pg->path, path, path_len);

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

	pg->lfs = (uint32_t) OBJLOCAL(lfs);

	SEND_PG();
	cfs_id = RECV_PG();

	return create_cfs(cfs_id);
}


//
// LFS

#include <kfs/josfs_base.h>

LFS_t * josfs(BD_t * block_device)
{
	const envid_t fsid = find_fs();
	uint32_t lfs_id;

	INIT_PG(JOSFS_BASE, josfs_base);

	pg->bd = (uint32_t) OBJLOCAL(block_device);

	SEND_PG();
	lfs_id = RECV_PG();

	return create_lfs(lfs_id);
}

int josfs_fsck(LFS_t * lfs)
{
	const envid_t fsid = find_fs();

	INIT_PG(JOSFS_FSCK, josfs_fsck);

	pg->lfs = (uint32_t) OBJLOCAL(lfs);

	SEND_PG();
	return RECV_PG();
}

#include <kfs/wholedisk_lfs.h>
LFS_t * wholedisk(BD_t * bd)
{
	const envid_t fsid = find_fs();
	uint32_t lfs_id;

	INIT_PG(WHOLEDISK, wholedisk);

	pg->bd = (uint32_t) OBJLOCAL(bd);

	SEND_PG();
	lfs_id = RECV_PG();

	return create_lfs(lfs_id);
}


//
// BD

#include <kfs/loop_bd.h>
BD_t * loop_bd(LFS_t * lfs, inode_t inum)
{
	const envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(LOOP_BD, loop_bd);

	pg->lfs = (uint32_t) OBJLOCAL(lfs);
	pg->inum = inum;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/mem_bd.h>
BD_t * mem_bd(uint32_t blocks, uint16_t blocksize)
{
	const envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(MEM_BD, mem_bd);

	pg->blocks = blocks;
	pg->blocksize = blocksize;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/nbd_bd.h>
BD_t * nbd_bd(const char * address, uint16_t port)
{
	const envid_t fsid = find_fs();
	const int address_len = strlen(address)+1;
	uint32_t bd_id;

	if (address_len > SKFS_MAX_NAMELEN)
	{
		Dprintf("%s(): address \"%s\" is too long for serial kfs (%u > %u)\n", __FUNCTION__, address, address_len, SKFS_MAX_NAMELEN);
		return NULL;
	}

	INIT_PG(NBD_BD, nbd_bd);

	strncpy(pg->address, address, address_len);
	pg->port = port;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/journal_bd.h>
BD_t * journal_bd(BD_t * disk)
{
	const envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(JOURNAL_BD, journal_bd);

	pg->bd = (uint32_t) OBJLOCAL(disk);

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

int journal_bd_set_journal(BD_t * bd, BD_t * journal)
{
	const envid_t fsid = find_fs();

	INIT_PG(JOURNAL_BD_SET_JOURNAL, journal_bd_set_journal);

	pg->bd = (uint32_t) OBJLOCAL(bd);
	pg->journal = journal ? (uint32_t) OBJLOCAL(journal) : 0;

	SEND_PG();

	return RECV_PG();
}

#include <kfs/wb_cache_bd.h>
BD_t * wb_cache_bd(BD_t * disk, uint32_t blocks)
{
	const envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(WB_CACHE_BD, wb_cache_bd);

	pg->bd = (uint32_t) OBJLOCAL(disk);
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

	pg->bd = (uint32_t) OBJLOCAL(disk);
	pg->blocks = blocks;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/elevator_cache_bd.h>
BD_t * elevator_cache_bd(BD_t * disk, uint32_t blocks)
{
	const envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(ELEVATOR_CACHE_BD, elevator_cache_bd);

	pg->bd = (uint32_t) OBJLOCAL(disk);
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

	pg->bd = (uint32_t) OBJLOCAL(disk);
	pg->blocksize = blocksize;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/md_bd.h>
BD_t * md_bd(BD_t * disk0, BD_t * disk1)
{
	const envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(MD_BD, md_bd);

	pg->disk0 = (uint32_t) OBJLOCAL(disk0);
	pg->disk1 = (uint32_t) OBJLOCAL(disk1);

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

#include <kfs/mirror_bd.h>

BD_t * mirror_bd(BD_t * disk0, BD_t * disk1, uint8_t stride)
{
	const envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(MIRROR_BD, mirror_bd);

	pg->disk0 = (uint32_t) OBJLOCAL(disk0);
	pg->disk1 = (uint32_t) OBJLOCAL(disk1);
	pg->stride = stride;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}

int mirror_bd_add_device(BD_t * bd, BD_t * newdevice)
{
	const envid_t fsid = find_fs();

	INIT_PG(MIRROR_BD_ADD, mirror_bd_add);

	pg->bd = (uint32_t) OBJLOCAL(bd);
	pg->newdevice = (uint32_t) OBJLOCAL(newdevice);

	SEND_PG();
	return RECV_PG();
}

int mirror_bd_remove_device(BD_t * bd, int diskno)
{
	const envid_t fsid = find_fs();

	INIT_PG(MIRROR_BD_REMOVE, mirror_bd_remove);

	pg->bd = (uint32_t) OBJLOCAL(bd);
	pg->diskno = diskno;

	SEND_PG();
	return RECV_PG();
}

#include <kfs/ide_pio_bd.h>
BD_t * ide_pio_bd(uint8_t controller, uint8_t disk, uint8_t readahead)
{
	const envid_t fsid = find_fs();
	uint32_t bd_id;

	INIT_PG(IDE_PIO_BD, ide_pio_bd);

	pg->controller = controller;
	pg->disk = disk;
	pg->readahead = readahead;

	SEND_PG();
	bd_id = RECV_PG();

	return create_bd(bd_id);
}


//
// modman
//
// Supported: lookup, name, it_init, it_next, and it_destroy.
// Not supported: init, add, add_anon, inc, dec, and rem.

#include <kfs/modman.h>

// FIXME: Memory leak:
// In kfsd one does not free a modman_entry_t* when done with it.
// The way MODMAN_LOOKUP() is implemented, the user *does* need to, and
// so modman_lookup_*() outside of kfsd leaks this memory.

#define MODMAN_LOOKUP(typel, typeu, skfs_modman_type)					\
const modman_entry_##typel##_t * modman_lookup_##typel(typeu##_t * t)	\
{																		\
	Dprintf("%s(0x%08x, id 0x%08x)\n", __FUNCTION__, t, OBJLOCAL(t));	\
	const envid_t fsid = find_fs();										\
	Skfs_modman_return_lookup_t * lookup = (Skfs_modman_return_lookup_t *) ROUNDUP32(ipc_recv_page, PGSIZE); \
	Skfs_modman_return_lookup_user_t * lookup_user = (Skfs_modman_return_lookup_user_t *) ROUNDUP32(ipc_recv_page, PGSIZE);	\
	int perm;															\
	int users_remaining, ur;											\
	vector_t * recved_lookup_users;										\
	modman_entry_##typel##_t * me;										\
	int i, r;															\
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
	recved_lookup_users = vector_create();								\
	if (!recved_lookup_users)											\
		goto error_use_names;											\
																		\
	/* request the lookup */											\
	INIT_PG(MODMAN_REQUEST_LOOKUP, modman_request_lookup);				\
	pg->type = skfs_modman_type;										\
	pg->id = (uint32_t) OBJLOCAL(t);									\
	SEND_PG();															\
																		\
	/* receive the lookup page */										\
	users_remaining = (int) ipc_recv(fsid, NULL, lookup, &perm, NULL, 0); \
	if (!perm)															\
		goto error_recved_lookup_users;									\
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
		vector_push_back(recved_lookup_users, memdup(lookup_user, sizeof(*lookup_user))); \
		assert(vector_elt_end(recved_lookup_users));					\
																		\
		r = vector_push_back((vector_t *) me->use_names, strdup(lookup_user->use_name)); \
		if (r < 0)														\
			panic("vector_push_back() failed\n");						\
																		\
		Dprintf("%s(): added user %s, %d users_remaining\n", __FUNCTION__, lookup_user->use_name, users_remaining); \
	}																	\
																		\
	/* create objects, which can't be done above because create_*()	*/	\
	/* may need to talk to kfsd */										\
	for (i=0; i < vector_size(recved_lookup_users);  i++)				\
	{																	\
		void * ut;														\
		lookup_user = vector_elt(recved_lookup_users, i);				\
		switch (lookup_user->type)										\
		{																\
			case 0: ut = create_cfs(lookup_user->id); break;			\
			case 1: ut = create_lfs(lookup_user->id); break;			\
			case 2: ut = create_bd (lookup_user->id); break;			\
			default: assert(0);											\
		}																\
		if (!ut)														\
			panic("create_*() failed\n");								\
		r = vector_push_back((vector_t *) me->users, ut);				\
		if (r < 0)														\
			panic("vector_push_back() failed\n");						\
																		\
		free(lookup_user);												\
	}																	\
	vector_destroy(recved_lookup_users);								\
																		\
	return me;															\
																		\
  error_recved_lookup_users:											\
	panic("TODO: free each entry");										\
	vector_destroy(recved_lookup_users);								\
  error_use_names:														\
	vector_destroy((vector_t *) me->use_names);							\
  error_users:															\
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


static int kic_modman_it_init(modman_it_t * it)
{
	it->v = vector_create();
	if (!it->v)
		return -E_NO_MEM;
	it->next = 0;
	return 0;
}

static void kic_modman_it_destroy(modman_it_t * it)
{
	vector_destroy(it->v);
	memset(it, 0, sizeof(*it));
}


#define MODMAN_IT_INIT(typel, typeu, skfs_modman_type)					\
int modman_it_init_##typel(modman_it_t * it)							\
{																		\
	const envid_t fsid = find_fs();										\
	uint32_t perm;														\
	uint32_t more_its;													\
	Skfs_modman_return_it_t * rit = (Skfs_modman_return_it_t *) ROUNDUP32(ipc_recv_page, PGSIZE); \
	int r;																\
																		\
	r = kic_modman_it_init(it);											\
	if (r < 0)															\
		return r;														\
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
	return 0;															\
}

MODMAN_IT_INIT(cfs, CFS, 0);
MODMAN_IT_INIT(lfs, LFS, 1);
MODMAN_IT_INIT(bd,  BD,  2);


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


// sync

int kfs_sync(const char * name)
{
	const envid_t fsid = find_fs();
	const int name_len = name ? strlen(name) + 1 : 0;

	if (name_len > SKFS_MAX_NAMELEN)
	{
		Dprintf("%s(): filename \"%s\" is too long for serial kfs (%u > %u)\n", __FUNCTION__, name, name_len, SKFS_MAX_NAMELEN);
		return -E_BAD_PATH;
	}

	INIT_PG(SYNC, sync);

	if (name)
		strncpy(pg->name, name, sizeof(pg->name));
	else
		pg->name[0] = 0;

	SEND_PG();
	return RECV_PG();
}


//
// Perf testing

int perf_test(int cfs_bd, const char * file, int size)
{
	const envid_t fsid = find_fs();
	const int file_len = strlen(file) + 1;

	if (file_len > SKFS_MAX_NAMELEN)
	{
		Dprintf("%s(): filename \"%s\" is too long for serial kfs (%u > %u)\n", __FUNCTION__, file, file_len, SKFS_MAX_NAMELEN);
		return -E_BAD_PATH;
	}

	INIT_PG(PERF_TEST, perf_test);

	pg->cfs_bd = cfs_bd;
	pg->size = size;
	strncpy(pg->file, file, sizeof(pg->file));

	SEND_PG();
	return RECV_PG();
}
