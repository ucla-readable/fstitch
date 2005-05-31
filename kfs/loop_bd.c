#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/lfs.h>
#include <kfs/modman.h>
#include <kfs/loop_bd.h>


#define LOOP_DEBUG 0

#if LOOP_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif


struct loop_info {
	LFS_t * lfs;
	BD_t * lfs_bd;
	uint16_t level;
	fdesc_t * file;
	const char * filename;
	uint16_t blocksize;
};
typedef struct loop_info loop_info_t;


static int loop_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct loop_info * info = (struct loop_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "filename: %s, blocksize: %d, count: %d", info->filename, info->blocksize, CALL(info->lfs, get_file_numblocks, info->file));
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "%s", info->filename);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "filename: %s, blocksize: %d", info->filename, info->blocksize);
	}
	return 0;
}

static int loop_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}
static uint32_t loop_get_numblocks(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	return CALL(info->lfs, get_file_numblocks, info->file);
}

static uint16_t loop_get_blocksize(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	return info->blocksize;
}

static uint16_t loop_get_atomicsize(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	return CALL(info->lfs_bd, get_atomicsize);
}

static bdesc_t * loop_read_block(BD_t * bd, uint32_t number)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, number);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	bdesc_t * block_lfs;
	bdesc_t * block;

	block_lfs = CALL(info->lfs, get_file_block, info->file, number * info->blocksize);
	if (!block_lfs)
		return NULL;

	block = bdesc_alloc_clone(block_lfs, number);
	if (!block)
		return NULL;
	bdesc_autorelease(block);

	return block;
}

static bdesc_t * loop_synthetic_read_block(BD_t * bd, uint32_t number, bool * synthetic)
{
	// This call goes around info->lfs to do a synthetic_read_block on the
	// info->lfs's BD. This seems simpler than adding synthetic_read_block
	// support to LFS and it seems this going-around behavior is acceptable.

	Dprintf("%s(0x%08x)\n", __FUNCTION__, number);
	loop_info_t * info = (struct loop_info_t *) OBJLOCAL(bd);
	uint32_t loop_number, lfs_number;
	bdesc_t * bdesc, * new_bdesc;

	loop_number = number;
	lfs_number = CALL(info->lfs, get_file_block_num, info->file, loop_number * info->blocksize);
	if(lfs_number == -1)
		return NULL;

	bdesc = CALL(info->lfs_bd, synthetic_read_block, lfs_number * info->blocksize, synthetic);
	if(!bdesc)
		return NULL;
	
	new_bdesc = bdesc_alloc_clone(bdesc, number);
	if(!new_bdesc)
		return NULL;
	bdesc_autorelease(new_bdesc);
	
	return new_bdesc;
}

static int loop_cancel_block(BD_t * bd, uint32_t number)
{
	// This call goes around info->lfs to do a synthetic_read_block on the
	// info->lfs's BD. This seems simpler than adding synthetic_read_block
	// support to LFS and it seems this going-around behavior is acceptable.

	Dprintf("%s(0x%08x)\n", __FUNCTION__, number);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);	
	return CALL(info->lfs_bd, cancel_block, number * info->blocksize);
}

static int loop_write_block(BD_t * bd, bdesc_t * block)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	uint32_t loop_number, lfs_number;
	bdesc_t * wblock;
	chdesc_t * head = NULL;
	chdesc_t * tail;
	int r;

	loop_number = block->number;
	lfs_number = CALL(info->lfs, get_file_block_num, info->file, loop_number * info->blocksize);
	if(lfs_number == -1)
		return -E_INVAL;

	wblock = bdesc_alloc_clone(block, lfs_number);
	if(!wblock)
		return -E_UNSPECIFIED;
	bdesc_autorelease(wblock);

	r = chdesc_push_down(bd, block, info->lfs_bd, wblock);
	if(r < 0)
		return r;

	return CALL(info->lfs, write_block, wblock, 0, wblock->ddesc->length, wblock->ddesc->data, &head, &tail);
}

static int loop_sync(BD_t * bd, bdesc_t * block)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	uint32_t loop_number, lfs_number;
	bdesc_t * wblock;

	if (!block)
		return CALL(info->lfs, sync, info->filename);

	loop_number = block->number;
	lfs_number = CALL(info->lfs, get_file_block_num, info->file, loop_number * info->blocksize);
	if(lfs_number == -1)
		return -E_INVAL;

	wblock = bdesc_alloc_clone(block, lfs_number);
	if(!wblock)
		return -E_UNSPECIFIED;
	bdesc_autorelease(wblock);

	return CALL(info->lfs_bd, sync, wblock);
}

static uint16_t loop_get_devlevel(BD_t * bd)
{
	return ((loop_info_t *) OBJLOCAL(bd))->level;
}

static int loop_destroy(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	modman_dec_lfs(info->lfs, bd);
	
	CALL(info->lfs, free_fdesc, info->file);
	free((char *) info->filename);
	free(info->file);
	memset(info, 0, sizeof(*info));
	free(info);

	memset(bd, 0, sizeof(*bd));
	free(bd);

	return 0;
}


BD_t * loop_bd(LFS_t * lfs, const char * file)
{
	Dprintf("%s(lfs 0x%08x, file \"%s\")\n", __FUNCTION__, lfs, file);
	BD_t * bd;
	loop_info_t * info;

	if (!lfs || !file)
		return NULL;

	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;

	info = malloc(sizeof(*info));
	if(!info)
		goto error_bd;
	
	BD_INIT(bd, loop, info);

	info->lfs = lfs;
	info->lfs_bd = CALL(info->lfs, get_blockdev);
	info->level = CALL(info->lfs_bd, get_devlevel);

	info->filename = strdup(file);
	if (!info->filename)
		goto error_info;

	info->file = CALL(info->lfs, lookup_name, info->filename);
	if (!info->file)
		goto error_filename;

	info->blocksize = CALL(info->lfs, get_blocksize);

	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	if(modman_inc_lfs(lfs, bd, NULL) < 0)
	{
		modman_rem_bd(bd);
		DESTROY(bd);
		return NULL;
	}

	return bd;

  error_filename:
	free((char *) info->filename);
  error_info:
	free(info);
  error_bd:
	free(bd);
	return NULL;
}
