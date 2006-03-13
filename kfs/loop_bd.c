#include <inc/error.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>
#include <lib/panic.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/lfs.h>
#include <kfs/modman.h>
#include <kfs/loop_bd.h>

#if !defined(__KERNEL__)
#include <assert.h>
#else
#warning Add assert.h support
#define assert(x) do { } while(0)
#endif

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
	inode_t inode;
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
			snprintf(string, length, "inode: %d, blocksize: %d, count: %d", info->inode, info->blocksize, CALL(info->lfs, get_file_numblocks, info->file));
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "%d", info->inode);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "inode: %d, blocksize: %d", info->inode, info->blocksize);
	}
	return 0;
}

static int loop_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	if (length >= 1)
		string[0] = 0;
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

static bdesc_t * loop_read_block(BD_t * bd, uint32_t number, uint16_t count)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, number);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	uint32_t lfs_bno;
	bdesc_t * block;

	/* FIXME: make this module support counts other than 1 */
	assert(count == 1);

	lfs_bno = CALL(info->lfs, get_file_block, info->file, number * info->blocksize);
	if (lfs_bno == INVALID_BLOCK)
		return NULL;

	block = CALL(info->lfs, lookup_block, lfs_bno);
	if (!block)
		return NULL;

	block = bdesc_alloc_clone(block, number);
	if (!block)
		return NULL;
	bdesc_autorelease(block);

	return block;
}

static bdesc_t * loop_synthetic_read_block(BD_t * bd, uint32_t number, uint16_t count, bool * synthetic)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, number);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	uint32_t lfs_bno;
	bdesc_t * block;

	/* FIXME: make this module support counts other than 1 */
	assert(count == 1);

	lfs_bno = CALL(info->lfs, get_file_block, info->file, number * info->blocksize);
	if (lfs_bno == INVALID_BLOCK)
		return NULL;

	block = CALL(info->lfs, synthetic_lookup_block, lfs_bno, synthetic);
	if (!block)
		return NULL;

	block = bdesc_alloc_clone(block, number);
	if (!block)
	{
		if(*synthetic)
			CALL(info->lfs, cancel_synthetic_block, lfs_bno);
		return NULL;
	}
	bdesc_autorelease(block);

	return block;
}

static int loop_cancel_block(BD_t * bd, uint32_t number)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, number);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	uint32_t lfs_bno;

	lfs_bno = CALL(info->lfs, get_file_block, info->file, number * info->blocksize);
	if (lfs_bno == INVALID_BLOCK)
		return -E_INVAL;

	return CALL(info->lfs, cancel_synthetic_block, lfs_bno);
}

static int loop_write_block(BD_t * bd, bdesc_t * block)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block);
	loop_info_t * info = (loop_info_t *) OBJLOCAL(bd);
	uint32_t loop_number, lfs_number;
	bdesc_t * wblock;
	chdesc_t * head = NULL;
	int r;

	loop_number = block->number;
	lfs_number = CALL(info->lfs, get_file_block, info->file, loop_number * info->blocksize);
	if(lfs_number == -1)
		return -E_INVAL;

	wblock = bdesc_alloc_clone(block, lfs_number);
	if(!wblock)
		return -E_UNSPECIFIED;
	bdesc_autorelease(wblock);

	r = chdesc_push_down(bd, block, info->lfs_bd, wblock);
	if(r < 0)
		return r;

	return CALL(info->lfs, write_block, wblock, &head);
}

static int loop_flush(BD_t * bd, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
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
	r = modman_dec_lfs(info->lfs, bd);
	assert(r >= 0);
	
	CALL(info->lfs, free_fdesc, info->file);
	memset(info, 0, sizeof(*info));
	free(info);

	memset(bd, 0, sizeof(*bd));
	free(bd);

	return 0;
}


BD_t * loop_bd(LFS_t * lfs, inode_t inode)
{
	Dprintf("%s(lfs 0x%08x, file \"%s\")\n", __FUNCTION__, lfs, file);
	BD_t * bd;
	loop_info_t * info;

	if (!lfs)
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

	info->inode = inode;

	info->file = CALL(info->lfs, lookup_inode, inode);
	if (!info->file)
		goto error_inode;

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

  error_inode:
	free(info);
  error_bd:
	free(bd);
	return NULL;
}
