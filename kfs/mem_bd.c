#include <stdio.h>
#include <inc/error.h>
#include <lib/types.h>
#include <malloc.h>
#include <string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/blockman.h>
#include <kfs/modman.h>
#include <kfs/mem_bd.h>
#include <kfs/revision.h>
#include <kfs/josfs_base.h>

/* We can remove this part once we no longer format as JOS by default */
#ifdef KUDOS_INC_FS_H
#error inc/fs.h got included in mem_bd.c
#endif

struct mem_info {
	uint8_t *blocks;
	uint32_t blockcount;
	uint16_t blocksize;
	uint16_t level;
	blockman_t * blockman;
};

static int mem_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct mem_info * info = (struct mem_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_BRIEF:
			snprintf(string, length, "%d(%dblks)", info->blocksize, info->blockcount);
			break;
		case CONFIG_VERBOSE:
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "%d bytes x %d blocks", info->blocksize, info->blockcount);
	}
	return 0;
}

static int mem_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}

static uint32_t mem_bd_get_numblocks(BD_t * object)
{
	return ((struct mem_info*) OBJLOCAL(object))->blockcount;
}

static uint16_t mem_bd_get_blocksize(BD_t * object)
{
	return ((struct mem_info*) OBJLOCAL(object))->blocksize;
}

static uint16_t mem_bd_get_atomicsize(BD_t * object)
{
	return mem_bd_get_blocksize(object);
}

static bdesc_t * mem_bd_read_block(BD_t * object, uint32_t number)
{
	struct mem_info * info = (struct mem_info *) OBJLOCAL(object);
	bdesc_t * ret;
	int r;

	if (number >= info->blockcount)
		return NULL;

	ret = blockman_managed_lookup(info->blockman, number);
	if (ret)
		return ret;

	ret = bdesc_alloc(number, info->blocksize);
	if (ret == NULL)
		return NULL;
	bdesc_autorelease(ret);
	
	memcpy(ret->ddesc->data, &info->blocks[info->blocksize * number], info->blocksize);

	r = blockman_managed_add(info->blockman, ret);
	if (r < 0)
		return NULL;
	return ret;
}

static bdesc_t * mem_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	bdesc_t * ret = mem_bd_read_block(object, number);
	if(ret)
		*synthetic = 0;
	return ret;
}

static int mem_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct mem_info * info = (struct mem_info *) OBJLOCAL(object);
	datadesc_t * ddesc = blockman_lookup(info->blockman, number);
	if(ddesc)
		blockman_remove(ddesc);
	return 0;
}

static int mem_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct mem_info * info = (struct mem_info *) OBJLOCAL(object);
	int r;
	
	if(block->ddesc->length != info->blocksize) {
		panic("wrote block with bad length\n");
		return -E_INVAL;
	}
	if (block->number >= info->blockcount) {
		panic("wrote bad block number\n");
		return -E_INVAL;
	}

	r = revision_tail_prepare(block, object);
	if (r != 0) {
		panic("revision_tail_prepare gave: %e\n", r);
		return r;
	}

	memcpy(&info->blocks[block->number * info->blocksize],
	       block->ddesc->data,
	       info->blocksize);

	r = revision_tail_acknowledge(block, object);
	if (r != 0) {
		panic("revision_tail_acknowledge gave error: %e\n", r);
		return r;
	}

	return 0;
}

static int mem_bd_sync(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return 0;
}

static uint16_t mem_bd_get_devlevel(BD_t * object)
{
	return ((struct mem_info *) OBJLOCAL(object))->level;
}

static int mem_bd_destroy(BD_t * bd)
{
	struct mem_info * info = (struct mem_info *) OBJLOCAL(bd);
	int r;

	r = modman_rem_bd(bd);
	if (r < 0) return r;

	blockman_destroy(&info->blockman);

	free(info->blocks);
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

static void mark_block_free(uint8_t *b8, int blockno)
{
	uint32_t *b32 = (uint32_t*)b8;
	int word = blockno / 32;
	int bit = blockno % 32;
	b32[word] |= 1<<bit;
}

static void mark_block_used(uint8_t *b8, int blockno)
{
	uint32_t *b32 = (uint32_t*)b8;
	int word = blockno / 32;
	int bit = blockno % 32;
	b32[word] &= ~(1<<bit);
}

BD_t * mem_bd(uint32_t blocks, uint16_t blocksize)
{
	struct mem_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	struct JOSFS_File *f;
	struct JOSFS_Super *s;
	int i;
	
	if (blocks < 1)
		return NULL;

	if(!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	info->blockcount = blocks;
	info->blocksize = blocksize;

	info->blocks = malloc(blocks * blocksize);
	if (!info->blocks) {
		free(info);
		free(bd);
		return NULL;
	}
	info->blockman = blockman_create();
	if (!info->blockman) {
		free(info->blocks);
		free(info);
		free(bd);
		return NULL;
	}

	memset(info->blocks, 0, blocks * blocksize);

	// Set up JOS fs on the mem device. in an ideal world this would
	// be done w/ mkjosfs
	s = (struct JOSFS_Super *) &info->blocks[blocksize];
	s->s_magic = JOSFS_FS_MAGIC;
	s->s_nblocks = blocks;

	f = &s->s_root;
	strcpy(f->f_name, "/");
	f->f_size = 0;
	f->f_type = JOSFS_TYPE_DIR;
	for (i = 0; i < JOSFS_NDIRECT; i++)
		f->f_direct[i] = 0;
	f->f_indirect = 0;
	f->f_dir = 0;

	for (i = 0; i < blocks; i++)
		mark_block_free(&info->blocks[blocksize * 2], i);
	mark_block_used(&info->blocks[blocksize * 2], 0);
	mark_block_used(&info->blocks[blocksize * 2], 1);
	for (i = 0; i < ((blocks/blocksize) + ((blocks % blocksize) != 0)); i++)
		mark_block_used(&info->blocks[blocksize * 2], i + 2);
	// done setting up JOS fs

	info->level = 0;
	
	BD_INIT(bd, mem_bd, info);
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}
