/* Avoid #including <inc/lib.h> to keep <inc/fs.h> out */
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
    bdesc_t * super_block;
    struct JOS_Super * super;
};

// Equivalent to JOS's read_super
int check_super(LFS_t * object)
{
    struct lfs_info * info = (struct lfs_info *) object->instance;
    uint32_t numblocks;

    /* make sure we have the block size we expect */
    if (CALL(info->ubd, get_blocksize) != JOS_BLKSIZE) {
        printf("Block device size is not BLKSIZE!\n");
        return -1;
    }

    /* the superblock is in block 1 */
    info->super_block = CALL(info->ubd, read_block, 1);
    bdesc_retain(&info->super_block);

    info->super = (struct JOS_Super*) info->super_block->data;
    if (info->super->s_magic != JOS_FS_MAGIC) {
        printf("josfs_base: bad file system magic number\n");
	bdesc_release(&info->super_block);
        return -1;
    }

    numblocks = CALL(info->ubd, get_numblocks);

    if (info->super->s_nblocks > numblocks) {
        printf("josfs_base: file system is too large\n");
	bdesc_release(&info->super_block);
        return -1;
    }

    return 0;
}

// Equivalent to JOS's read_bitmap
int check_bitmap(LFS_t * object)
{
    struct lfs_info * info = (struct lfs_info *) object->instance;
    int i, blocks_to_read;

    blocks_to_read = info->super->s_nblocks / JOS_BLKBITSIZE;

    if (info->super->s_nblocks % JOS_BLKBITSIZE) {
        blocks_to_read++;
    }

    // Make sure the reserved and root blocks are marked in-use.
    if (block_is_free(object, 0) || block_is_free(object, 1)) {
        printf("josfs_base: Boot Sector or Parition Table marked free!\n");
        return -1;
    }

    // Make sure that the bitmap blocks are marked in-use.
    for (i = 0; i < blocks_to_read; i++) {
        if (block_is_free(object, 2+i)) {
            printf("josfs_base: Free Block Bitmap block %d marked free!\n");
            return -1;
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

    target = 2 + (blockno / (JOS_BLKBITSIZE));

    bdesc = CALL(((struct lfs_info *) object->instance)->ubd, read_block, target);

    if (bdesc->length != JOS_BLKSIZE) {
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
    struct lfs_info * info = (struct lfs_info *) object->instance;
    bdesc_t * bdesc;
    int target;
    int r;
    uint32_t * ptr;

    if (blockno == 0) {
        printf("josfs_base: attempted to free zero block!\n");
        return -1;
    }

    target = 2 + (blockno / (JOS_BLKBITSIZE));

    bdesc = CALL(info->ubd, read_block, target);

    if (bdesc->length != JOS_BLKSIZE) {
        printf("josfs_base: trouble reading bitmap!\n");
        return -1;
    }

    ptr = ((uint32_t *)bdesc->data) + (blockno / 32);
    if (value)
        *ptr |= (1 << (blockno % 32));
    else
        *ptr &= ~(1 << (blockno % 32));


    if ((r = CALL(info->ubd, write_block, bdesc)) < 0)
        return r;

    return 0;
}

static bdesc_t * josfs_allocate_block(LFS_t * object, uint32_t size, int purpose)
{
    struct lfs_info * info = (struct lfs_info *) object->instance;
    int blockno;
    int bitmap_size;
    int s_nblocks;

    s_nblocks = info->super->s_nblocks;
    bitmap_size = s_nblocks / JOS_BLKBITSIZE;

    if (s_nblocks % JOS_BLKBITSIZE) {
        bitmap_size++;
    }

    for (blockno = 2 + bitmap_size; blockno < s_nblocks; blockno++) {
        if (block_is_free(object, blockno) == 1) {
            write_bitmap(object, blockno, 0);
            return bdesc_alloc(info->ubd, blockno, 0, JOS_BLKSIZE);
        }
    }

    return NULL;
}

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number, uint32_t offset, uint32_t size)
{
    if (offset != 0 || size != JOS_BLKSIZE)
        return NULL;
    
    return bdesc_alloc(((struct lfs_info *) object->instance)->ubd, number, 0, JOS_BLKSIZE);
}

// TODO
static fdesc_t * josfs_lookup_name(LFS_t * object, const char * name)
{
    return 0;
}

// TODO
static void josfs_free_fdesc(LFS_t * object, fdesc_t * fdesc)
{
}

// TODO
static bdesc_t * josfs_get_file_block(LFS_t * object, fdesc_t * file, uint32_t offset)
{
    return 0;
}

// TODO
static int josfs_append_file_block(LFS_t * object, fdesc_t * file, bdesc_t * block)
{
    return 0;
}

// TODO
static fdesc_t * josfs_allocate_name(LFS_t * object, char * name, int type, fdesc_t * link)
{
    return 0;
}

// TODO
static int josfs_rename(LFS_t * object, const char * oldname, const char * newname)
{
    return 0;
}

// TODO
static bdesc_t * josfs_truncate_file_block(LFS_t * object, fdesc_t * file)
{
    /*
    struct JOS_File * f;
    uint32_t bno, nblocks, new_nblocks;

    f = (jos_fdesc)file->file;

    nblocks = (f->f_size / BLKSIZE);
    if (f->f_size % BLKSIZE) {
        nblocks++;
    }
    */

    // FIXME


    return 0;
}

// TODO
static int josfs_free_block(LFS_t * object, bdesc_t * block)
{
    if (block->number == 0)
        return -1;
    write_bitmap(object, block->number, 1);
    return 0;
}

// TODO
static int josfs_apply_changes(LFS_t * object, chdesc_t * changes)
{
    return 0;
}

// TODO
static int josfs_remove_name(LFS_t * object, const char * name)
{
    return 0;
}

static int josfs_write_block(LFS_t * object, bdesc_t * block, uint32_t offset, uint32_t size, void * data)
{
    int r;
    struct lfs_info * info = (struct lfs_info *) object->instance;

    if (offset + size > JOS_BLKSIZE) {
        return -1;
    }

    memcpy(block->data + offset, data, size);
    if ((r = CALL(info->ubd, write_block, block)) < 0)
        return r;

    return 0;
}

static size_t josfs_get_num_features(LFS_t * object, const char * name)
{
    return 1;
}

static const feature_t * josfs_get_feature(LFS_t * object, const char * name, size_t num)
{
    feature_t s;
    feature_t * f = NULL;

    // FIXME
    if (num == 0) {
        s.id = 0xabba;
        s.optional = 0;
        s.warn = 0;
        f = &s;
    }
    return f;
}

// TODO
static int josfs_get_metadata(LFS_t * object, const char * name, uint32_t id, size_t * size, void ** data)
{
    return 0;
}

static int josfs_set_metadata(LFS_t * object, const char * name, uint32_t id, size_t size, const void * data)
{
    // TODO: lookup file path
    struct jos_fdesc foo;

    if (id == 0xabba) {
        if (size >= sizeof(off_t)) {
            if ((off_t)data >= 0 && (off_t)data < 4194304) {
                foo.file->f_size = (off_t)data;
                return 0;
            }
        }
    }

    return -1;
}

// TODO
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
    struct lfs_info * info;
    LFS_t * lfs = malloc(sizeof(*lfs));

    if (!lfs)
        return NULL;

    info = malloc(sizeof(*info));
    if (!info)
    {
        free(lfs);
        return NULL;
    }
    lfs->instance = info;

    ASSIGN(lfs, josfs, allocate_block);
    ASSIGN(lfs, josfs, lookup_block);
    ASSIGN(lfs, josfs, lookup_name);
    ASSIGN(lfs, josfs, free_fdesc);
    ASSIGN(lfs, josfs, get_file_block);
    ASSIGN(lfs, josfs, append_file_block);
    ASSIGN(lfs, josfs, allocate_name);
    ASSIGN(lfs, josfs, rename);
    ASSIGN(lfs, josfs, truncate_file_block);
    ASSIGN(lfs, josfs, free_block);
    ASSIGN(lfs, josfs, apply_changes);
    ASSIGN(lfs, josfs, remove_name);
    ASSIGN(lfs, josfs, write_block);
    ASSIGN(lfs, josfs, get_num_features);
    ASSIGN(lfs, josfs, get_feature);
    ASSIGN(lfs, josfs, get_metadata);
    ASSIGN(lfs, josfs, set_metadata);
    ASSIGN(lfs, josfs, sync);
    ASSIGN_DESTROY(lfs, josfs, destroy);

    info->ubd = block_device;
    if (check_super(lfs)) {
        free(info);
        free(lfs);
        return NULL;
    }

    if (check_bitmap(lfs)) {
        free(info);
        free(lfs);
        return NULL;
    }

    return lfs;
}
