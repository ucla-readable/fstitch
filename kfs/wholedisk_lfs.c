#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/feature.h>
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

static bdesc_t * wholedisk_allocate_block(LFS_t * object, uint32_t size, int purpose)
{
	/* always fail - no free blocks */
	return NULL;
}

static bdesc_t * wholedisk_lookup_block(LFS_t * object, uint32_t number, uint32_t offset, uint32_t size)
{
	return CALL(((struct wd_info *) object->instance)->bd, read_block, number);
}

static fdesc_t * wholedisk_lookup_name(LFS_t * object, const char * name)
{
	return &fdesc;
}

static void wholedisk_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	/* no-op */
}

static bdesc_t * wholedisk_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	offset /= ((struct wd_info *) object->instance)->blocksize;
	return CALL(((struct wd_info *) object->instance)->bd, read_block, offset);
}

static int wholedisk_get_dirent(LFS_t * object, fdesc_t * file, uint32_t index, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	return -1;
}

static int wholedisk_append_file_block(LFS_t * object, fdesc_t * file, bdesc_t * block)
{
	return -1;
}

static fdesc_t * wholedisk_allocate_name(LFS_t * object, char * name, uint8_t type, fdesc_t * link)
{
	return NULL;
}

static int wholedisk_rename(LFS_t * object, const char * oldname, const char * newname)
{
	return -1;
}

static bdesc_t * wholedisk_truncate_file_block(LFS_t * object, fdesc_t * file)
{
	return NULL;
}

static int wholedisk_free_block(LFS_t * object, bdesc_t * block)
{
	return -1;
}

static int wholedisk_apply_changes(LFS_t * object, chdesc_t * changes)
{
	return -1;
}

static int wholedisk_remove_name(LFS_t * object, const char * name)
{
	return -1;
}

static int wholedisk_write_block(LFS_t * object, bdesc_t * block, uint32_t offset, uint32_t size, void * data)
{
	struct wd_info * info = (struct wd_info *) object->instance;
	int value;
	
	/* have to test all three of these because of the possibility of wrapping */
	if(offset >= info->blocksize || size >= info->blocksize || offset + size >= info->blocksize)
		return -1;
	
	bdesc_retain(&block);
	bdesc_touch(&block);
	memcpy(&block->data[offset], data, size);
	value = CALL(info->bd, write_block, block);
	bdesc_release(&block);
	
	return value;
}

static size_t wholedisk_get_num_features(LFS_t * object, const char * name)
{
	return 1;
}

static const feature_t * wholedisk_get_feature(LFS_t * object, const char * name, size_t num)
{
	if(num)
		return NULL;
	return &KFS_feature_size;
}

static int wholedisk_get_metadata(LFS_t * object, const char * name, uint32_t id, size_t * size, void ** data)
{
	return -1;
}

static int wholedisk_set_metadata(LFS_t * object, const char * name, uint32_t id, size_t size, const void * data)
{
	return -1;
}

static int wholedisk_sync(LFS_t * object, const char * name)
{
	CALL(((struct wd_info *) object->instance)->bd, sync, NULL);
	return 0;
}

static int wholedisk_destroy(LFS_t * lfs)
{
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
	ASSIGN(lfs, wholedisk, allocate_block);
	ASSIGN(lfs, wholedisk, lookup_block);
	ASSIGN(lfs, wholedisk, lookup_name);
	ASSIGN(lfs, wholedisk, free_fdesc);
	ASSIGN(lfs, wholedisk, get_file_block);
	ASSIGN(lfs, wholedisk, get_dirent);
	ASSIGN(lfs, wholedisk, append_file_block);
	ASSIGN(lfs, wholedisk, allocate_name);
	ASSIGN(lfs, wholedisk, rename);
	ASSIGN(lfs, wholedisk, truncate_file_block);
	ASSIGN(lfs, wholedisk, free_block);
	ASSIGN(lfs, wholedisk, apply_changes);
	ASSIGN(lfs, wholedisk, remove_name);
	ASSIGN(lfs, wholedisk, write_block);
	ASSIGN(lfs, wholedisk, get_num_features);
	ASSIGN(lfs, wholedisk, get_feature);
	ASSIGN(lfs, wholedisk, get_metadata);
	ASSIGN(lfs, wholedisk, set_metadata);
	ASSIGN(lfs, wholedisk, sync);
	ASSIGN_DESTROY(lfs, wholedisk, destroy);
	
	info->bd = bd;
	info->blocksize = CALL(bd, get_blocksize);
	
	return lfs;
}
