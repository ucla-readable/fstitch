#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
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
	fdesc_t * file;
	const char * filename;
	uint16_t blocksize;
};
typedef struct loop_info loop_info_t;


static int loop_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct loop_info * info = (struct loop_info *) bd->instance;
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
	loop_info_t * info = (loop_info_t *) bd->instance;
	return CALL(info->lfs, get_file_numblocks, info->file);
}

static uint16_t loop_get_blocksize(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_info_t * info = (loop_info_t *) bd->instance;
	return CALL(info->lfs, get_blocksize);
}

static uint16_t loop_get_atomicsize(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_info_t * info = (loop_info_t *) bd->instance;
	BD_t * lfs_bd = CALL(info->lfs, get_blockdev);
	return CALL(lfs_bd, get_atomicsize);
}

static bdesc_t * loop_read_block(BD_t * bd, uint32_t number)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, number);
	loop_info_t * info = (loop_info_t *) bd->instance;
	bdesc_t * bdesc;

	bdesc = CALL(info->lfs, get_file_block, info->file, number * info->blocksize);
	if (!bdesc)
		return NULL;

	// adjust bdesc to match this bd
	if (bdesc_alter(&bdesc) < 0)
	{
		bdesc_drop(&bdesc);
		return NULL;
	}
	bdesc->bd = bd;
	bdesc->number = number;

	return bdesc;
}

static int loop_write_block(BD_t * bd, bdesc_t * block)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block);
	loop_info_t * info = (loop_info_t *) bd->instance;
	uint32_t refs, loop_number, lfs_number;
	chdesc_t * head = NULL;
	chdesc_t * tail;
	int r;

	if(block->bd != bd)
		return -E_INVAL;

	loop_number = block->number;
	lfs_number = CALL(info->lfs, get_file_block_num, info->file, loop_number * info->blocksize);
	if(lfs_number == -1)
		return -E_INVAL;

	refs = block->refs;
	block->translated++;
	block->bd = CALL(info->lfs, get_blockdev);
	block->number = lfs_number;

	r =  CALL(info->lfs, write_block, block, block->offset, block->length, block->ddesc->data, &head, &tail);

	if (refs)
	{
		block->bd = bd;
		block->number = loop_number;
		block->translated--;
	}

	return r;
}

static int loop_sync(BD_t * bd, bdesc_t * block)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block);
	loop_info_t * info = (loop_info_t *) bd->instance;
	BD_t * lfs_bd = CALL(info->lfs, get_blockdev);
	uint32_t refs;
	int r;

	if (!block)
		return CALL(info->lfs, sync, info->filename);

	assert(block->bd == bd);

	refs = block->refs;
	block->translated++;
	block->bd = lfs_bd;

	r = CALL(lfs_bd, sync, block);

	if (refs)
	{
		block->bd = bd;
		block->translated--;
	}

	return r;
}

static int loop_destroy(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_info_t * info = (loop_info_t *) bd->instance;
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

	bd->instance = info;

	OBJFLAGS(bd) = 0;
	OBJMAGIC(bd) = 0;
	OBJASSIGN(bd, loop, get_config);
	OBJASSIGN(bd, loop, get_status);
	ASSIGN(bd, loop, get_numblocks);
	ASSIGN(bd, loop, get_blocksize);
	ASSIGN(bd, loop, get_atomicsize);
	ASSIGN(bd, loop, read_block);
	ASSIGN(bd, loop, write_block);
	ASSIGN(bd, loop, sync);
	DESTRUCTOR(bd, loop, destroy);

	info->lfs = lfs;

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
