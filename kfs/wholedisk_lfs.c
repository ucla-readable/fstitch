#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>
#include <inc/error.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/feature.h>
#include <kfs/modman.h>
#include <kfs/wholedisk_lfs.h>

struct wd_info {
	BD_t * bd;
	uint32_t blocksize;
};

static fdesc_t fdesc;

static uint32_t wholedisk_get_blocksize(LFS_t * object)
{
	return ((struct wd_info *) object->instance)->blocksize;
}

static BD_t * wholedisk_get_blockdev(LFS_t * object)
{
	return ((struct wd_info *) object->instance)->bd;
}

static bdesc_t * wholedisk_allocate_block(LFS_t * object, uint32_t size, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
	if (head && tail)
		*head = *tail = NULL;
	/* always fail - no block accounting */
	return NULL;
}

static bdesc_t * wholedisk_lookup_block(LFS_t * object, uint32_t number, uint32_t offset, uint32_t size)
{
	return CALL(((struct wd_info *) object->instance)->bd, read_block, number);
}

static fdesc_t * wholedisk_lookup_name(LFS_t * object, const char * name)
{
	/* only allow the empty name */
	if(name[0])
		return NULL;
	return &fdesc;
}

static void wholedisk_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	/* no-op */
}

static uint32_t wholedisk_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	return CALL(((struct wd_info *) object->instance)->bd, get_numblocks);
}

static uint32_t wholedisk_get_file_block_num(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	return offset / ((struct wd_info *) object->instance)->blocksize;
}

static bdesc_t * wholedisk_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	offset /= ((struct wd_info *) object->instance)->blocksize;
	return CALL(((struct wd_info *) object->instance)->bd, read_block, offset);
}

static int wholedisk_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	return -E_INVAL;
}

static int wholedisk_append_file_block(LFS_t * object, fdesc_t * file, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	if (head && tail)
		*head = *tail = NULL;
	/* always fail - size immutable */
	return -E_INVAL;
}

static fdesc_t * wholedisk_allocate_name(LFS_t * object, const char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
	if (head && tail)
		*head = *tail = NULL;
	/* always fail - no filenames */
	return NULL;
}

static int wholedisk_rename(LFS_t * object, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
	if (head && tail)
		*head = *tail = NULL;
	/* always fail - no filenames */
	return -E_INVAL;
}

static bdesc_t * wholedisk_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	if (head && tail)
		*head = *tail = NULL;
	/* always fail - size immutable */
	return NULL;
}

static int wholedisk_free_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	if (head && tail)
		*head = *tail = NULL;
	/* always fail - no block accounting */
	return -E_INVAL;
}

static int wholedisk_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	if (head && tail)
		*head = *tail = NULL;
	/* always fail - no filenames */
	return -E_INVAL;
}

static int wholedisk_write_block(LFS_t * object, bdesc_t * block, uint32_t offset, uint32_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	struct wd_info * info = (struct wd_info *) object->instance;
	
	if (head && tail)
		*head = *tail = NULL;
	
	/* have to test all three of these because of the possibility of wrapping */
	if(offset >= info->blocksize || size > info->blocksize || offset + size > info->blocksize)
		return -E_INVAL;
	
	bdesc_touch(block);
	memcpy(&block->ddesc->data[offset], data, size);
	/* pass our ownership of this bdesc on */
	return CALL(info->bd, write_block, block);
}

static const feature_t * wholedisk_features[] = {&KFS_feature_size, &KFS_feature_filetype};

static size_t wholedisk_get_num_features(LFS_t * object, const char * name)
{
	return sizeof(wholedisk_features) / sizeof(wholedisk_features[0]);
}

static const feature_t * wholedisk_get_feature(LFS_t * object, const char * name, size_t num)
{
	if(num < 0 || num >= sizeof(wholedisk_features) / sizeof(wholedisk_features[0]))
		return NULL;
	return wholedisk_features[num];
}

static int wholedisk_get_metadata(LFS_t * object, uint32_t id, size_t * size, void ** data)
{
	struct wd_info * state = (struct wd_info *) object->instance;

	if (id == KFS_feature_size.id)
	{
		const size_t file_size = state->blocksize * CALL(state->bd, get_numblocks);
		*data = malloc(sizeof(file_size));
		if (!*data)
			return -E_NO_MEM;
		*size = sizeof(file_size);
		memcpy(*data, &file_size, sizeof(file_size));
	}
	else if (id == KFS_feature_filetype.id)
	{
		const int32_t type = TYPE_DEVICE;
		*data = malloc(sizeof(type));
		if (!*data)
			return -E_NO_MEM;
		*size = sizeof(type);
		memcpy(*data, &type, sizeof(type));
	}
	else
		return -E_INVAL;

	return 0;
}

static int wholedisk_get_metadata_name(LFS_t * object, const char * name, uint32_t id, size_t * size, void ** data)
{
	return wholedisk_get_metadata(object, id, size, data);
}

static int wholedisk_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t * size, void ** data)
{
	return wholedisk_get_metadata(object, id, size, data);
}

static int wholedisk_set_metadata(LFS_t * object, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	if (head && tail)
		*head = *tail = NULL;
	return -E_INVAL;
}

static int wholedisk_set_metadata_name(LFS_t * object, const char * name, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	return wholedisk_set_metadata(object, id, size, data, head, tail);
}

static int wholedisk_set_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
	return wholedisk_set_metadata(object, id, size, data, head, tail);
}

static int wholedisk_sync(LFS_t * object, const char * name)
{
	CALL(((struct wd_info *) object->instance)->bd, sync, NULL);
	return 0;
}

static int wholedisk_destroy(LFS_t * lfs)
{
	int r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(((struct wd_info *) lfs->instance)->bd, lfs);
	
	free(lfs->instance);
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
	lfs->instance = info;

	ASSIGN(lfs, wholedisk, get_blocksize);
	ASSIGN(lfs, wholedisk, get_blockdev);
	ASSIGN(lfs, wholedisk, allocate_block);
	ASSIGN(lfs, wholedisk, lookup_block);
	ASSIGN(lfs, wholedisk, lookup_name);
	ASSIGN(lfs, wholedisk, free_fdesc);
	ASSIGN(lfs, wholedisk, get_file_numblocks);
	ASSIGN(lfs, wholedisk, get_file_block_num);
	ASSIGN(lfs, wholedisk, get_file_block);
	ASSIGN(lfs, wholedisk, get_dirent);
	ASSIGN(lfs, wholedisk, append_file_block);
	ASSIGN(lfs, wholedisk, allocate_name);
	ASSIGN(lfs, wholedisk, rename);
	ASSIGN(lfs, wholedisk, truncate_file_block);
	ASSIGN(lfs, wholedisk, free_block);
	ASSIGN(lfs, wholedisk, remove_name);
	ASSIGN(lfs, wholedisk, write_block);
	ASSIGN(lfs, wholedisk, get_num_features);
	ASSIGN(lfs, wholedisk, get_feature);
	ASSIGN(lfs, wholedisk, get_metadata_name);
	ASSIGN(lfs, wholedisk, get_metadata_fdesc);
	ASSIGN(lfs, wholedisk, set_metadata_name);
	ASSIGN(lfs, wholedisk, set_metadata_fdesc);
	ASSIGN(lfs, wholedisk, sync);
	DESTRUCTOR(lfs, wholedisk, destroy);
	
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
