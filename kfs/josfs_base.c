/* Avoid #including <inc/lib.h> to keep <inc/fs.h> out */
#include <inc/types.h>
#include <inc/x86.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/lfs.h>
#include <kfs/josfs_base.h>

#define block_is_free read_bitmap

int read_bitmap(LFS_t * object, uint32_t blockno);
int write_bitmap(LFS_t * object, uint32_t blockno, bool value);

struct lfs_info
{
    BD_t * ubd;
    bdesc_t * super_block;
};

struct josfs_fdesc {
    struct bdesc * dirb;
    int index;
    struct JOSFS_File * file;
};

#define super ((struct JOSFS_Super *) info->super_block->ddesc->data)

// Equivalent to JOS's read_super
int check_super(LFS_t * object)
{
    struct lfs_info * info = (struct lfs_info *) object->instance;
    uint32_t numblocks;

    /* make sure we have the block size we expect */
    if (CALL(info->ubd, get_blocksize) != JOSFS_BLKSIZE) {
        printf("Block device size is not BLKSIZE!\n");
        return -1;
    }

    /* the superblock is in block 1 */
    info->super_block = CALL(info->ubd, read_block, 1);
    bdesc_retain(&info->super_block);

    if (super->s_magic != JOSFS_FS_MAGIC) {
        printf("josfs_base: bad file system magic number\n");
        bdesc_release(&info->super_block);
        return -1;
    }

    numblocks = CALL(info->ubd, get_numblocks);

    if (super->s_nblocks > numblocks) {
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

    blocks_to_read = super->s_nblocks / JOSFS_BLKBITSIZE;

    if (super->s_nblocks % JOSFS_BLKBITSIZE) {
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

    target = 2 + (blockno / (JOSFS_BLKBITSIZE));

    bdesc = CALL(((struct lfs_info *) object->instance)->ubd, read_block, target);

    if (bdesc->length != JOSFS_BLKSIZE) {
        printf("josfs_base: trouble reading bitmap!\n");
        return -1;
    }

    ptr = ((uint32_t *) bdesc->ddesc->data) + (blockno / 32);
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

    target = 2 + (blockno / (JOSFS_BLKBITSIZE));

    bdesc = CALL(info->ubd, read_block, target);

    if (bdesc->length != JOSFS_BLKSIZE) {
        printf("josfs_base: trouble reading bitmap!\n");
        return -1;
    }

    ptr = ((uint32_t *) bdesc->ddesc->data) + (blockno / 32);
    if (value)
        *ptr |= (1 << (blockno % 32));
    else
        *ptr &= ~(1 << (blockno % 32));


    if ((r = CALL(info->ubd, write_block, bdesc)) < 0)
        return r;

    return 0;
}

static uint32_t josfs_get_blocksize(LFS_t * object)
{
    return JOSFS_BLKSIZE;
}

static BD_t * josfs_get_blockdev(LFS_t * object)
{
    return ((struct lfs_info *) object->instance)->ubd;
}

static bdesc_t * josfs_allocate_block(LFS_t * object, uint32_t size, int purpose, chdesc_t ** head, chdesc_t ** tail)
{
    struct lfs_info * info = (struct lfs_info *) object->instance;
    int blockno;
    int bitmap_size;
    int s_nblocks;

    s_nblocks = super->s_nblocks;
    bitmap_size = s_nblocks / JOSFS_BLKBITSIZE;

    if (s_nblocks % JOSFS_BLKBITSIZE) {
        bitmap_size++;
    }

    for (blockno = 2 + bitmap_size; blockno < s_nblocks; blockno++) {
        if (block_is_free(object, blockno) == 1) {
            write_bitmap(object, blockno, 0);
            return bdesc_alloc(info->ubd, blockno, 0, JOSFS_BLKSIZE);
        }
    }

    return NULL;
}

static bdesc_t * josfs_lookup_block(LFS_t * object, uint32_t number, uint32_t offset, uint32_t size)
{
    if (offset != 0 || size != JOSFS_BLKSIZE)
        return NULL;
    
    return bdesc_alloc(((struct lfs_info *) object->instance)->ubd, number, 0, JOSFS_BLKSIZE);
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
static int josfs_get_dirent(LFS_t * object, fdesc_t * file, uint32_t index, struct dirent * entry, uint16_t size, uint32_t * basep)
{
    return 0;
}

static int josfs_append_file_block(LFS_t * object, fdesc_t * file, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
    struct lfs_info * info = (struct lfs_info *) object->instance;
    struct josfs_fdesc * f = (struct josfs_fdesc *) file;
    uint32_t nblocks = (f->file->f_size / JOSFS_BLKSIZE);
    bdesc_t * indirect;
    uint32_t * indirect_offset;
    
    if (f->file->f_size % JOSFS_BLKSIZE) {
        nblocks++;
    }

    if (nblocks > JOSFS_NINDIRECT) {
        return -1;
    }
    else if (nblocks > JOSFS_NDIRECT) {
        indirect = CALL(info->ubd, read_block, f->file->f_indirect);
        if (indirect != NULL) {
            indirect_offset = (uint32_t *) (indirect->ddesc->data + nblocks);
            *indirect_offset = block->number;
            return CALL(info->ubd, write_block, indirect);
        }
    }
    else if (nblocks == JOSFS_NDIRECT) {
        // FIXME chdescr
        indirect = josfs_allocate_block(object, JOSFS_BLKSIZE, 0, NULL, NULL);
        if (indirect != NULL) {
            f->file->f_indirect = indirect->number;
            indirect_offset = (uint32_t *) (indirect->ddesc->data + nblocks);
            *indirect_offset = block->number;
            return CALL(info->ubd, write_block, indirect);
        }
    }
    else {
        f->file->f_direct[nblocks] = block->number;
        return 0;
    }

    return -1;
}

// TODO
static fdesc_t * josfs_allocate_name(LFS_t * object, char * name, uint8_t type, fdesc_t * link, chdesc_t ** head, chdesc_t ** tail)
{
    return 0;
}

// TODO
static int josfs_rename(LFS_t * object, const char * oldname, const char * newname, chdesc_t ** head, chdesc_t ** tail)
{
    return 0;
}

static bdesc_t * josfs_truncate_file_block(LFS_t * object, fdesc_t * file, chdesc_t ** head, chdesc_t ** tail)
{
    struct lfs_info * info = (struct lfs_info *) object->instance;
    struct josfs_fdesc * f = (struct josfs_fdesc *) file;
    uint32_t nblocks = (f->file->f_size / JOSFS_BLKSIZE);
    bdesc_t * indirect;
    
    if (f->file->f_size % JOSFS_BLKSIZE) {
        nblocks++;
    }

    if (nblocks > JOSFS_NINDIRECT) {
        return NULL;
    }
    else if (nblocks > JOSFS_NDIRECT + 1) {
        indirect = CALL(info->ubd, read_block, f->file->f_indirect);
        if (indirect != NULL) {
            return CALL(info->ubd, read_block, (uint32_t) (indirect->ddesc->data) + nblocks);
        }
    }
    else if (nblocks == JOSFS_NDIRECT + 1) {
        indirect = CALL(info->ubd, read_block, f->file->f_indirect);
        if (indirect != NULL) {
            // FIXME chdescr
            if (josfs_free_block(object, indirect, NULL, NULL) == 0) {
                f->file->f_indirect = 0;
                return CALL(info->ubd, read_block, (uint32_t) (indirect->ddesc->data) + nblocks);
            }
        }
    }
    else {
        return CALL(info->ubd, read_block, f->file->f_direct[nblocks]);
    }

    return NULL;
}

static int josfs_free_block(LFS_t * object, bdesc_t * block, chdesc_t ** head, chdesc_t ** tail)
{
    if (block->number == 0)
        return -1;
    write_bitmap(object, block->number, 1);
    return 0;
}

// TODO
static int josfs_remove_name(LFS_t * object, const char * name, chdesc_t ** head, chdesc_t ** tail)
{
    return 0;
}

static int josfs_write_block(LFS_t * object, bdesc_t * block, uint32_t offset, uint32_t size, void * data, chdesc_t ** head, chdesc_t ** tail)
{
    int r;
    struct lfs_info * info = (struct lfs_info *) object->instance;

    if (offset + size > JOSFS_BLKSIZE) {
        return -1;
    }

    memcpy(&block->ddesc->data[offset], data, size);
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
    if (num == 0) {
        return &KFS_feature_size;
    }
    return NULL;
}

static int josfs_get_metadata(LFS_t * object, const char * name, uint32_t id, size_t * size, void ** data)
{
    // TODO lookup file path
    return 0;
}

static int josfs_set_metadata(LFS_t * object, const char * name, uint32_t id, size_t size, const void * data, chdesc_t ** head, chdesc_t ** tail)
{
    // TODO: lookup file path
    struct josfs_fdesc foo;

    if (id == KFS_feature_size.id) {
        if (sizeof(off_t) >= size) {
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

    ASSIGN(lfs, josfs, get_blocksize);
    ASSIGN(lfs, josfs, get_blockdev);
    ASSIGN(lfs, josfs, allocate_block);
    ASSIGN(lfs, josfs, lookup_block);
    ASSIGN(lfs, josfs, lookup_name);
    ASSIGN(lfs, josfs, free_fdesc);
    ASSIGN(lfs, josfs, get_file_block);
    ASSIGN(lfs, josfs, get_dirent);
    ASSIGN(lfs, josfs, append_file_block);
    ASSIGN(lfs, josfs, allocate_name);
    ASSIGN(lfs, josfs, rename);
    ASSIGN(lfs, josfs, truncate_file_block);
    ASSIGN(lfs, josfs, free_block);
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
