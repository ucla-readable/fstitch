#include <inc/lib.h>
#include <inc/types.h>
#include <inc/x86.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/josfs_base.h>
#include <kfs/josfs_fdesc.h>

int block_is_free(LFS_t * object, uint32_t blockno);
int read_bitmap(LFS_t * object, uint32_t blockno);
int write_bitmap(LFS_t * object, uint32_t blockno);

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

    bdesc = CALL(((struct lfs_info *) object->instance)->ubd, read_block, 0);

    if (bdesc->length < sizeof(struct JOS_Super)) {
        printf("josfs_base: Didn't read back enough data\n");
        return NULL;
    }

    super = (struct JOS_Super*) bdesc->data;
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

int block_is_free(LFS_t * object, uint32_t blockno)
{
    int r;
    if ((r = read_bitmap(object, blockno)) == -1) {
        return -1;
    }

    return !r;
}

// Return 1 if block is in use
int read_bitmap(LFS_t * object, uint32_t blockno)
{
    // FIXME gotta figure out how to extract the bit
    bdesc_t * bdesc;
    int blocksize;
    int target;
//    int int32_offset;
//    int bit_offset;
//    uint32_t * ptr;

    blocksize = CALL(((struct lfs_info *) object->instance)->ubd, get_blocksize);
    target = (8192 + blockno) / (blocksize*8);
//    int32_offset = (8192 + blockno) % (blocksize*8);

    bdesc = CALL(((struct lfs_info *) object->instance)->ubd, read_block, target);

    if (bdesc->length != blocksize) {
        printf("josfs_base: trouble reading bitmap!\n");
        return -1;
    }

    return 0;
}

int write_bitmap(LFS_t * object, uint32_t blockno)
{
    // Fix me
    return 0;
}

static bdesc_t * josfs_allocate_block(LFS_t * object, uint32_t size, int purpose)
{
    return 0;
}

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number, uint32_t offset, uint32_t size)
{
    return 0;
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

static int josfs_get_metadata(LFS_t * object, fdesc_t * file, uint32_t id, size_t ** size, void * data)
{
    return 0;
}

static int josfs_set_metadata(LFS_t * object, fdesc_t * file, uint32_t id, size_t size, const void * data)
{
    return 0;
}

static int josfs_sync(LFS_t * object, fdesc_t * file)
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
    if (s == NULL)
        return NULL;

    b = check_bitmap(lfs);
    if (b == 1)
        return NULL;

    ((struct lfs_info *) lfs->instance)->super = s;

    return lfs;
}

