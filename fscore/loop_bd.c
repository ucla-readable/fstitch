#include <lib/platform.h>

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
	BD_t bd;
	
	LFS_t * lfs;
	fdesc_t * file;
	inode_t inode;
};
typedef struct loop_info loop_info_t;

static bdesc_t * loop_read_block(BD_t * bd, uint32_t number, uint16_t count, page_t * page)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, number);
	loop_info_t * info = (loop_info_t *) bd;
	uint32_t lfs_bno;

	/* FIXME: make this module support counts other than 1 */
	assert(count == 1);

	lfs_bno = CALL(info->lfs, get_file_block, info->file, number * bd->blocksize);
	if (lfs_bno == INVALID_BLOCK)
		return NULL;

	return CALL(info->lfs, lookup_block, lfs_bno, page);
}

static bdesc_t * loop_synthetic_read_block(BD_t * bd, uint32_t number, uint16_t count, page_t * page)
{
	Dprintf("%s(0x%x)\n", __FUNCTION__, number);
	loop_info_t * info = (loop_info_t *) bd;
	uint32_t lfs_bno;

	/* FIXME: make this module support counts other than 1 */
	assert(count == 1);

	lfs_bno = CALL(info->lfs, get_file_block, info->file, number * bd->blocksize);
	if (lfs_bno == INVALID_BLOCK)
		return NULL;

	return CALL(info->lfs, synthetic_lookup_block, lfs_bno, page);
}

static int loop_write_block(BD_t * bd, bdesc_t * block, uint32_t loop_number)
{
	Dprintf("%s(0x%08x)\n", __FUNCTION__, block);
	loop_info_t * info = (loop_info_t *) bd;
	uint32_t lfs_number;
	chdesc_t * head = NULL;
	int r;

	lfs_number = CALL(info->lfs, get_file_block, info->file, loop_number * bd->blocksize);
	if(lfs_number == -1)
		return -EINVAL;

	r = chdesc_push_down(block, bd, info->lfs->blockdev);
	if(r < 0)
		return r;

	/* masquerade as an opgroup for things like the journal */
	opgroup_masquerade();
	r = CALL(info->lfs, write_block, block, lfs_number, &head);
	opgroup_demasquerade();
	return r;
}

static int loop_flush(BD_t * bd, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t ** loop_get_write_head(BD_t * bd)
{
	loop_info_t * info = (loop_info_t *) bd;
	return CALL(info->lfs, get_write_head);
}

static int32_t loop_get_block_space(BD_t * bd)
{
	loop_info_t * info = (loop_info_t *) bd;
	return CALL(info->lfs, get_block_space);
}

static int loop_destroy(BD_t * bd)
{
	Dprintf("%s()\n", __FUNCTION__);
	loop_info_t * info = (loop_info_t *) bd;
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	r = modman_dec_lfs(info->lfs, bd);
	assert(r >= 0);
	
	CALL(info->lfs, free_fdesc, info->file);
	memset(info, 0, sizeof(*info));
	free(info);

	return 0;
}


BD_t * loop_bd(LFS_t * lfs, inode_t inode)
{
	Dprintf("%s(lfs 0x%08x, file \"%s\")\n", __FUNCTION__, lfs, file);
	BD_t * bd;
	loop_info_t * info;

	if (!lfs)
		return NULL;

	info = malloc(sizeof(*info));
	if(!info)
		goto error_inode;
	bd = &info->bd;
	
	BD_INIT(bd, loop);

	info->lfs = lfs;
	info->file = CALL(info->lfs, lookup_inode, inode);
	if (!info->file)
		goto error_inode;
	info->inode = inode;

	bd->atomicsize = info->lfs->blockdev->atomicsize;
	bd->blocksize = info->lfs->blockdev->blocksize;
	/* this prevents someone from dynamically growing the disk */
	bd->numblocks = CALL(info->lfs, get_file_numblocks, info->file);
	assert(bd->blocksize == info->lfs->blocksize);

	bd->level = info->lfs->blockdev->level;
	bd->graph_index = info->lfs->blockdev->graph_index + 1;
	if(bd->graph_index >= NBDINDEX)
	{
		DESTROY(bd);
		return NULL;
	}

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
	return NULL;
}
