/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/pool.h>
#include <lib/hash_map.h>

#include <fscore/bd.h>
#include <fscore/lfs.h>
#include <fscore/modman.h>
#include <fscore/patch.h>
#include <fscore/sched.h>
#include <fscore/debug.h>
#include <fscore/feature.h>

#include <modules/waffle.h>
#include <modules/waffle_lfs.h>

#define WAFFLE_LFS_DEBUG 1

#if WAFFLE_LFS_DEBUG
#define Dprintf(x...) printf("WAFFLEDEBUG: " x)
#else
#define Dprintf(x...)
#endif

/* structures and globals {{{ */

/* Every active block is ultimately pointed to via some chain of pointers by the
 * superblock. When we need to clone a block, we may need to clone all the
 * blocks up to the superblock. Since we will discover that we need to do this
 * at the leaves of the pointer tree (and it is a tree, not a DAG), we need a
 * way to track backwards to the root. This structure helps us do that by
 * encapsulating a block number along with the unique location in some other
 * block that points to it, and a pointer to the same structure for that block
 * pointer. Some of these are kept in a hash map keyed by block number; others
 * can be stored on the stack if they are only to be used temporarily. */

struct blkptr {
	/* the block number */
	uint32_t number;
	/* the block itself */
	bdesc_t * block;
	/* the offset within the parent block */
	uint16_t parent_offset;
	/* the parent block pointer structure - NULL for superblock pointers */
	struct blkptr * parent;
	/* the number of children that refer to this structure */
	int references;
};
#define blkptr_data(blkptr) bdesc_data((blkptr)->block)

struct waffle_fdesc {
	/* extend struct fdesc */
	struct fdesc_common * common;
	struct fdesc_common base;
	
	struct waffle_fdesc ** f_cache_pprev;
	struct waffle_fdesc * f_cache_next;
	uint32_t f_nopen;
	uint32_t f_age;
	
	inode_t f_inode;
	uint8_t f_type;
	/* the struct blkptr for the inode block */
	struct blkptr * f_inode_blkptr;
	uint16_t f_inode_offset;
};
#define f_ip(wfd) ((const struct waffle_inode *) (blkptr_data((wfd)->f_inode_blkptr) + (wfd)->f_inode_offset))
	
struct waffle_bitmap_cache {
	bdesc_t * bb_cache;
	uint32_t bb_number;
	/* block bitmap block index */
	uint32_t bb_index;
};

struct waffle_old_snapshot {
	patchweakref_t overwrite;
	struct waffle_bitmap_cache bitmap;
	struct waffle_snapshot snapshot;
	struct waffle_old_snapshot * next;
};

/* waffle LFS structure */
struct waffle_info {
	LFS_t lfs;
	
	BD_t * ubd;
	patch_t ** write_head;
	bdesc_t * super_cache;
	const struct waffle_super * super;
	struct waffle_snapshot s_active;
	struct waffle_bitmap_cache active, checkpoint, snapshot[WAFFLE_SNAPSHOT_COUNT];
	struct waffle_old_snapshot * old_snapshots;
	int cloned_since_checkpoint;
	patch_t * checkpoint_changes;
	patch_t * checkpoint_tail;
	int try_next_free;
	uint32_t free_blocks;
	struct waffle_fdesc * filecache;
	/* map from block number -> struct blkptr */
	hash_map_t * blkptr_map;
	int fdesc_count;
};

DECLARE_POOL(waffle_blkptr_pool, struct blkptr);
DECLARE_POOL(waffle_fdesc_pool, struct waffle_fdesc);
DECLARE_POOL(waffle_snapshot_pool, struct waffle_old_snapshot);
static int n_waffle_instances = 0;

/* }}} */

/* blkptr library {{{ */

static struct blkptr * waffle_get_blkptr(struct waffle_info * info, struct blkptr * parent, uint32_t number, bdesc_t * block, uint16_t parent_offset)
{
	struct blkptr * blkptr = (struct blkptr *) hash_map_find_val(info->blkptr_map, (void *) number);
	if(blkptr)
	{
		blkptr->references++;
		return blkptr;
	}
	blkptr = waffle_blkptr_pool_alloc();
	if(!blkptr)
		return NULL;
	blkptr->number = number;
	blkptr->block = block;
	blkptr->parent_offset = parent_offset;
	blkptr->parent = parent;
	blkptr->references = 1;
	if(hash_map_insert(info->blkptr_map, (void *) number, blkptr) < 0)
	{
		waffle_blkptr_pool_free(blkptr);
		return NULL;
	}
	bdesc_retain(block);
	if(parent)
		parent->references++;
	return blkptr;
}

static void waffle_put_blkptr(struct waffle_info * info, struct blkptr * blkptr)
{
	if(!--blkptr->references)
	{
		if(blkptr->parent)
			waffle_put_blkptr(info, blkptr->parent);
		bdesc_release(&blkptr->block);
		hash_map_erase(info->blkptr_map, (void *) blkptr->number);
		waffle_blkptr_pool_free(blkptr);
	}
}
#define waffle_put_blkptr(info, blkptr) waffle_put_blkptr(info, *(blkptr)), *(blkptr) = NULL

/* }}} */

/* bitmap block cloning {{{ */

/* In order to avoid infinite recursion trying to allocate a block to clone a
 * bitmap to allocate a block, enough bitmap blocks for n + 2 copies of the
 * bitmap (where there are n on-disk snapshots) are preallocated and always
 * marked as in use. They are set up in consecutive sets of n + 2, where for any
 * given position in the block bitmap each snapshot must use one of the
 * consecutive n + 2 blocks assigned to that position. This allows a fast check
 * to see which of those blocks are in use, without using a bitmap that would
 * have to be modified (and thus cloned). */

static uint32_t waffle_get_inode_block(struct waffle_info * info, const struct waffle_inode * inode, uint32_t offset);

static inline int waffle_bitmap_block_in_use(struct waffle_info * info, struct waffle_bitmap_cache * cache, const struct waffle_snapshot * snapshot, uint32_t number, uint32_t index)
{
	if(index >= snapshot->sn_blocks)
		/* it's not even in this snapshot */
		return 0;
	if(cache->bb_cache && cache->bb_number == number)
	{
		/* it's not only in use but it's the currently cached block */
		assert(cache->bb_index == index);
		return 1;
	}
	return number == waffle_get_inode_block(info, &snapshot->sn_block, index * WAFFLE_BLOCK_SIZE);
}

/* the number is of the indirect block; the index is of the referenced bitmap block */
static inline int waffle_bitmap_indir_in_use(struct waffle_info * info, struct waffle_bitmap_cache * cache, const struct waffle_snapshot * snapshot, uint32_t number, uint32_t index)
{
	bdesc_t * dindirect;
	assert(index > WAFFLE_DIRECT_BLOCKS);
	if(index >= snapshot->sn_blocks)
		/* it's not even in this snapshot */
		return 0;
	if(index <= WAFFLE_INDIRECT_BLOCKS)
		return number == snapshot->sn_block.i_indirect;
	dindirect = CALL(info->ubd, read_block, snapshot->sn_block.i_dindirect, 1, NULL);
	if(!dindirect)
	{
		fprintf(stderr, "%s(): warning: could not read bitmap double indirect block!\n", __FUNCTION__);
		return 1;
	}
	index -= WAFFLE_DIRECT_BLOCKS;
	index /= WAFFLE_BLOCK_POINTERS;
	return number == ((uint32_t *) bdesc_data(dindirect))[index];
}

/* the number is of the double indirect block; the index is of the referenced bitmap block */
static inline int waffle_bitmap_dindir_in_use(struct waffle_info * info, struct waffle_bitmap_cache * cache, const struct waffle_snapshot * snapshot, uint32_t number, uint32_t index)
{
	assert(index > WAFFLE_INDIRECT_BLOCKS);
	return number == snapshot->sn_block.i_dindirect;
}

#define DEFINE_WAFFLE_BITMAP_X_IN_SNAPSHOT(x) \
static int waffle_bitmap_##x##_in_snapshot(struct waffle_info * info, uint32_t number, uint32_t index) \
{ \
	struct waffle_old_snapshot ** next = &info->old_snapshots; \
	int i; \
	\
	if(number >= info->super->s_blocks) \
		return -EINVAL; \
	\
	if(waffle_bitmap_##x##_in_use(info, &info->checkpoint, &info->super->s_checkpoint, number, index)) \
		return 1; \
	for(i = 0; i < WAFFLE_SNAPSHOT_COUNT; i++) \
		if(waffle_bitmap_##x##_in_use(info, &info->snapshot[i], &info->super->s_snapshot[i], number, index)) \
			return 1; \
	\
	while(*next) \
	{ \
		if(!WEAK((*next)->overwrite)) \
		{ \
			struct waffle_old_snapshot * old = *next; \
			*next = old->next; \
			if(old->bitmap.bb_cache) \
				bdesc_release(&old->bitmap.bb_cache); \
			waffle_snapshot_pool_free(old); \
			continue; \
		} \
		if(waffle_bitmap_##x##_in_use(info, &(*next)->bitmap, &(*next)->snapshot, number, index)) \
			return 1; \
		next = &(*next)->next; \
	} \
	\
	return 0; \
}

DEFINE_WAFFLE_BITMAP_X_IN_SNAPSHOT(block);
DEFINE_WAFFLE_BITMAP_X_IN_SNAPSHOT(indir);
DEFINE_WAFFLE_BITMAP_X_IN_SNAPSHOT(dindir);

/* we can use the same waffle_update_pointer() from below, with is_bitmap = 1 */
static int waffle_update_pointer(struct waffle_info * info, struct blkptr * blkptr, uint32_t block, int is_bitmap);

static int waffle_clone_bitmap_guts(struct waffle_info * info, struct blkptr * blkptr, uint32_t target)
{
	int r;
	patch_t * patch = NULL;
	bdesc_t * copy = CALL(info->ubd, synthetic_read_block, target, 1, NULL);
	if(!copy)
		return -1;
	r = patch_create_full(copy, info->ubd, blkptr_data(blkptr), &patch);
	if(r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, patch, "init bitmap copy");
	r = patch_add_depend(info->checkpoint_changes, patch);
	if(r < 0)
		kpanic("unrecoverable error adding patch dependency: %d", r);
	r = CALL(info->ubd, write_block, copy, target);
	if(r < 0)
		kpanic("unrecoverable error writing block: %d", r);
	r = waffle_update_pointer(info, blkptr, target, 1);
	if(r < 0)
		return r;
	r = hash_map_change_key(info->blkptr_map, (void *) blkptr->number, (void *) target);
	if(r < 0 && r != -ENOENT)
	{
		kpanic("unexpected error changing hash map keys: %d", r);
		return r;
	}
	/* update the active block bitmap cache if needed */
	if(info->active.bb_number == blkptr->number)
	{
		bdesc_release(&info->active.bb_cache);
		info->active.bb_cache = bdesc_retain(copy);
		info->active.bb_number = target;
	}
	blkptr->number = target;
	bdesc_release(&blkptr->block);
	blkptr->block = bdesc_retain(copy);
	info->cloned_since_checkpoint++;
	return 0;
}

/* the blkptr is of the double indirect block; the index is of the referenced block */
static int waffle_clone_bitmap_dindir(struct waffle_info * info, struct blkptr * blkptr, uint32_t index)
{
	Dprintf("%s %u [%u]\n", __FUNCTION__, blkptr->number, index);
	uint32_t group = blkptr->number - (blkptr->number % WAFFLE_BITMAP_MODULUS);
	uint32_t check, max = group + WAFFLE_BITMAP_MODULUS;
	assert(!blkptr->parent);
	for(;;)
	{
		for(check = group; check != max; check++)
		{
			if(check == blkptr->number)
				continue;
			if(waffle_bitmap_dindir_in_snapshot(info, check, index))
				continue;
			return waffle_clone_bitmap_guts(info, blkptr, check);
		}
		/* all the bitmap blocks for this location are in use; force some of the old uses to be overwritten */
		if(CALL(info->ubd, flush, FLUSH_DEVICE, NULL) == FLUSH_NONE)
			kpanic("could not flush snapshots!");
	}
}

/* the blkptr is of the indirect block; the index is of the referenced block */
static int waffle_clone_bitmap_indir(struct waffle_info * info, struct blkptr * blkptr, uint32_t index)
{
	Dprintf("%s %u [%u]\n", __FUNCTION__, blkptr->number, index);
	uint32_t group = blkptr->number - (blkptr->number % WAFFLE_BITMAP_MODULUS);
	uint32_t check, max = group + WAFFLE_BITMAP_MODULUS;
	assert(blkptr->parent && !blkptr->parent->parent);
	for(;;)
	{
		for(check = group; check != max; check++)
		{
			if(check == blkptr->number)
				continue;
			if(waffle_bitmap_indir_in_snapshot(info, check, index))
				continue;
			return waffle_clone_bitmap_guts(info, blkptr, check);
		}
		/* all the bitmap blocks for this location are in use; force some of the old uses to be overwritten */
		if(CALL(info->ubd, flush, FLUSH_DEVICE, NULL) == FLUSH_NONE)
			kpanic("could not flush snapshots!");
	}
}

static int waffle_clone_bitmap_block(struct waffle_info * info, struct blkptr * blkptr, uint32_t index)
{
	Dprintf("%s %u [%u]\n", __FUNCTION__, blkptr->number, index);
	uint32_t group = blkptr->number - (blkptr->number % WAFFLE_BITMAP_MODULUS);
	uint32_t check, max = group + WAFFLE_BITMAP_MODULUS;
	if(blkptr->parent && waffle_bitmap_indir_in_snapshot(info, blkptr->parent->number, index))
	{
		int r;
		struct blkptr * parent = blkptr->parent;
		if(parent->parent && waffle_bitmap_dindir_in_snapshot(info, parent->parent->number, index))
		{
			r = waffle_clone_bitmap_dindir(info, parent->parent, index);
			if(r < 0)
				return r;
		}
		r = waffle_clone_bitmap_indir(info, parent, index);
		if(r < 0)
			return r;
	}
	for(;;)
	{
		for(check = group; check != max; check++)
		{
			if(check == blkptr->number)
				continue;
			if(waffle_bitmap_block_in_snapshot(info, check, index))
				continue;
			return waffle_clone_bitmap_guts(info, blkptr, check);
		}
		/* all the bitmap blocks for this location are in use; force some of the old uses to be overwritten */
		if(CALL(info->ubd, flush, FLUSH_DEVICE, NULL) == FLUSH_NONE)
			kpanic("could not flush snapshots!");
	}
}

/* }}} */

/* regular block cloning and bitmap tracking {{{ */

/* Now the regular (non-bitmap) block allocation and cloning routines... */

static inline int waffle_block_in_use(struct waffle_info * info, struct waffle_bitmap_cache * cache, const struct waffle_snapshot * snapshot, uint32_t number)
{
	uint32_t need = number / WAFFLE_BITS_PER_BLOCK;
	if(number >= snapshot->sn_blocks)
		/* it's not even in this snapshot */
		return 0;
	if(!cache->bb_cache || cache->bb_index != need)
	{
		if(cache->bb_cache)
			bdesc_release(&cache->bb_cache);
		uint32_t bitmap_block = waffle_get_inode_block(info, &snapshot->sn_block, need * WAFFLE_BLOCK_SIZE);
		if(!bitmap_block || bitmap_block == INVALID_BLOCK)
			return -1;
		cache->bb_cache = CALL(info->ubd, read_block, bitmap_block, 1, NULL);
		if(!cache->bb_cache)
			return -1;
		bdesc_retain(cache->bb_cache);
		cache->bb_cache->flags |= BDESC_FLAG_BITMAP;
		cache->bb_number = bitmap_block;
		cache->bb_index = need;
	}
	need = number % WAFFLE_BITS_PER_BLOCK;
	return !((((uint32_t *) bdesc_data(cache->bb_cache))[need / 32] >> (need % 32)) & 1);
}

/* returns true if the specified photograph contains an eggo... */
static int waffle_in_snapshot(struct waffle_info * info, uint32_t number)
{
	struct waffle_old_snapshot ** next = &info->old_snapshots;
	int i;
	
	if(number >= info->super->s_blocks)
		return -EINVAL;
	
	if(waffle_block_in_use(info, &info->checkpoint, &info->super->s_checkpoint, number))
		return 1;
	for(i = 0; i < WAFFLE_SNAPSHOT_COUNT; i++)
		if(waffle_block_in_use(info, &info->snapshot[i], &info->super->s_snapshot[i], number))
			return 1;
	
	while(*next)
	{
		if(!WEAK((*next)->overwrite))
		{
			struct waffle_old_snapshot * old = *next;
			*next = old->next;
			if(old->bitmap.bb_cache)
				bdesc_release(&old->bitmap.bb_cache);
			waffle_snapshot_pool_free(old);
			continue;
		}
		if(waffle_block_in_use(info, &(*next)->bitmap, &(*next)->snapshot, number))
			return 1;
		next = &(*next)->next;
	}
	
	return 0;
}

static int waffle_can_allocate(struct waffle_info * info, uint32_t number)
{
	struct waffle_old_snapshot ** next = &info->old_snapshots;
	int i;
	
	if(number >= info->super->s_blocks)
		return 0;
	
	if(waffle_block_in_use(info, &info->checkpoint, &info->super->s_checkpoint, number))
		return 0;
	for(i = 0; i < WAFFLE_SNAPSHOT_COUNT; i++)
		if(waffle_block_in_use(info, &info->snapshot[i], &info->super->s_snapshot[i], number))
			return 0;
	if(waffle_block_in_use(info, &info->active, &info->s_active, number))
		return 0;
	
	while(*next)
	{
		if(!WEAK((*next)->overwrite))
		{
			struct waffle_old_snapshot * old = *next;
			*next = old->next;
			if(old->bitmap.bb_cache)
				bdesc_release(&old->bitmap.bb_cache);
			waffle_snapshot_pool_free(old);
			continue;
		}
		if(waffle_block_in_use(info, &(*next)->bitmap, &(*next)->snapshot, number))
			return 0;
		next = &(*next)->next;
	}
	
	return 1;
}

static uint32_t waffle_find_free_block(struct waffle_info * info, uint32_t number)
{
	uint32_t start = number;
	/* TODO: this is a really stupid way to do this; can we do better? */
	while(!waffle_can_allocate(info, number))
	{
		if(++number >= info->super->s_blocks)
			number = WAFFLE_SUPER_BLOCK + 1;
		if(number == start)
			return INVALID_BLOCK;
	}
	return number;
}

/* returns the requested blkptr with its reference count increased */
static struct blkptr * waffle_follow_pointer(struct waffle_info * info, struct blkptr * parent, const uint32_t * pointer)
{
	bdesc_t * block;
	uint32_t offset;
	if(parent)
	{
		offset = ((uintptr_t) pointer) - (uintptr_t) blkptr_data(parent);
		if(offset > WAFFLE_BLOCK_SIZE - sizeof(*pointer))
			return NULL;
	}
	else
	{
		/* root blkptr: relative to info->s_active */
		offset = ((uintptr_t) pointer) - (uintptr_t) &info->s_active;
		if(offset > sizeof(info->s_active) - sizeof(*pointer))
			return NULL;
	}
	block = CALL(info->ubd, read_block, *pointer, 1, NULL);
	if(!block)
		return NULL;
	return waffle_get_blkptr(info, parent, *pointer, block, offset);
}

/* updates the pointer to this blkptr (mid clone); parent must already be cloned */
static int waffle_update_pointer(struct waffle_info * info, struct blkptr * blkptr, uint32_t block, int is_bitmap)
{
	Dprintf("%s %u -> %u\n", __FUNCTION__, blkptr->number, block);
	if(blkptr->parent)
	{
		assert(is_bitmap || !waffle_in_snapshot(info, blkptr->parent->number));
		patch_t * patch = NULL;
		int r = patch_create_byte(blkptr->parent->block, info->ubd, blkptr->parent_offset, sizeof(block), &block, &patch);
		if(r < 0)
			return r;
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, patch, "update pointer");
		r = patch_add_depend(info->checkpoint_changes, patch);
		if(r < 0)
			kpanic("unrecoverable error adding patch dependency: %d", r);
		return CALL(info->ubd, write_block, blkptr->parent->block, blkptr->parent->number);
	}
	/* root blkptr: relative to info->s_active */
	*((uint32_t *) (((void *) &info->s_active) + blkptr->parent_offset)) = block;
	return 0;
}
#define waffle_update_pointer(info, blkptr, block) waffle_update_pointer(info, blkptr, block, 0)

static int waffle_clone_block(struct waffle_info * info, struct blkptr * blkptr);

/* updates the indicated value on the block pointed to by this blkptr */
static int waffle_update_value(struct waffle_info * info, struct blkptr * blkptr, const void * pointer, const void * value, size_t size)
{
	Dprintf("%s\n", __FUNCTION__);
	int r;
	patch_t * patch = NULL;
	uint32_t offset = offset = ((uintptr_t) pointer) - (uintptr_t) blkptr_data(blkptr);
	if(size < 1 || size > WAFFLE_BLOCK_SIZE || offset > WAFFLE_BLOCK_SIZE - size)
		return -EINVAL;
	if(waffle_in_snapshot(info, blkptr->number))
	{
		r = waffle_clone_block(info, blkptr);
		if(r < 0)
			return r;
	}
	r = patch_create_byte(blkptr->block, info->ubd, offset, size, value, &patch);
	if(r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, patch, "update value");
	r = patch_add_depend(info->checkpoint_changes, patch);
	if(r < 0)
		kpanic("unrecoverable error adding patch dependency: %d", r);
	return CALL(info->ubd, write_block, blkptr->block, blkptr->number);
}

/* returns the requested blkptr with its reference count increased */
static struct blkptr * waffle_get_data_blkptr(struct waffle_info * info, const struct waffle_inode * inode, struct blkptr * inode_blkptr, uint32_t inode_offset)
{
	struct blkptr * blkptr;
	uint32_t * pointer;
	struct blkptr * indirect_blkptr;
	struct blkptr * dindirect_blkptr;
	inode_offset /= WAFFLE_BLOCK_SIZE;
	if(inode_offset >= inode->i_blocks)
		/* asked for an offset past the last block */
		return NULL;
	if(inode_offset < WAFFLE_DIRECT_BLOCKS)
		return waffle_follow_pointer(info, inode_blkptr, &inode->i_direct[inode_offset]);
	if(inode_offset < WAFFLE_INDIRECT_BLOCKS)
	{
		indirect_blkptr = waffle_follow_pointer(info, inode_blkptr, &inode->i_indirect);
		if(!indirect_blkptr)
			return NULL;
		pointer = &((uint32_t *) blkptr_data(indirect_blkptr))[inode_offset - WAFFLE_DIRECT_BLOCKS];
		blkptr = waffle_follow_pointer(info, indirect_blkptr, pointer);
		waffle_put_blkptr(info, &indirect_blkptr);
		return blkptr;
	}
	dindirect_blkptr = waffle_follow_pointer(info, inode_blkptr, &inode->i_dindirect);
	if(!dindirect_blkptr)
		return NULL;
	pointer = &((uint32_t *) blkptr_data(dindirect_blkptr))[(inode_offset - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS];
	indirect_blkptr = waffle_follow_pointer(info, dindirect_blkptr, pointer);
	waffle_put_blkptr(info, &indirect_blkptr);
	if(!indirect_blkptr)
		return NULL;
	pointer = &((uint32_t *) blkptr_data(indirect_blkptr))[(inode_offset - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS];
	blkptr = waffle_follow_pointer(info, indirect_blkptr, pointer);
	waffle_put_blkptr(info, &indirect_blkptr);
	return blkptr;
}

static int waffle_change_allocation(struct waffle_info * info, uint32_t number, int is_free)
{
	Dprintf("%s %u (%s)\n", __FUNCTION__, number, is_free ? "free" : "used");
	int r = 0;
	patch_t * patch = NULL;
	uint32_t index = number % WAFFLE_BITS_PER_BLOCK;
	uint32_t bitmap_index = number / WAFFLE_BITS_PER_BLOCK;
	struct blkptr * bitmap = waffle_get_data_blkptr(info, &info->s_active.sn_block, NULL, number / 8);
	if(!bitmap)
		return -1;
	if(((((uint32_t *) blkptr_data(bitmap))[index / 32] >> (index % 32)) & 1) == !!is_free)
		/* already in the right state */
		goto exit_r;
	if(waffle_bitmap_block_in_snapshot(info, bitmap->number, bitmap_index))
	{
		r = waffle_clone_bitmap_block(info, bitmap, bitmap_index);
		if(r < 0)
			goto exit_r;
	}
	r = patch_create_bit(bitmap->block, info->ubd, index / 32, 1 << (index % 32), &patch);
	if(r < 0)
		goto exit_r;
	r = patch_add_depend(info->checkpoint_changes, patch);
	if(r < 0)
		goto exit_r;
	r = CALL(info->ubd, write_block, bitmap->block, bitmap->number);
  exit_r:
	waffle_put_blkptr(info, &bitmap);
	return r;
}
#define waffle_mark_allocated(info, number) waffle_change_allocation(info, number, 0)
#define waffle_mark_deallocated(info, number) waffle_change_allocation(info, number, 1)

/* TODO: update try_next_free somewhere in here? */
static int waffle_clone_block(struct waffle_info * info, struct blkptr * blkptr)
{
	Dprintf("%s\n", __FUNCTION__);
	uint32_t number;
	bdesc_t * copy;
	patch_t * patch = NULL;
	int r;
	
	number = waffle_find_free_block(info, info->try_next_free);
	if(!number || number == INVALID_BLOCK)
		return -ENOSPC;
	r = waffle_mark_allocated(info, number);
	if(r < 0)
		return r;
	copy = CALL(info->ubd, synthetic_read_block, number, 1, NULL);
	if(!copy)
	{
		waffle_mark_deallocated(info, number);
		return -1;
	}
	r = patch_create_full(copy, info->ubd, blkptr_data(blkptr), &patch);
	if(r < 0)
	{
	  fail_r:
		waffle_mark_deallocated(info, number);
		return r;
	}
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, patch, "init copy");
	r = patch_add_depend(info->checkpoint_changes, patch);
	if(r < 0)
		kpanic("unrecoverable error adding patch dependency: %d", r);
	r = CALL(info->ubd, write_block, copy, number);
	if(r < 0)
		goto fail_r;
	if(blkptr->parent && waffle_in_snapshot(info, blkptr->parent->number))
	{
		r = waffle_clone_block(info, blkptr->parent);
		if(r < 0)
			goto fail_r;
	}
	r = waffle_update_pointer(info, blkptr, number);
	if(r < 0)
		goto fail_r;
	r = hash_map_change_key(info->blkptr_map, (void *) blkptr->number, (void *) number);
	/* XXX: this has returned -EEXIST in the kernel; why?? */
	if(r < 0 && r != -ENOENT)
	{
		kpanic("unexpected error changing hash map keys: %d", r);
		return r;
	}
	/* update the active block bitmap cache if needed */
	if(info->active.bb_number == blkptr->number)
	{
		bdesc_release(&info->active.bb_cache);
		info->active.bb_cache = bdesc_retain(copy);
		info->active.bb_number = number;
	}
	r = waffle_mark_deallocated(info, blkptr->number);
	if(r < 0)
		kpanic("unrecoverable error deallocating cloned block: %d", r);
	blkptr->number = number;
	bdesc_release(&blkptr->block);
	blkptr->block = bdesc_retain(copy);
	info->cloned_since_checkpoint++;
	return 0;
}

/* }}} */

/* LFS helper functions {{{ */

static int waffle_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head);

static int waffle_add_dentry(struct waffle_info * info, struct waffle_fdesc * directory, const char * name, inode_t inode, uint16_t waffle_type)
{
	Dprintf("%s %u:%s -> %u\n", __FUNCTION__, directory->f_inode, name, inode);
	int r;
	uint32_t index;
	struct waffle_dentry init;
	struct waffle_dentry * dirent = NULL;
	struct blkptr * dir_blkptr = NULL;
	const struct waffle_inode * f_ip = f_ip(directory);
	
	/* search for an empty dentry */
	for(index = 0; index < f_ip->i_size; index += WAFFLE_BLOCK_SIZE)
	{
		uint16_t offset;
		dir_blkptr = waffle_get_data_blkptr(info, f_ip, directory->f_inode_blkptr, index);
		if(!dir_blkptr)
			return -1;
		for(offset = 0; offset < WAFFLE_BLOCK_SIZE; offset += sizeof(struct waffle_dentry))
		{
			dirent = (struct waffle_dentry *) (blkptr_data(dir_blkptr) + offset);
			if(!dirent->d_inode)
				break;
		}
		if(offset < WAFFLE_BLOCK_SIZE)
			break;
		waffle_put_blkptr(info, &dir_blkptr);
	}
	
	if(!dir_blkptr)
	{
		bdesc_t * zero;
		patch_t * patch = NULL;
		uint32_t new_size = f_ip->i_size + WAFFLE_BLOCK_SIZE;
		uint32_t number = waffle_find_free_block(info, info->try_next_free);
		if(number == INVALID_BLOCK)
			return -ENOSPC;
		assert(!waffle_in_snapshot(info, number));
		zero = CALL(info->ubd, synthetic_read_block, number, 1, NULL);
		if(!zero)
			return -1;
		/* TODO: technically we might want to hook this up to checkpoint_changes */
		r = patch_create_init(zero, info->ubd, &patch);
		if(r < 0)
			return r;
		r = CALL(info->ubd, write_block, zero, number);
		if(r < 0)
			return r;
		r = waffle_mark_allocated(info, number);
		if(r < 0)
			return r;
		r = waffle_append_file_block(&info->lfs, (fdesc_t *) directory, number, NULL);
		if(r < 0)
		{
			waffle_mark_deallocated(info, number);
			return r;
		}
		r = waffle_update_value(info, directory->f_inode_blkptr, &f_ip->i_size, &new_size, sizeof(f_ip->i_size));
		if(r < 0)
			kpanic("unexpected error updating inode");
		f_ip = f_ip(directory);
		dir_blkptr = waffle_get_data_blkptr(info, f_ip, directory->f_inode_blkptr, index);
		if(!dir_blkptr)
			return -1;
		dirent = (struct waffle_dentry *) blkptr_data(dir_blkptr);
	}
	
	/* set up dentry */
	init.d_inode = inode;
	init.d_type = waffle_type;
	memset(init.d_name, 0, sizeof(init.d_name));
	strncpy(init.d_name, name, sizeof(init.d_name));
	init.d_name[sizeof(init.d_name) - 1] = 0;
	
	/* write dentry */
	r = waffle_update_value(info, dir_blkptr, dirent, &init, sizeof(*dirent));
	waffle_put_blkptr(info, &dir_blkptr);
	return r;
}

static int waffle_clear_dentry(struct waffle_info * info, struct waffle_fdesc * directory, const char * name)
{
	Dprintf("%s %u:%s\n", __FUNCTION__, directory->f_inode, name);
	uint32_t index;
	struct waffle_dentry * dirent = NULL;
	struct blkptr * dir_blkptr = NULL;
	const struct waffle_inode * f_ip = f_ip(directory);
	inode_t zero = 0;
	int r;
	
	/* search for an empty dentry */
	for(index = 0; index < f_ip->i_size; index += WAFFLE_BLOCK_SIZE)
	{
		uint16_t offset;
		dir_blkptr = waffle_get_data_blkptr(info, f_ip, directory->f_inode_blkptr, index);
		if(!dir_blkptr)
			return -1;
		for(offset = 0; offset < WAFFLE_BLOCK_SIZE; offset += sizeof(struct waffle_dentry))
		{
			dirent = (struct waffle_dentry *) (blkptr_data(dir_blkptr) + offset);
			if(dirent->d_inode && !strcmp(dirent->d_name, name))
				break;
		}
		if(offset < WAFFLE_BLOCK_SIZE)
			break;
		waffle_put_blkptr(info, &dir_blkptr);
	}
	
	if(!dir_blkptr)
		return -ENOENT;
	
	/* write dentry */
	r = waffle_update_value(info, dir_blkptr, &dirent->d_inode, &zero, sizeof(dirent->d_inode));
	waffle_put_blkptr(info, &dir_blkptr);
	return r;
}

/* now the simple read-only stuff */

static inline uint8_t waffle_to_fstitch_type(uint16_t type)
{
	switch(type & WAFFLE_S_IFMT)
	{
		case WAFFLE_S_IFDIR:
			return TYPE_DIR;
		case WAFFLE_S_IFREG:
			return TYPE_FILE;
		case WAFFLE_S_IFLNK:
			return TYPE_SYMLINK;	
		default:
			return TYPE_INVAL;
	}
}

static inline uint16_t fstitch_to_waffle_type(uint8_t type)
{
	switch(type)
	{
		case TYPE_DIR:
			return WAFFLE_S_IFDIR;
		case TYPE_FILE:
			return WAFFLE_S_IFREG;
		case TYPE_SYMLINK:
			return WAFFLE_S_IFLNK;
		default:
			return 0;
	}
}

/* NOTE: both 0 and INVALID_BLOCK may be returned as errors from this function */
static uint32_t waffle_get_inode_block(struct waffle_info * info, const struct waffle_inode * inode, uint32_t offset)
{
	bdesc_t * indirect;
	bdesc_t * dindirect;
	offset /= WAFFLE_BLOCK_SIZE;
	if(offset >= inode->i_blocks)
		/* asked for an offset past the last block */
		return INVALID_BLOCK;
	if(offset < WAFFLE_DIRECT_BLOCKS)
		return inode->i_direct[offset];
	if(offset < WAFFLE_INDIRECT_BLOCKS)
	{
		indirect = CALL(info->ubd, read_block, inode->i_indirect, 1, NULL);
		if(!indirect)
			return INVALID_BLOCK;
		return ((uint32_t *) bdesc_data(indirect))[offset - WAFFLE_DIRECT_BLOCKS];
	}
	dindirect = CALL(info->ubd, read_block, inode->i_dindirect, 1, NULL);
	if(!dindirect)
		return INVALID_BLOCK;
	indirect = CALL(info->ubd, read_block, ((uint32_t *) bdesc_data(dindirect))[(offset - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS], 1, NULL);
	if(!indirect)
		return INVALID_BLOCK;
	return ((uint32_t *) bdesc_data(indirect))[(offset - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS];
}

static int waffle_fetch_inode(struct waffle_info * info, struct waffle_fdesc * fdesc)
{
	uint32_t offset;
	assert(fdesc);
	assert(fdesc->f_inode >= WAFFLE_ROOT_INODE && fdesc->f_inode <= info->super->s_inodes);
	assert(!fdesc->f_inode_blkptr);
	
	offset = fdesc->f_inode * sizeof(struct waffle_inode);
	fdesc->f_inode_blkptr = waffle_get_data_blkptr(info, &info->s_active.sn_inode, NULL, offset);
	if(!fdesc->f_inode_blkptr)
		return -1;
	
	fdesc->f_inode_offset = offset % WAFFLE_BLOCK_SIZE;
	
	return 0;
}

/* returns 0 on error (including not found), otherwise the inode number */
static inode_t waffle_directory_search(struct waffle_info * info, struct waffle_fdesc * fdesc, const char * name, bdesc_t ** block_p, uint32_t * number_p, uint16_t * offset_p)
{
	uint32_t index;
	const struct waffle_inode * f_ip = f_ip(fdesc);
	for(index = 0; index < f_ip->i_size; index += WAFFLE_BLOCK_SIZE)
	{
		bdesc_t * block;
		uint16_t offset;
		uint32_t number = waffle_get_inode_block(info, f_ip, index);
		if(!number || number == INVALID_BLOCK)
			return 0;
		block = CALL(info->ubd, read_block, number, 1, NULL);
		if(!block)
			return 0;
		for(offset = 0; offset < WAFFLE_BLOCK_SIZE; offset += sizeof(struct waffle_dentry))
		{
			struct waffle_dentry * dirent = (struct waffle_dentry *) (bdesc_data(block) + offset);
			if(!dirent->d_inode)
				continue;
			if(strcmp(dirent->d_name, name))
				continue;
			if(block_p)
				*block_p = block;
			if(number_p)
				*number_p = number;
			if(offset_p)
				*offset_p = offset;
			return dirent->d_inode;
		}
	}
	return 0;
}

static int waffle_get_metadata(LFS_t * object, const struct waffle_fdesc * fd, uint32_t id, size_t size, void * data)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, fd, id);
	struct waffle_info * info = (struct waffle_info *) object;
	if(id == FSTITCH_FEATURE_SIZE)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = f_ip(fd)->i_size;
	}
	else if(id == FSTITCH_FEATURE_FILETYPE)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = fd->f_type;
	}
	else if(id == FSTITCH_FEATURE_FREESPACE)
	{
		struct waffle_info * info = (struct waffle_info *) object;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = info->free_blocks;
	}
	else if(id == FSTITCH_FEATURE_FILE_LFS)
	{
		if(size < sizeof(object))
			return -ENOMEM;
		size = sizeof(object);
		*((typeof(object) *) data) = object;
	}
	else if(id == FSTITCH_FEATURE_BLOCKSIZE)
	{
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = WAFFLE_BLOCK_SIZE;
	}
	else if(id == FSTITCH_FEATURE_DEVSIZE)
	{
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = info->super->s_blocks;
	}
	else if(id == FSTITCH_FEATURE_NLINKS)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = (uint32_t) f_ip(fd)->i_links;
	}
	else if(id == FSTITCH_FEATURE_UID)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = f_ip(fd)->i_uid;
	}
	else if(id == FSTITCH_FEATURE_GID)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = f_ip(fd)->i_gid;
	}
	else if(id == FSTITCH_FEATURE_UNIX_PERM)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint16_t))
			return -ENOMEM;
		size = sizeof(uint16_t);
		*((uint16_t *) data) = f_ip(fd)->i_mode & ~WAFFLE_S_IFMT;
	}
	else if(id == FSTITCH_FEATURE_MTIME)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = f_ip(fd)->i_mtime;
	}
	else if(id == FSTITCH_FEATURE_ATIME)
	{
		if(!fd)
			return -EINVAL;
		if(size < sizeof(uint32_t))
			return -ENOMEM;
		size = sizeof(uint32_t);
		*((uint32_t *) data) = f_ip(fd)->i_atime;
	}
	else if(id == FSTITCH_FEATURE_SYMLINK)
	{
		const struct waffle_inode * f_ip;
		struct waffle_info * info = (struct waffle_info *) object;
		if(!fd || fd->f_type != TYPE_SYMLINK)
			return -EINVAL;
		f_ip = f_ip(fd);
		/* fd->f_ip->i_size includes the zero byte */
		if(size < f_ip->i_size)
			return -ENOMEM;
		size = f_ip->i_size;
		if(size <= WAFFLE_INLINE_SIZE)
			memcpy(data, f_ip->i_inline, size);
		else
		{
			bdesc_t * symlink_block;
			symlink_block = CALL(info->ubd, read_block, f_ip->i_direct[0], 1, NULL);
			if(!symlink_block)
				return -1;
			memcpy(data, bdesc_data(symlink_block), f_ip->i_size);
		}
	}
	else
		return -EINVAL;
	
	return size;
}

static inode_t waffle_find_free_inode(struct waffle_info * info, struct blkptr ** inode_blkptr, uint16_t * i_offset)
{
	Dprintf("%s\n", __FUNCTION__);
	inode_t number;
	
	/* TODO: this is a really stupid way to do this; can we do better? */
	for(number = WAFFLE_ROOT_INODE + 1; number < info->super->s_inodes; number++)
	{
		uint32_t offset = number * sizeof(struct waffle_inode);
		const struct waffle_inode * inode;
		*inode_blkptr = waffle_get_data_blkptr(info, &info->s_active.sn_inode, NULL, offset);
		if(!*inode_blkptr)
			break;
		offset %= WAFFLE_BLOCK_SIZE;
		inode = (struct waffle_inode *) (blkptr_data(*inode_blkptr) + offset);
		if(!inode->i_links)
		{
			*i_offset = offset;
			return number;
		}
		waffle_put_blkptr(info, inode_blkptr);
	}
	
	*i_offset = -1;
	return -1;
}

static inline int waffle_update_inode(struct waffle_info * info, struct waffle_fdesc * fdesc, const struct waffle_inode * update)
{
	const struct waffle_inode * f_ip = f_ip(fdesc);
	return waffle_update_value(info, fdesc->f_inode_blkptr, f_ip, update, sizeof(*f_ip));
}

static inline int waffle_set_pointer(struct waffle_info * info, struct blkptr * indirect, uint32_t index, uint32_t block)
{
	return waffle_update_value(info, indirect, &((uint32_t *) blkptr_data(indirect))[index], &block, sizeof(block));
}

/* }}} */

/* LFS functions {{{ */

static int waffle_get_root(LFS_t * object, inode_t * inode)
{
	*inode = WAFFLE_ROOT_INODE;
	return 0;
}

static uint32_t waffle_allocate_block(LFS_t * object, fdesc_t * file, int purpose, patch_t ** tail)
{
	Dprintf("%s %p\n", __FUNCTION__, file);
	struct waffle_info * info = (struct waffle_info *) object;
	uint32_t number = waffle_find_free_block(info, info->try_next_free);
	if(number == INVALID_BLOCK)
		return INVALID_BLOCK;
	if(waffle_mark_allocated(info, number) < 0)
		return INVALID_BLOCK;
	return number;
}

static bdesc_t * waffle_lookup_block(LFS_t * object, uint32_t number, page_t * page)
{
	Dprintf("%s %u\n", __FUNCTION__, number);
	struct waffle_info * info = (struct waffle_info *) object;
	return CALL(info->ubd, read_block, number, 1, page);
}

static bdesc_t * waffle_synthetic_lookup_block(LFS_t * object, uint32_t number, page_t * page)
{
	Dprintf("%s %u\n", __FUNCTION__, number);
	struct waffle_info * info = (struct waffle_info *) object;
	return CALL(info->ubd, synthetic_read_block, number, 1, page);
}

static void waffle_free_fdesc(LFS_t * object, fdesc_t * fdesc);

static fdesc_t * waffle_lookup_inode(LFS_t * object, inode_t inode)
{
	Dprintf("%s %u\n", __FUNCTION__, inode);
	struct waffle_info * info = (struct waffle_info *) object;
	struct waffle_fdesc * fd = NULL;
	struct waffle_fdesc * oldest_fd = NULL;
	static uint32_t age = 0;
	int r, nincache = 0;
	
	if(inode <= 0)
		return NULL;
	
	if(!++age)
		++age;
	
	for(fd = info->filecache; fd; fd = fd->f_cache_next)
		if(fd->f_inode == inode)
		{
			fd->f_nopen += (fd->f_age ? 1 : 2);
			fd->f_age = age;
			return (fdesc_t *) fd;
		}
		else if(fd->f_age)
		{
			++nincache;
			if(!oldest_fd || (int32_t) (oldest_fd->f_age - fd->f_age) > 0)
				oldest_fd = fd;
		}
	
	fd = waffle_fdesc_pool_alloc();
	if(!fd)
		goto waffle_lookup_inode_exit;
	
	fd->common = &fd->base;
	fd->base.parent = INODE_NONE;
	fd->f_nopen = 1;
	fd->f_age = age;
	fd->f_inode = inode;
	fd->f_inode_blkptr = NULL;
	fd->f_inode_offset = -1;
	
	r = waffle_fetch_inode(info, fd);
	if(r < 0)
		goto waffle_lookup_inode_exit;
	fd->f_type = waffle_to_fstitch_type(f_ip(fd)->i_mode);
	
	/* stick in cache */
	if(oldest_fd && nincache >= 4)
	{
		oldest_fd->f_age = 0;
		waffle_free_fdesc(object, (fdesc_t *) oldest_fd);
	}
	fd->f_cache_pprev = &info->filecache;
	fd->f_cache_next = info->filecache;
	info->filecache = fd;
	if(fd->f_cache_next)
		fd->f_cache_next->f_cache_pprev = &fd->f_cache_next;
	
	return (fdesc_t *) fd;
	
  waffle_lookup_inode_exit:
	waffle_fdesc_pool_free(fd);
	return NULL;
}

static int waffle_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * inode)
{
	Dprintf("%s %u:%s\n", __FUNCTION__, parent, name);
	struct waffle_fdesc * fd = (struct waffle_fdesc *) waffle_lookup_inode(object, parent);
	if(!fd)
		return -EINVAL;
	*inode = waffle_directory_search((struct waffle_info *) object, fd, name, NULL, NULL, NULL);
	waffle_free_fdesc(object, (fdesc_t *) fd);
	return *inode ? 0 : -ENOENT;
}

static void __waffle_free_fdesc(struct waffle_info * info, struct waffle_fdesc * fdesc)
{
	assert(fdesc && !fdesc->f_nopen);
	if(fdesc->f_inode_blkptr)
		waffle_put_blkptr(info, &fdesc->f_inode_blkptr);
	if((*fdesc->f_cache_pprev = fdesc->f_cache_next))
		fdesc->f_cache_next->f_cache_pprev = fdesc->f_cache_pprev;
	waffle_fdesc_pool_free(fdesc);
}

static void waffle_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
	Dprintf("%s %p\n", __FUNCTION__, fdesc);
	struct waffle_fdesc * fd = (struct waffle_fdesc *) fdesc;
	if(fd && !--fd->f_nopen)
		__waffle_free_fdesc((struct waffle_info *) object, fd);
}

static uint32_t waffle_get_file_numblocks(LFS_t * object, fdesc_t * file)
{
	Dprintf("%s %p\n", __FUNCTION__, file);
	struct waffle_fdesc * fdesc = (struct waffle_fdesc *) file;
	const struct waffle_inode * f_ip = f_ip(fdesc);
	return f_ip->i_blocks;
}

/* XXX: we need to do some fancy stuff to support writing to the blocks returned by this */
static uint32_t waffle_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, file, offset);
	struct waffle_fdesc * fdesc = (struct waffle_fdesc *) file;
	return waffle_get_inode_block((struct waffle_info *) object, f_ip(fdesc), offset);
}

static int waffle_get_dirent(LFS_t * object, fdesc_t * file, struct dirent * entry, uint16_t size, uint32_t * basep)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, basep, *basep);
	struct waffle_info * info = (struct waffle_info *) object;
	struct waffle_fdesc * fd = (struct waffle_fdesc *) file;
	const struct waffle_inode * f_ip = f_ip(fd);
	uint32_t index = *basep * sizeof(struct waffle_dentry);
	uint16_t offset = index % WAFFLE_BLOCK_SIZE;
	if(fd->f_type != TYPE_DIR)
		return -ENOTDIR;
	for(index -= offset; index < f_ip->i_size; index += WAFFLE_BLOCK_SIZE)
	{
		bdesc_t * block;
		uint32_t number = waffle_get_inode_block(info, f_ip, index);
		if(!number || number == INVALID_BLOCK)
			return -ENOENT;
		block = CALL(info->ubd, read_block, number, 1, NULL);
		if(!block)
			return -ENOENT;
		for(; offset < WAFFLE_BLOCK_SIZE; offset += sizeof(struct waffle_dentry))
		{
			struct waffle_dentry * dirent = (struct waffle_dentry *) (bdesc_data(block) + offset);
			uint16_t namelen, reclen;
			if(!dirent->d_inode)
			{
				++*basep;
				continue;
			}
			namelen = strlen(dirent->d_name);
			reclen = sizeof(*entry) - sizeof(entry->d_name) + namelen + 1;
			if(size < reclen)
				return -ENOSPC;
			entry->d_fileno = dirent->d_inode;
			entry->d_reclen = reclen;
			entry->d_type = waffle_to_fstitch_type(dirent->d_type);
			entry->d_namelen = namelen;
			strcpy(entry->d_name, dirent->d_name);
			++*basep;
			return 0;
		}
		offset = 0;
	}
	/* end of directory */
	return -1;
}

static int waffle_append_file_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, file, block);
	struct waffle_info * info = (struct waffle_info *) object;
	struct waffle_fdesc * fd = (struct waffle_fdesc *) file;
	struct waffle_inode init = *f_ip(fd);
	struct blkptr * indirect;
	struct blkptr * dindirect;
	uint32_t * pointer;
	int r = 0;
	if(init.i_blocks < WAFFLE_DIRECT_BLOCKS)
	{
		init.i_direct[init.i_blocks++] = block;
		return waffle_update_inode(info, fd, &init);
	}
	
	/* inode has/needs indirect block */
	if(init.i_blocks == WAFFLE_DIRECT_BLOCKS)
	{
		uint32_t number = waffle_find_free_block(info, info->try_next_free);
		if(number == INVALID_BLOCK)
			return -ENOSPC;
		r = waffle_mark_allocated(info, number);
		if(r < 0)
			return r;
		init.i_indirect = number;
		r = waffle_update_inode(info, fd, &init);
		if(r < 0)
		{
			waffle_mark_deallocated(info, number);
			return r;
		}
	}
	if(init.i_blocks < WAFFLE_INDIRECT_BLOCKS)
	{
		indirect = waffle_follow_pointer(info, fd->f_inode_blkptr, &f_ip(fd)->i_indirect);
		if(!indirect)
		{
			r = -1;
			goto fail_undo_1;
		}
		if(waffle_in_snapshot(info, init.i_indirect))
		{
			r = waffle_clone_block(info, indirect);
			if(r < 0)
			{
				waffle_put_blkptr(info, &indirect);
				goto fail_undo_1;
			}
		}
		r = waffle_set_pointer(info, indirect, init.i_blocks - WAFFLE_DIRECT_BLOCKS, block);
		waffle_put_blkptr(info, &indirect);
		if(r < 0)
			goto fail_undo_1;
		init.i_blocks++;
		r = waffle_update_inode(info, fd, &init);
		if(r < 0)
			kpanic("unexpected error updating inode");
		return 0;
	}
	
	/* inode has/needs double indirect block */
	if(init.i_blocks == WAFFLE_INDIRECT_BLOCKS)
	{
		uint32_t number = waffle_find_free_block(info, info->try_next_free);
		if(number == INVALID_BLOCK)
			return -ENOSPC;
		r = waffle_mark_allocated(info, number);
		if(r < 0)
			return r;
		init.i_dindirect = number;
		r = waffle_update_inode(info, fd, &init);
		if(r < 0)
		{
			waffle_mark_deallocated(info, number);
			return r;
		}
	}
	dindirect = waffle_follow_pointer(info, fd->f_inode_blkptr, &f_ip(fd)->i_dindirect);
	if(!dindirect)
	{
		r = -1;
		goto fail_undo_1;
	}
	if(!((init.i_blocks - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS))
	{
		uint32_t number = waffle_find_free_block(info, info->try_next_free);
		if(number == INVALID_BLOCK)
		{
			r = -ENOSPC;
			goto fail_undo_2;
		}
		r = waffle_mark_allocated(info, number);
		if(r < 0)
			goto fail_undo_2;
		if(waffle_in_snapshot(info, init.i_dindirect))
		{
			r = waffle_clone_block(info, dindirect);
			if(r < 0)
			{
				waffle_mark_deallocated(info, number);
				goto fail_undo_2;
			}
		}
		r = waffle_set_pointer(info, dindirect, (init.i_blocks - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS, number);
		if(r < 0)
			goto fail_undo_2;
	}
	pointer = &((uint32_t *) blkptr_data(dindirect))[(init.i_blocks - WAFFLE_INDIRECT_BLOCKS) / WAFFLE_BLOCK_POINTERS];
	indirect = waffle_follow_pointer(info, dindirect, pointer);
	if(!indirect)
	{
		r = -1;
		goto fail_undo_3;
	}
	if(waffle_in_snapshot(info, indirect->number))
	{
		r = waffle_clone_block(info, indirect);
		if(r < 0)
			goto fail_undo_4;
	}
	r = waffle_set_pointer(info, indirect, (init.i_blocks - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS, block);
	if(r < 0)
		goto fail_undo_4;
	init.i_blocks++;
	r = waffle_update_inode(info, fd, &init);
	if(r < 0)
		kpanic("unexpected error updating inode");
	
	waffle_put_blkptr(info, &indirect);
	waffle_put_blkptr(info, &dindirect);
	return 0;
	
  fail_undo_4:
	waffle_put_blkptr(info, &indirect);
  fail_undo_3:
	if(!((init.i_blocks - WAFFLE_INDIRECT_BLOCKS) % WAFFLE_BLOCK_POINTERS))
		waffle_mark_deallocated(info, *pointer);
  fail_undo_2:
	waffle_put_blkptr(info, &dindirect);
  fail_undo_1:
	if(init.i_blocks == WAFFLE_INDIRECT_BLOCKS)
		waffle_mark_deallocated(info, init.i_dindirect);
	if(init.i_blocks == WAFFLE_DIRECT_BLOCKS)
		waffle_mark_deallocated(info, init.i_indirect);
	return r;
}

static fdesc_t * waffle_allocate_name(LFS_t * object, inode_t parent_inode, const char * name,
                                      uint8_t type, fdesc_t * link, const metadata_set_t * initialmd,
                                      inode_t * new_inode, patch_t ** head)
{
	Dprintf("%s %u:%s\n", __FUNCTION__, parent_inode, name);
	struct waffle_info * info = (struct waffle_info *) object;
	struct waffle_fdesc * fd;
	struct blkptr * inode_blkptr = NULL;
	struct waffle_fdesc * parent;
	int r = waffle_lookup_name(object, parent_inode, name, new_inode);
	if(r != -ENOENT)
		return NULL;
	if(link)
	{
		uint16_t i_links;
		const struct waffle_inode * f_ip;
		fd = (struct waffle_fdesc *) link;
		if(fd->f_type == TYPE_DIR)
			return NULL;
		/* increase refcount */
		f_ip = f_ip(fd);
		i_links = f_ip->i_links + 1;
		r = waffle_update_value(info, fd->f_inode_blkptr, &f_ip->i_links, &i_links, sizeof(f_ip->i_links));
		if(r < 0)
			return NULL;
		*new_inode = fd->f_inode;
		type = fd->f_type;
	}
	else
	{
		uint32_t x32;
		uint16_t x16, inode_offset;
		struct waffle_inode init;
		const struct waffle_inode * f_ip;
		memset(&init, 0, sizeof(init));
		init.i_mode = fstitch_to_waffle_type(type);
		if(!init.i_mode)
			return NULL;
		init.i_mode |= WAFFLE_S_IRUSR | WAFFLE_S_IWUSR;
		init.i_links = 1;
		init.i_size = 0;
		init.i_blocks = 0;
		
		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_UNIX_PERM, sizeof(x16), &x16);
		if(r > 0)
			init.i_mode = (init.i_mode & WAFFLE_S_IFMT) | x16;
		else if(r != -ENOENT)
			assert(0);
		
		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_UID, sizeof(x32), &x32);
		if(r > 0)
			init.i_uid = x32;
		else if(r == -ENOENT)
			init.i_uid = 0;
		else
			assert(0);
		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_GID, sizeof(x32), &x32);
		if(r > 0)
			init.i_gid = x32;
		else if(r == -ENOENT)
			init.i_gid = 0;
		else
			assert(0);
		
		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_ATIME, sizeof(x32), &x32);
		if(r > 0)
			init.i_atime = x32;
		else if(r == -ENOENT)
			init.i_atime = 0;
		else
			assert(0);
		r = initialmd->get(initialmd->arg, FSTITCH_FEATURE_MTIME, sizeof(x32), &x32);
		if(r > 0)
			init.i_mtime = x32;
		else if(r == -ENOENT)
			init.i_mtime = 0;
		else
			assert(0);
		
		/* set up initial inode */
		*new_inode = waffle_find_free_inode(info, &inode_blkptr, &inode_offset);
		if(*new_inode <= WAFFLE_ROOT_INODE)
			return NULL;
		
		if(type == TYPE_DIR)
		{
			bdesc_t * block;
			patch_t * patch = NULL;
			struct waffle_dentry dots[2];
			uint32_t number = waffle_find_free_block(info, info->try_next_free);
			if(number == INVALID_BLOCK)
				kpanic("TODO: better error handling");
			assert(!waffle_in_snapshot(info, number));
			init.i_links++;
			init.i_size += WAFFLE_BLOCK_SIZE;
			init.i_direct[init.i_blocks++] = number;
			block = CALL(info->ubd, synthetic_read_block, number, 1, NULL);
			if(!block)
				kpanic("TODO: better error handling");
			r = patch_create_init(block, info->ubd, &patch);
			if(r < 0)
				kpanic("TODO: better error handling");
			memset(dots, 0, sizeof(dots));
			dots[0].d_inode = *new_inode;
			dots[0].d_type = WAFFLE_S_IFDIR;
			strcpy(dots[0].d_name, ".");
			dots[1].d_inode = parent_inode;
			dots[1].d_type = WAFFLE_S_IFDIR;
			strcpy(dots[1].d_name, "..");
			r = patch_create_byte(block, info->ubd, 0, sizeof(dots), &dots, &patch);
			if(r < 0)
				kpanic("TODO: better error handling");
			r = patch_add_depend(info->checkpoint_changes, patch);
			if(r < 0)
				kpanic("TODO: better error handling");
			r = CALL(info->ubd, write_block, block, number);
			if(r < 0)
				kpanic("TODO: better error handling");
			r = waffle_mark_allocated(info, number);
			if(r < 0)
				kpanic("TODO: better error handling");
			parent = (struct waffle_fdesc *) waffle_lookup_inode(object, parent_inode);
			if(!parent)
				kpanic("TODO: better error handling");
			f_ip = f_ip(parent);
			x16 = f_ip->i_links + 1;
			r = waffle_update_value(info, parent->f_inode_blkptr, &f_ip->i_links, &x16, sizeof(&f_ip->i_links));
			waffle_free_fdesc(object, (fdesc_t *) parent);
			if(r < 0)
				kpanic("TODO: better error handling");
		}
		
		/* FIXME: handle symlinks */
		if(type != TYPE_FILE && type != TYPE_DIR)
			kpanic("only files and directories supported right now");
		
		r = waffle_update_value(info, inode_blkptr, blkptr_data(inode_blkptr) + inode_offset, &init, sizeof(*f_ip));
		waffle_put_blkptr(info, &inode_blkptr);
		if(r < 0)
			return NULL;
	}
	/* we now would have to reset the inode if we encounter any failures... */
	
	parent = (struct waffle_fdesc *) waffle_lookup_inode(object, parent_inode);
	if(!parent)
		kpanic("unrecoverable failure after inode update");
	r = waffle_add_dentry(info, parent, name, *new_inode, fstitch_to_waffle_type(type));
	if(r < 0)
		kpanic("unrecoverable failure after inode update");
	waffle_free_fdesc(object, (fdesc_t *) parent);
	
	return waffle_lookup_inode(object, *new_inode);
}

static int waffle_lookup_name(LFS_t * object, inode_t parent, const char * name, inode_t * inode);

static int waffle_rename(LFS_t * object, inode_t oldparent, const char * oldname, inode_t newparent, const char * newname, patch_t ** head)
{
	Dprintf("%s %u:%s -> %u:%s\n", __FUNCTION__, oldparent, oldname, newparent, newname);
	struct waffle_info * info = (struct waffle_info *) object;
	struct waffle_fdesc * file = NULL;
	struct waffle_fdesc * parent = NULL;
	inode_t inode;
	int r = waffle_lookup_name(object, oldparent, oldname, &inode);
	if(r < 0)
		return r;
	file = (struct waffle_fdesc *) waffle_lookup_inode(object, inode);
	if(!file)
		return -1;
	
	parent = (struct waffle_fdesc *) waffle_lookup_inode(object, newparent);
	if(!parent)
	{
		r = -1;
		goto out;
	}
	r = waffle_add_dentry(info, parent, newname, inode, fstitch_to_waffle_type(file->f_type));
	if(r < 0)
		goto out;
	waffle_free_fdesc(object, (fdesc_t *) parent);
	
	parent = (struct waffle_fdesc *) waffle_lookup_inode(object, oldparent);
	if(!parent)
	{
		r = -1;
		goto out;
	}
	r = waffle_clear_dentry(info, parent, oldname);
	if(r < 0)
		kpanic("unrecoverable failure after link creation");
	
  out:
	if(file)
		waffle_free_fdesc(object, (fdesc_t *) file);
	if(parent)
		waffle_free_fdesc(object, (fdesc_t *) parent);
	return r;
}

static uint32_t waffle_truncate_file_block(LFS_t * object, fdesc_t * file, patch_t ** head)
{
	Dprintf("%s %p\n", __FUNCTION__, file);
	/* FIXME */
	return INVALID_BLOCK;
}

static int waffle_free_block(LFS_t * object, fdesc_t * file, uint32_t block, patch_t ** head)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, file, block);
	struct waffle_info * info = (struct waffle_info *) object;
	return waffle_mark_deallocated(info, block);
}

static int waffle_remove_name(LFS_t * object, inode_t parent, const char * name, patch_t ** head)
{
	Dprintf("%s %u:%s\n", __FUNCTION__, parent, name);
	/* FIXME */
	return -ENOSYS;
}

static int waffle_write_block(LFS_t * object, bdesc_t * block, uint32_t number, patch_t ** head)
{
	Dprintf("%s %u\n", __FUNCTION__, number);
	struct waffle_info * info = (struct waffle_info *) object;
	
	if(waffle_in_snapshot(info, number))
		/* XXX: we must do COW here! */
		kpanic("can't write blocks still in a snapshot yet");
	
	/* FIXME: add dependencies from checkpoint_changes -> (patches on block) */
	
	return CALL(info->ubd, write_block, block, number);
}

static patch_t ** waffle_get_write_head(LFS_t * object)
{
	struct waffle_info * info = (struct waffle_info *) object;
	return info->write_head;
}

static int32_t waffle_get_block_space(LFS_t * object)
{
	struct waffle_info * info = (struct waffle_info *) object;
	return CALL(info->ubd, get_block_space);
}

static const bool waffle_features[] = {[FSTITCH_FEATURE_SIZE] = 1, [FSTITCH_FEATURE_FILETYPE] = 1, [FSTITCH_FEATURE_FREESPACE] = 1, [FSTITCH_FEATURE_FILE_LFS] = 1, [FSTITCH_FEATURE_BLOCKSIZE] = 1, [FSTITCH_FEATURE_DEVSIZE] = 1, [FSTITCH_FEATURE_MTIME] = 1, [FSTITCH_FEATURE_ATIME] = 1, [FSTITCH_FEATURE_GID] = 1, [FSTITCH_FEATURE_UID] = 1, [FSTITCH_FEATURE_UNIX_PERM] = 1, [FSTITCH_FEATURE_NLINKS] = 1, [FSTITCH_FEATURE_SYMLINK] = 1, [FSTITCH_FEATURE_DELETE] = 1};

static size_t waffle_get_max_feature_id(LFS_t * object)
{
	return sizeof(waffle_features) / sizeof(waffle_features[0]) - 1;
}

static const bool * waffle_get_feature_array(LFS_t * object)
{
	return waffle_features;
}

static int waffle_get_metadata_inode(LFS_t * object, inode_t inode, uint32_t id, size_t size, void * data)
{
	Dprintf("%s %u, %u\n", __FUNCTION__, inode, id);
	const struct waffle_fdesc * fd = (struct waffle_fdesc *) waffle_lookup_inode(object, inode);
	int r = waffle_get_metadata(object, fd, id, size, data);
	if(fd)
		waffle_free_fdesc(object, (fdesc_t *) fd);
	return r;
}

static int waffle_get_metadata_fdesc(LFS_t * object, const fdesc_t * file, uint32_t id, size_t size, void * data)
{
	Dprintf("%s %p, %u\n", __FUNCTION__, file, id);
	const struct waffle_fdesc * fd = (struct waffle_fdesc *) file;
	return waffle_get_metadata(object, fd, id, size, data);
}

static int waffle_set_metadata2(LFS_t * object, struct waffle_fdesc * fd, const fsmetadata_t * fsm, size_t nfsm, patch_t ** head)
{
	Dprintf("%s %u\n", __FUNCTION__, fd->f_inode);
	struct waffle_info * info = (struct waffle_info *) object;
	assert(head && fd && (!nfsm || fsm));
	
	struct waffle_inode init = *f_ip(fd);
	
  retry:
	if(!nfsm)
		return waffle_update_inode(info, fd, &init);
	
	if(fsm->fsm_feature == FSTITCH_FEATURE_SIZE)
	{
		if(fd->f_type == TYPE_DIR)
			return -EINVAL;
		if(fsm->fsm_value.u >= WAFFLE_MAX_FILE_SIZE)
			return -EINVAL;
		init.i_size = fsm->fsm_value.u; 
	}
	else if(fsm->fsm_feature == FSTITCH_FEATURE_FILETYPE)
	{
		uint32_t fs_type;
		switch(fsm->fsm_value.u)
		{
			case TYPE_FILE:
				fs_type = WAFFLE_S_IFREG;
				break;
			case TYPE_DIR:
				fs_type = WAFFLE_S_IFDIR;
				break;
			default:
				return -EINVAL;
		}
		
		init.i_mode = (init.i_mode & ~WAFFLE_S_IFMT) | (fs_type);
		fd->f_type = fsm->fsm_value.u;
	}
	else if(fsm->fsm_feature == FSTITCH_FEATURE_UID)
		init.i_uid = fsm->fsm_value.u;
	else if(fsm->fsm_feature == FSTITCH_FEATURE_GID)
		init.i_gid = fsm->fsm_value.u;
	else if(fsm->fsm_feature == FSTITCH_FEATURE_UNIX_PERM)
		init.i_mode = (init.i_mode & WAFFLE_S_IFMT) | (fsm->fsm_value.u & ~WAFFLE_S_IFMT);
	else if(fsm->fsm_feature == FSTITCH_FEATURE_MTIME)
		init.i_mtime = fsm->fsm_value.u;
	else if(fsm->fsm_feature == FSTITCH_FEATURE_ATIME)
		init.i_atime = fsm->fsm_value.u;
	else if(fsm->fsm_feature == FSTITCH_FEATURE_SYMLINK)
	{
		if(fd->f_type != TYPE_SYMLINK)
			return -EINVAL;
		/* FIXME: implement symlinks */
		return -1;
	}
	else
		return -1;
	
	fsm++;
	nfsm--;
	goto retry;
}

static int waffle_set_metadata2_inode(LFS_t * object, inode_t inode, const fsmetadata_t * fsm, size_t nfsm, patch_t ** head)
{
	Dprintf("%s %u\n", __FUNCTION__, inode);
	int r;
	struct waffle_fdesc * fd = (struct waffle_fdesc *) waffle_lookup_inode(object, inode); 
	if(!fd)
		return -1;
	r = waffle_set_metadata2(object, fd, fsm, nfsm, head);
	waffle_free_fdesc(object, (fdesc_t *) fd);
	return r;
}

static int waffle_set_metadata2_fdesc(LFS_t * object, fdesc_t * file, const fsmetadata_t * fsm, size_t nfsm, patch_t ** head)
{
	Dprintf("%s %p\n", __FUNCTION__, file);
	struct waffle_fdesc * fd = (struct waffle_fdesc *) file;
	return waffle_set_metadata2(object, fd, fsm, nfsm, head);
}

static void waffle_callback(void * arg)
{
	struct waffle_info * info = (struct waffle_info *) arg;
	struct waffle_old_snapshot * old_snapshot;
	patch_t * patch = info->checkpoint_changes;
	int r;
	if(!info->cloned_since_checkpoint)
		return;
	old_snapshot = waffle_snapshot_pool_alloc();
	if(!old_snapshot)
	{
		fprintf(stderr, "%s(): warning: failed to allocate snapshot!\n", __FUNCTION__);
		return;
	}
	/* save the old snapshot */
	WEAK_INIT(old_snapshot->overwrite);
	old_snapshot->bitmap = info->checkpoint;
	old_snapshot->snapshot = info->super->s_checkpoint;
	old_snapshot->next = info->old_snapshots;
	
	r = patch_create_byte_atomic(info->super_cache, info->ubd, offsetof(struct waffle_super, s_checkpoint), sizeof(struct waffle_snapshot), &info->s_active, &patch);
	if(r < 0)
	{
		waffle_snapshot_pool_free(old_snapshot);
		fprintf(stderr, "%s(): warning: failed to create checkpoint!\n", __FUNCTION__);
		return;
	}
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, patch, "checkpoint");
	/* weak retain the new checkpoint so we know when the old one is no longer on disk */
	patch_weak_retain(patch, &old_snapshot->overwrite, NULL, NULL);
	info->old_snapshots = old_snapshot;
	info->checkpoint = info->active;
	/* increase the reference count of the bitmap cache we copied */
	if(info->checkpoint.bb_cache)
		bdesc_retain(info->checkpoint.bb_cache);
	
	patch_satisfy(&info->checkpoint_tail);
	/* for safety, in case we fail below and try to use it after that */
	info->checkpoint_changes = NULL;
	r = CALL(info->ubd, write_block, info->super_cache, WAFFLE_SUPER_BLOCK);
	if(r < 0)
		fprintf(stderr, "%s(): warning: failed to write superblock!\n", __FUNCTION__);
	r = patch_create_empty_list(NULL, &info->checkpoint_tail, NULL);
	if(r < 0)
		kpanic("failed to create new checkpoint: %d", r);
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->checkpoint_tail, "checkpoint tail");
	patch_claim_empty(info->checkpoint_tail);
	r = patch_create_empty_list(NULL, &info->checkpoint_changes, info->checkpoint_tail, NULL);
	if(r < 0)
		kpanic("failed to create new checkpoint: %d", r);
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->checkpoint_changes, "checkpoint changes");
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, info->checkpoint_changes, PATCH_SET_EMPTY);
	info->checkpoint_changes->flags |= PATCH_SET_EMPTY;
	info->cloned_since_checkpoint = 0;
}

static int waffle_destroy(LFS_t * lfs)
{
	struct waffle_info * info = (struct waffle_info *) lfs;
	struct waffle_fdesc * fd;
	int r;
	
	if(info->fdesc_count)
		return -EBUSY;
	
	r = modman_rem_lfs(lfs);
	if(r < 0)
		return r;
	modman_dec_bd(info->ubd, lfs);
	
	r = sched_unregister(waffle_callback, info);
	/* should not fail */
	assert(r >= 0);
	
	if(info->cloned_since_checkpoint)
		waffle_callback(info);
	
	if(info->checkpoint_changes->befores->before.next)
		fprintf(stderr, "%s(): warning: checkpoint changes still exist!\n", __FUNCTION__);
	patch_satisfy(&info->checkpoint_tail);
	
	while(info->old_snapshots)
	{
		struct waffle_old_snapshot * old = info->old_snapshots;
		info->old_snapshots = old->next;
		if(WEAK(old->overwrite))
			patch_weak_release(&old->overwrite, 0);
		if(old->bitmap.bb_cache)
			bdesc_release(&old->bitmap.bb_cache);
		waffle_snapshot_pool_free(old);
	}
	
	for(fd = info->filecache; fd; fd = fd->f_cache_next)
		assert(fd->f_nopen == 1 && fd->f_age != 0);
	while(info->filecache)
		waffle_free_fdesc(lfs, (fdesc_t *) info->filecache);
	if(!hash_map_empty(info->blkptr_map))
		fprintf(stderr, "%s(): warning: blkptr hash map is not empty!\n", __FUNCTION__);
	hash_map_destroy(info->blkptr_map);
	if(info->active.bb_cache)
		bdesc_release(&info->active.bb_cache);
	if(info->checkpoint.bb_cache)
		bdesc_release(&info->checkpoint.bb_cache);
	for(r = 0; r < WAFFLE_SNAPSHOT_COUNT; r++)
		if(info->snapshot[r].bb_cache)
			bdesc_release(&info->snapshot[r].bb_cache);
	if(info->super_cache)
		bdesc_release(&info->super_cache);
	
	if(!--n_waffle_instances)
	{
		waffle_snapshot_pool_free_all();
		waffle_fdesc_pool_free_all();
		waffle_blkptr_pool_free_all();
	}
	
	memset(info, 0, sizeof(*info));
	free(info);
	
	return 0;
}

/* }}} */

/* constructor {{{ */

LFS_t * waffle_lfs(BD_t * block_device)
{
	Dprintf("%s\n", __FUNCTION__);
	struct waffle_info * info;
	LFS_t * lfs;
	int i;
	
	if(!block_device)
		return NULL;
	if(block_device->blocksize != WAFFLE_BLOCK_SIZE)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	
	lfs = &info->lfs;
	LFS_INIT(lfs, waffle);
	OBJMAGIC(lfs) = WAFFLE_FS_MAGIC;
	
	lfs->blocksize = WAFFLE_BLOCK_SIZE;
	info->ubd = lfs->blockdev = block_device;
	info->write_head = CALL(block_device, get_write_head);
	info->active.bb_cache = NULL;
	info->active.bb_number = INVALID_BLOCK;
	info->active.bb_index = INVALID_BLOCK;
	info->checkpoint = info->active;
	for(i = 0; i < WAFFLE_SNAPSHOT_COUNT; i++)
		info->snapshot[i] = info->active;
	info->free_blocks = 0;
	info->filecache = NULL;
	info->blkptr_map = hash_map_create();
	if(!info->blkptr_map)
		goto fail_info;
	info->fdesc_count = 0;
	
	/* superblock */
	info->super_cache = CALL(info->ubd, read_block, WAFFLE_SUPER_BLOCK, 1, NULL);
	if(!info->super_cache)
		goto fail_hash;
	bdesc_retain(info->super_cache);
	info->super = (struct waffle_super *) bdesc_data(info->super_cache);
	info->s_active = info->super->s_checkpoint;
	info->old_snapshots = NULL;
	info->cloned_since_checkpoint = 0;
	/* TODO: something better than this could be nice */
	info->try_next_free = WAFFLE_SUPER_BLOCK + 1;
	
	if(patch_create_empty_list(NULL, &info->checkpoint_tail, NULL) < 0)
		goto fail_super;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->checkpoint_tail, "checkpoint tail");
	patch_claim_empty(info->checkpoint_tail);
	if(patch_create_empty_list(NULL, &info->checkpoint_changes, info->checkpoint_tail, NULL) < 0)
		goto fail_tail;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, info->checkpoint_changes, "checkpoint changes");
	FSTITCH_DEBUG_SEND(FDB_MODULE_PATCH_ALTER, FDB_PATCH_SET_FLAGS, info->checkpoint_changes, PATCH_SET_EMPTY);
	info->checkpoint_changes->flags |= PATCH_SET_EMPTY;
	if(sched_register(waffle_callback, info, 10 * HZ) < 0)
		goto fail_tail;
	
	/* FIXME: count the free blocks */
	
	n_waffle_instances++;
	printf("Mounted waffle file system: %u blocks, %u inodes\n", info->super->s_blocks, info->super->s_inodes);
	
	if(modman_add_anon_lfs(lfs, __FUNCTION__))
	{
		DESTROY(lfs);
		return NULL;
	}
	if(modman_inc_bd(block_device, lfs, NULL) < 0)
	{
		modman_rem_lfs(lfs);
		DESTROY(lfs);
		return NULL;
	}
	
	return lfs;
	
  fail_tail:
	patch_satisfy(&info->checkpoint_tail);
  fail_super:
	bdesc_release(&info->super_cache);
  fail_hash:
	hash_map_destroy(info->blkptr_map);
  fail_info:
	free(info);
	return NULL;
}

/* }}} */
