#include <lib/types.h>
#include <malloc.h>
#include <string.h>
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

static int wholedisk_get_config(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != WHOLEDISK_MAGIC)
		return -E_INVAL;

	snprintf(string, length, "");
	return 0;
}

static int wholedisk_get_status(void * object, int level, char * string, size_t length)
{
	LFS_t * lfs = (LFS_t *) object;
	if(OBJMAGIC(lfs) != WHOLEDISK_MAGIC)
		return -E_INVAL;
	
	snprintf(string, length, "");
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

static uint32_t wholedisk_allocate_block(LFS_t * object, int purpose, chdesc_t ** head, chdesc_t ** tail)
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
	return CALL(((struct wd_info *) OBJLOCAL(object))->bd, get_numblocks);
}

static uint32_t wholedisk_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	return offset / ((struct wd_info *) OBJLOCAL(object))->blocksize;
}

static int wholedisk_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	return -E_INVAL;
}

static int wholedisk_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - size immutable */
	return -E_INVAL;
}

static fdesc_t * wholedisk_allocate_name(LFS_t * object, const char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - no filenames */
	return NULL;
}

static int wholedisk_rename(LFS_t * object, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - no filenames */
	return -E_INVAL;
}

static uint32_t wholedisk_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - size immutable */
	return INVALID_BLOCK;
}

static int wholedisk_free_block(LFS_t * object, uint32_t block, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - no block accounting */
	return -E_INVAL;
}

static int wholedisk_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	/* always fail - no filenames */
	return -E_INVAL;
}

static int wholedisk_write_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
	return CALL(((struct wd_info *) OBJLOCAL(object))->bd, write_block, block);
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
	struct wd_info * state = (struct wd_info *) OBJLOCAL(object);

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
	*tail = NULL; /* leave *head as is, this seems like acceptable behavior */
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
	CALL(((struct wd_info *) OBJLOCAL(object))->bd, sync, SYNC_FULL_DEVICE, NULL);
	return 0;
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
