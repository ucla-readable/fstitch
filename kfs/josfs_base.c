#include <inc/lib.h>
#include <inc/types.h>
#include <inc/x86.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/josfs_fdesc.h>

#define block_is_free read_bitmap

int read_bitmap(LFS_t * object, uint32_t blockno);
int write_bitmap(LFS_t * object, uint32_t blockno, bool value);

struct lfs_info
{
    BD_t * ubd;
    struct JOS_Super * super;
};

// Equivalent to JOS's read_super
struct JOS_Super * check_super(LFS_t * object)
{
    struct JOS_Super* super;
    uint32_t numblocks;
    bdesc_t * bdesc;

    if (CALL(((struct lfs_info *) object->instance)->ubd, get_blocksize) != BLKSIZE) {
        printf("Block device size is not BLKSIZE!\n");
        return NULL;
    }

    bdesc = CALL(((struct lfs_info *) object->instance)->ubd, read_block, 0);

    if (bdesc->length < sizeof(struct JOS_Super)) {
        printf("josfs_base: Didn't read back enough data\n");
        return NULL;
    }

    super = (struct JOS_Super*)(bdesc->data + bdesc->offset);
    if (super->s_magic != FS_MAGIC) {
        printf("josfs_base: bad file system magic number\n");
        return NULL;
    }

    numblocks = CALL(((struct lfs_info *) object->instance)->ubd, get_numblocks);

    if (super->s_nblocks > numblocks) {
        printf("josfs_base: file system is too large\n");
        return NULL;
    }

    return super;
}

// Equivalent to JOS's read_bitmap
int check_bitmap(LFS_t * object)
{
    int i;
    int blocks_to_read;

    blocks_to_read = ((struct lfs_info *) object->instance)->super->s_nblocks / BLKBITSIZE;

    if (((struct lfs_info *) object->instance)->super->s_nblocks % BLKBITSIZE) {
        blocks_to_read++;
    }

    // Make sure the reserved and root blocks are marked in-use.
    if (block_is_free(object, 0) || block_is_free(object, 1)) {
        printf("josfs_base: Boot Sector or Parition Table marked free!\n");
        return 1;
    }

    // Make sure that the bitmap blocks are marked in-use.
    for (i = 0; i < blocks_to_read; i++) {
        if (block_is_free(object, 2+i)) {
            printf("josfs_base: Free Block Bitmap block %d marked free!\n");
            return 1;
        }
    }

    return 0;
}

// Return 1 if block is free
int read_bitmap(LFS_t * object, uint32_t blockno)
{
    bdesc_t * bdesc;
    int target;
    uint32_t * ptr;

    target = 2 + (blockno / (BLKSIZE*8));

    bdesc = CALL(((struct lfs_info *) object->instance)->ubd, read_block, target);

    if (bdesc->length != BLKSIZE) {
        printf("josfs_base: trouble reading bitmap!\n");
        return -1;
    }

    ptr = ((uint32_t *)bdesc->data) + (blockno / 32);
    if (*ptr & (1 << (blockno % 32)))
        return 1;
    return 0;
}

int write_bitmap(LFS_t * object, uint32_t blockno, bool value)
{
    bdesc_t * bdesc;
    int target;
    int r;
    uint32_t * ptr;

    if (blockno == 0) {
        printf("josfs_base: attempted to free zero block!\n");
        return -1;
    }

    target = 2 + (blockno / (BLKSIZE*8));

    bdesc = CALL(((struct lfs_info *) object->instance)->ubd, read_block, target);

    if (bdesc->length != BLKSIZE) {
        printf("josfs_base: trouble reading bitmap!\n");
        return -1;
    }

    ptr = ((uint32_t *)bdesc->data) + (blockno / 32);
    if (value)
        *ptr |= (1 << (blockno % 32));
    else
        *ptr &= ~(1 << (blockno % 32));


    if ((r = CALL(((struct lfs_info *) object->instance)->ubd, write_block, bdesc)) < 0)
        return r;

    return 0;
}

static bdesc_t * josfs_allocate_block(LFS_t * object, uint32_t size, int purpose)
{
    int blockno;
    int bitmap_size;
    int s_nblocks;

    s_nblocks = ((struct lfs_info *) object->instance)->super->s_nblocks;
    bitmap_size = s_nblocks / BLKBITSIZE;

    if (s_nblocks % BLKBITSIZE) {
        bitmap_size++;
    }

    for (blockno = 2 + bitmap_size; blockno < s_nblocks; blockno++) {
        if (block_is_free(object, blockno) == 1) {
            write_bitmap(object, blockno, 0);
            return bdesc_alloc(((struct lfs_info *) object->instance)->ubd, blockno, 0, BLKSIZE);
        }
    }

    return NULL;
}

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number, uint32_t offset, uint32_t size)
{
    if (offset != 0 || size != BLKSIZE)
        return NULL;
    
    return bdesc_alloc(((struct lfs_info *) object->instance)->ubd, number, 0, BLKSIZE);
}

static fdesc_t * josfs_lookup_name(LFS_t * object, const char * name)
{
    return 0;
}

static bdesc_t * josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
    return 0;
}

static int josfs_append_file_block(LFS_t * object, fdesc_t * file, bdesc_t * block)
{
    return 0;
}

static fdesc_t * josfs_allocate_name(LFS_t * object, char * name, int type, fdesc_t * link)
{
    return 0;
}

static int josfs_rename(LFS_t * object, const char * oldname, const char * newname)
{
    return 0;
}

static bdesc_t * josfs_truncate_file_block(LFS_t * object, fdesc_t * file)
{
    return 0;
}

static int josfs_free_block(LFS_t * object, bdesc_t * block)
{
    if (block->number == 0)
        return -1;
    write_bitmap(object, block->number, 1);
    return 0;
}

static int josfs_apply_changes(LFS_t * object, chdesc_t * changes)
{
    return 0;
}

static int josfs_remove_name(LFS_t * object, const char * name)
{
    return 0;
}

static int josfs_write_block(LFS_t * object, bdesc_t * block, uint32_t offset, uint32_t size, void * data)
{
    return 0;
}

static const feature_t * josfs_get_features(LFS_t * object)
{
    return 0;
}

static int josfs_get_metadata(LFS_t * object, const char * name, uint32_t id, size_t * size, void ** data)
{
    return 0;
}

static int josfs_set_metadata(LFS_t * object, const char * name, uint32_t id, size_t size, const void * data)
{
    return 0;
}

static int josfs_sync(LFS_t * object, const char * name)
{
	return 0;
}

static int josfs_destroy(LFS_t * lfs)
{
    free(lfs->instance);
    memset(lfs, 0, sizeof(*lfs));
    free(lfs);
    return 0;
}

LFS_t * josfs(BD_t * block_device)
{
    bool b;
    struct JOS_Super * s;
    LFS_t * lfs = malloc(sizeof(*lfs));

    if (!lfs)
        return NULL;

    lfs->instance = malloc(sizeof(struct lfs_info));
    if(!lfs->instance)
    {
        free(lfs);
        return NULL;
    }

    ASSIGN(lfs, josfs, allocate_block);
    ASSIGN(lfs, josfs, lookup_block);
    ASSIGN(lfs, josfs, lookup_name);
    ASSIGN(lfs, josfs, get_file_block);
    ASSIGN(lfs, josfs, append_file_block);
    ASSIGN(lfs, josfs, allocate_name);
    ASSIGN(lfs, josfs, rename);
    ASSIGN(lfs, josfs, truncate_file_block);
    ASSIGN(lfs, josfs, free_block);
    ASSIGN(lfs, josfs, apply_changes);
    ASSIGN(lfs, josfs, remove_name);
    ASSIGN(lfs, josfs, write_block);
    ASSIGN(lfs, josfs, get_features);
    ASSIGN(lfs, josfs, get_metadata);
    ASSIGN(lfs, josfs, set_metadata);
    ASSIGN(lfs, josfs, sync);
    ASSIGN_DESTROY(lfs, josfs, destroy);

    ((struct lfs_info *) lfs->instance)->ubd = block_device;
    s = check_super(lfs);
    if (s == NULL) {
        free(lfs);
        return NULL;
    }

    b = check_bitmap(lfs);
    if (b == 1) {
        free(lfs);
        return NULL;
    }

    ((struct lfs_info *) lfs->instance)->super = s;

    return lfs;
}

