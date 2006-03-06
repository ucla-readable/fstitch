#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <inc/error.h>
#include <lib/types.h>
#include <lib/stdio.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/feature.h>
#include <kfs/modman.h>
#include <kfs/wholedisk_lfs.h>

#define INODE_ROOT ((inode_t) 1)
#define INODE_DISK ((inode_t) 2)

#define DISK_NAME "disk"

struct wd_info {
	BD_t * bd;
	uint32_t blocksize;
};

struct wd_fdesc {
	fdesc_common_t * common;
	fdesc_common_t base;
};
typedef struct wd_fdesc wd_fdesc_t;

union fdesc_cast {
	wd_fdesc_t wd;
	fdesc_t cast;
};

/* stupid compiler warnings... must use unions to evade */
static union fdesc_cast root_fdesc = {wd: {common: &root_fdesc.wd.base, base: {parent: INODE_NONE}}};
static union fdesc_cast disk_fdesc = {wd: {common: &disk_fdesc.wd.base, base: {parent: INODE_ROOT}}};

static int wholedisk_get_config(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != WHOLEDISK_MAGIC)
		return -E_INVAL;

	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int wholedisk_get_status(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != WHOLEDISK_MAGIC)
		return -E_INVAL;
	
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int wholedisk_get_root(LFS_t * lfs, inode_t * ino)
{
	*ino = INODE_ROOT;
	return 0;
}

static uint32_t wholedisk_get_blocksize(LFS_t * object)
{
	return ((struct wd_info *) OBJLOCAL(object))->blocksize;
}

static BD_t * wholedisk_get_blockdev(LFS_t * object)
{
	return ((struct wd_info *) OBJLOCAL(object))->bd;
}

static uint32_t wholedisk_allocate_block(LFS_t * object, fdesc_t * file, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - no block accounting */
	return INVALID_BLOCK;
}

static bdesc_t * wholedisk_lookup_block(LFS_t * object, uint32_t number)
{
	return CALL(((struct wd_info *) OBJLOCAL(object))->bd, read_block, number);
}

static bdesc_t * wholedisk_synthetic_lookup_block(LFS_t * object, uint32_t number, bool * synthetic)
{
	return CALL(((struct wd_info *) OBJLOCAL(object))->bd, synthetic_read_block, number, synthetic);
}

static int wholedisk_cancel_synthetic_block(LFS_t * object, uint32_t number)
{
	return CALL(((struct wd_info *) OBJLOCAL(object))->bd, cancel_block, number);
}

static fdesc_t * wholedisk_lookup_inode(LFS_t * object, inode_t inode)
{
	if(inode == INODE_ROOT)
		return (fdesc_t *) &root_fdesc;
	if(inode == INODE_DISK)
		return (fdesc_t *) &disk_fdesc;
	return NULL;
}

static int wholedisk_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * inode)
{
	/* only allow the fixed disk name */
	if(parent != INODE_ROOT || strcmp(name, DISK_NAME))
		return -E_NOT_FOUND;
	if(inode)
		*inode = INODE_DISK;
	return 0;
}

static void wholedisk_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	/* no-op */
}

static uint32_t wholedisk_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	if(file != (fdesc_t *) &disk_fdesc)
		return INVALID_BLOCK;
	return CALL(((struct wd_info *) OBJLOCAL(object))->bd, get_numblocks);
}

static uint32_t wholedisk_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	if(file != (fdesc_t *) &disk_fdesc)
		return INVALID_BLOCK;
	return offset / ((struct wd_info *) OBJLOCAL(object))->blocksize;
}

static int wholedisk_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	struct wd_info * state = (struct wd_info *) OBJLOCAL(object);
	const char * name;
	size_t namelen;
	
	if(file != (fdesc_t *) &root_fdesc)
		return -E_NOT_DIR;
	
	if(size < sizeof(*entry) - sizeof(entry->d_name))
		return -E_INVAL;
	
	switch(*basep)
	{
		case 0:
			/* . */
			entry->d_type = TYPE_DIR;
			entry->d_fileno = INODE_ROOT;
			entry->d_namelen = 1;
			entry->d_filesize = 0;
			name = ".";
			break;
		case 1:
			/* .. */
			entry->d_type = TYPE_DIR;
			entry->d_fileno = INODE_ROOT;
			entry->d_namelen = 2;
			entry->d_filesize = 0;
			name = "..";
			break;
		case 2:
			/* disk */
			entry->d_type = TYPE_DEVICE;
			entry->d_fileno = INODE_DISK;
			namelen = strlen(DISK_NAME);
			/* do not make DISK_NAME longer than DIRENT_MAXNAMELEN...
			 * strncpy() is defined to pad the remaining space in the
			 * destination with nulls which we can't do here! */
			assert(namelen < DIRENT_MAXNAMELEN);
			entry->d_namelen = namelen;
			entry->d_filesize = state->blocksize * CALL(state->bd, get_numblocks);
			name = DISK_NAME;
			break;
		default:
			return -E_EOF;
	}
	
	entry->d_reclen = sizeof(*entry) - sizeof(entry->d_name) + entry->d_namelen + 1;
	if(entry->d_reclen > size)
	{
		memset(entry, 0, size);
		return -E_INVAL;
	}
	strcpy(entry->d_name, name);
	entry->d_name[entry->d_namelen] = 0;
	
	*basep += 1;
	
	return 0;
}

static int wholedisk_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - size immutable */
	return -E_INVAL;
}

static fdesc_t * wholedisk_allocate_name(LFS_t * object, inode_t parent, const char * name, uint8_t type, fdesc_t * link, inode_t * newino, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - no filenames */
	return NULL;
}

static int wholedisk_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - no filenames */
	return -E_PERM;
}

static uint32_t wholedisk_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - size immutable */
	return INVALID_BLOCK;
}

static int wholedisk_free_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - no block accounting */
	return -E_INVAL;
}

static int wholedisk_remove_name(LFS_t * object, inode_t parent, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - no filenames */
	return -E_PERM;
}

static int wholedisk_write_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	return CALL(((struct wd_info *) OBJLOCAL(object))->bd, write_block, block);
}

static const feature_t * wholedisk_features[] = {&KFS_feature_size, &KFS_feature_filetype, &KFS_feature_freespace, &KFS_feature_file_lfs, &KFS_feature_blocksize, &KFS_feature_devicesize};

static size_t wholedisk_get_num_features(LFS_t * object, inode_t inode)
{
	return sizeof(wholedisk_features) / sizeof(wholedisk_features[0]);
}

static const feature_t * wholedisk_get_feature(LFS_t * object, inode_t inode, size_t num)
{
	if(num < 0 || num >= sizeof(wholedisk_features) / sizeof(wholedisk_features[0]))
		return NULL;
	return wholedisk_features[num];
}

static int wholedisk_get_metadata_inode(LFS_t * object, inode_t inode, uint32_t id, size_t * size, void ** data)
{
	struct wd_info * state = (struct wd_info *) OBJLOCAL(object);

	if (id == KFS_feature_size.id)
	{
		const size_t file_size = (inode == INODE_DISK) ? state->blocksize * CALL(state->bd, get_numblocks) : 0;
		*data = malloc(sizeof(file_size));
		if (!*data)
			return -E_NO_MEM;
		*size = sizeof(file_size);
		memcpy(*data, &file_size, sizeof(file_size));
	}
	else if (id == KFS_feature_filetype.id)
	{
		const int32_t type = (inode == INODE_DISK) ? TYPE_DEVICE : TYPE_DIR;
		*data = malloc(sizeof(type));
		if (!*data)
			return -E_NO_MEM;
		*size = sizeof(type);
		memcpy(*data, &type, sizeof(type));
	}
	else if (id == KFS_feature_freespace.id)
	{
		uint32_t free_space;
		*data = malloc(sizeof(free_space));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(free_space);
		free_space = 0;
		memcpy(*data, &free_space, sizeof(free_space));
	}
	else if (id == KFS_feature_file_lfs.id)
	{
		*data = malloc(sizeof(object));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(object);
		memcpy(*data, &object, sizeof(object));
	}
	else if (id == KFS_feature_blocksize.id)
	{
		uint32_t blocksize = CALL(state->bd, get_blocksize);
		*data = malloc(sizeof(blocksize));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(blocksize);
		memcpy(*data, &blocksize, sizeof(blocksize));
	}
	else if (id == KFS_feature_devicesize.id)
	{
		uint32_t devicesize = CALL(state->bd, get_numblocks);
		*data = malloc(sizeof(devicesize));
		if (!*data)
			return -E_NO_MEM;

		*size = sizeof(devicesize);
		memcpy(*data, &devicesize, sizeof(devicesize));
	}
	else
		return -E_INVAL;

	return 0;
}

static int wholedisk_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t * size, void ** data)
{
	inode_t inode = INODE_NONE;
	if(file == (fdesc_t *) &root_fdesc)
		inode = INODE_ROOT;
	else if(file == (fdesc_t *) &disk_fdesc)
		inode = INODE_DISK;
	return wholedisk_get_metadata_inode(object, inode, id, size, data);
}

static int wholedisk_set_metadata(LFS_t * object, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	return -E_INVAL;
}

static int wholedisk_set_metadata_inode(LFS_t * object, inode_t inode, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	return wholedisk_set_metadata(object, id, size, data, head, tail);
}

static int wholedisk_set_metadata_fdesc(LFS_t * object, fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	return wholedisk_set_metadata(object, id, size, data, head, tail);
}

static int wholedisk_destroy(LFS_t * lfs)
{
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(((struct wd_info *) OBJLOCAL(lfs))->bd, lfs);
	
	free(OBJLOCAL(lfs));
	memset(lfs, 0, sizeof(*lfs));
	free(lfs);
	
	return 0;
}

LFS_t * wholedisk(BD_t * bd)
{
	struct wd_info * info;
	LFS_t * lfs = malloc(sizeof(*lfs));

	if(!lfs)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(lfs);
		return NULL;
	}

	LFS_INIT(lfs, wholedisk, info);
	OBJMAGIC(lfs) = WHOLEDISK_MAGIC;
	
	info->bd = bd;
	info->blocksize = CALL(bd, get_blocksize);
	
	if(modman_add_anon_lfs(lfs, __FUNCTION__))
	{
		DESTROY(lfs);
		return NULL;
	}
	if(modman_inc_bd(bd, lfs, NULL) < 0)
	{
		modman_rem_lfs(lfs);
		DESTROY(lfs);
		return NULL;
	}
	
	return lfs;
}
