#include <lib/platform.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/blockman.h>
#include <kfs/modman.h>
#include <kfs/mem_bd.h>
#include <kfs/revision.h>
#include <kfs/josfs_base.h>

#ifdef __KERNEL__
#include <linux/vmalloc.h>
#else
#define vmalloc(x) malloc(x)
#define vfree(x) free(x)
#endif

struct mem_info {
	BD_t bd;
	
	uint8_t *blocks;
	blockman_t blockman;
};

#if 0
static int mem_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	switch(level)
	{
		case CONFIG_BRIEF:
			snprintf(string, length, "%d(%dblks)", bd->blocksize, bd->numblocks);
			break;
		case CONFIG_VERBOSE:
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "%d bytes x %d blocks", bd->blocksize, bd->numblocks);
	}
	return 0;
}

static int mem_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	if (length >= 1)
		string[0] = 0;
	return 0;
}
#endif

static bdesc_t * mem_bd_read_block(BD_t * object, uint32_t number, uint32_t nbytes)
{
	struct mem_info * info = (struct mem_info *) object;
	bdesc_t * bdesc;

	bdesc = blockman_lookup(&info->blockman, number);
	if (bdesc)
	{
		assert(bdesc->length == nbytes);
		if (!bdesc->synthetic)
			return bdesc;
	}
	else
	{
		/* make sure it's a valid block */
		assert(nbytes && number + nbytes / object->blocksize <= object->numblocks);

		bdesc = bdesc_alloc(number, nbytes);
		if (bdesc == NULL)
			return NULL;
		bdesc_autorelease(bdesc);
	}

	memcpy(bdesc->data, &info->blocks[object->blocksize * number], nbytes);

	/* currently we will never get synthetic blocks anyway, but it's easy to handle them */
	if (bdesc->synthetic)
		bdesc->synthetic = 0;
	else
		blockman_add(&info->blockman, bdesc, number);
	return bdesc;
}

static bdesc_t * mem_bd_synthetic_read_block(BD_t * object, uint32_t number, uint32_t nbytes)
{
	/* mem_bd doesn't bother with synthetic blocks,
	 * since it's just as fast to use real ones */
	return mem_bd_read_block(object, number, nbytes);
}

static int mem_bd_write_block(BD_t * object, bdesc_t * block, uint32_t number)
{
	struct mem_info * info = (struct mem_info *) object;
	int r;

	assert(block->length == object->blocksize
	       && number < object->numblocks);

	r = revision_tail_prepare(block, object);
	if (r < 0) {
		kpanic("revision_tail_prepare gave: %i\n", r);
		return r;
	}

	memcpy(&info->blocks[number * object->blocksize],
	       block->data,
	       object->blocksize);

	r = revision_tail_acknowledge(block, object);
	if (r < 0) {
		kpanic("revision_tail_acknowledge gave error: %i\n", r);
		return r;
	}

	return 0;
}

static int mem_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static chdesc_t ** mem_bd_get_write_head(BD_t * object)
{
	return NULL;
}

static int32_t mem_bd_get_block_space(BD_t * object)
{
	return 0;
}

static int mem_bd_destroy(BD_t * bd)
{
	struct mem_info * info = (struct mem_info *) bd;
	int r;

	r = modman_rem_bd(bd);
	if (r < 0) return r;

	blockman_destroy(&info->blockman);

	vfree(info->blocks);
	free_memset(info, sizeof(*info));
	free(info);
	
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
	struct mem_info * info = malloc(sizeof(*info));
	BD_t * bd;
	struct JOSFS_File *f;
	struct JOSFS_Super *s;
	int i;
	
	if (blocks < 1)
		return NULL;

	if(!info)
		return NULL;
	bd = &info->bd;
	
	bd->numblocks = blocks;
	bd->blocksize = blocksize;
	bd->atomicsize = blocksize;

	/* When running in the Linux kernel, we can't allocate this much
	 * memory with kmalloc(). So, we use vmalloc() instead. */
	info->blocks = vmalloc(blocks * blocksize);
	if (!info->blocks) {
		free(info);
		return NULL;
	}
	if (blockman_init(&info->blockman) < 0) {
		free(info->blocks);
		free(info);
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

	for (i = 0; i < blocks; i++)
		mark_block_free(&info->blocks[blocksize * 2], i);
	mark_block_used(&info->blocks[blocksize * 2], 0);
	mark_block_used(&info->blocks[blocksize * 2], 1);
	for (i = 0; i < (blocks + JOSFS_BLKBITSIZE - 1) / JOSFS_BLKBITSIZE; i++)
		mark_block_used(&info->blocks[blocksize * 2], i + 2);
	// done setting up JOS fs

	BD_INIT(bd, mem_bd);
	bd->level = 0;
		
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}
