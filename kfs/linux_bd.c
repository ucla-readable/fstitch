#include <inc/error.h>
#include <lib/assert.h>
#include <lib/stdio.h>
#include <lib/stdlib.h>
#include <lib/string.h>
#include <lib/types.h>
#include <lib/panic.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/blockman.h>
#include <kfs/modman.h>
#include <kfs/linux_bd.h>
#include <kfs/revision.h>

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/namei.h>

#ifndef __KERNEL__
#error linux_bd must be compiled for the linux kernel
#endif

struct linux_info {
	struct block_device *bdev;
	const char * path;

	wait_queue_head_t waitq; // wait for DMA to complete
	spinlock_t wait_lock; // lock for 'waitq'
	int dma_done;

	uint32_t blockcount;
	uint16_t blocksize;
	uint16_t level;
	blockman_t * blockman;
};

static int linux_bd_get_config(void * object, int level,
							   char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct linux_info * info = (struct linux_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_BRIEF:
			snprintf(string, length, "%s", info->path);
			break;
		case CONFIG_VERBOSE:
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "%s: %d bytes x %d blocks", info->path,
					 info->blocksize, info->blockcount);
	}
	return 0;
}

static int linux_bd_get_status(void * object, int level,
							   char * string, size_t length)
{
	/* no status to report */
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static uint32_t linux_bd_get_numblocks(BD_t * object)
{
	return ((struct linux_info*) OBJLOCAL(object))->blockcount;
}

static uint16_t linux_bd_get_blocksize(BD_t * object)
{
	return ((struct linux_info*) OBJLOCAL(object))->blocksize;
}

static uint16_t linux_bd_get_atomicsize(BD_t * object)
{
	return 512;
}

static int bio_end_io_fn(struct bio *bio, unsigned int done, int error);

static bdesc_t * linux_bd_read_block(BD_t * object, uint32_t number,
									 uint16_t count)
{
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	bdesc_t * ret;
	struct bio *bio;
	struct bio_vec *bv;
	int vec_len;
	int r;
	int i;

	if (!count || number + count > info->blockcount)
		return NULL;

	ret = blockman_managed_lookup(info->blockman, number);
	if (ret)
	{
		assert(ret->count == count);
		return ret;
	}

	ret = bdesc_alloc(number, info->blocksize, count);
	if (ret == NULL)
		return NULL;
	bdesc_autorelease(ret);
	
	vec_len = (count * info->blocksize) / 4096;
	if ((count * info->blocksize) % 4096)
		vec_len++;

	bio = bio_alloc(GFP_KERNEL, vec_len);
	if (!bio) {
		printk(KERN_ERR "bio_alloc() failed\n");
		return NULL;
	}
	for (i = 0; i < vec_len; i++) {
		bv = bio_iovec_idx(bio, 0);
		bv->bv_page = alloc_page(GFP_KERNEL);
		if (!bv->bv_page) {
			printk(KERN_ERR "mempool_alloc() failed\n");
			return NULL;
		}
		bv->bv_len = 4096;
		bv->bv_offset = 0;
	}
	bio->bi_idx = 0;
	bio->bi_vcnt = vec_len;
	bio->bi_sector = number;
	bio->bi_size = info->blocksize * count;
	bio->bi_bdev = info->bdev;
	bio->bi_rw = READ;
	bio->bi_end_io = bio_end_io_fn;
	bio->bi_private = object;

	generic_make_request(bio);

	// wait for it to complete
	printk(KERN_ERR "going to sleep!\n");
	DEFINE_WAIT(wait);
	spin_lock(&info->wait_lock);
	prepare_to_wait(&info->waitq, &wait, TASK_INTERRUPTIBLE);
	info->dma_done = 0;
	while (info->dma_done == 0) {
		spin_unlock(&info->wait_lock);
		printk(KERN_ERR "dma not done. sleeping\n");
		schedule_timeout(50);
		spin_lock(&info->wait_lock);
	}
	spin_unlock(&info->wait_lock);
	finish_wait(&info->waitq, &wait);
	printk(KERN_ERR "woke up!\n");

	r = blockman_managed_add(info->blockman, ret);
	if (r < 0)
		return NULL;
	return ret;
}

static int
bio_end_io_fn(struct bio *bio, unsigned int done, int error)
{
	struct linux_info * info = (struct linux_info *)
		OBJLOCAL((BD_t*)(bio->bi_private));
	unsigned char *p;


	printk(KERN_ERR "done w/ bio transfer\n");
	if (bio->bi_size)
		return 1;
	printk(KERN_ERR "done w/ bio transfer 2\n");
	p = (unsigned char *)(page_address(bio->bi_io_vec->bv_page));
	// XXX doesn't actually copy the data yet
	bio_put(bio);

	spin_lock(&info->wait_lock);
	wake_up_all(&info->waitq);
	info->dma_done = 1;
	spin_unlock(&info->wait_lock);

	return error;
}

static bdesc_t * linux_bd_synthetic_read_block(BD_t * object, uint32_t number,
											   uint16_t count,
											   bool * synthetic)
{
	/* linux_bd doesn't bother with synthetic blocks,
	 * since it's just as fast to use real ones */
	bdesc_t * ret = linux_bd_read_block(object, number, count);
	if(ret)
		*synthetic = 0;
	return ret;
}

static int linux_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	datadesc_t * ddesc = blockman_lookup(info->blockman, number);
	if(ddesc)
		blockman_remove(ddesc);
	return 0;
}

static int linux_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	
	if(block->ddesc->length != info->blocksize) {
		panic("wrote block with bad length\n");
		return -E_INVAL;
	}
	if (block->number >= info->blockcount) {
		panic("wrote bad block number\n");
		return -E_INVAL;
	}
	return -E_INVAL;
}

static int linux_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return FLUSH_EMPTY;
}

static uint16_t linux_bd_get_devlevel(BD_t * object)
{
	return ((struct linux_info *) OBJLOCAL(object))->level;
}

static int linux_bd_destroy(BD_t * bd)
{
	struct linux_info * info = (struct linux_info *) OBJLOCAL(bd);
	int r;

	r = modman_rem_bd(bd);
	if (r < 0) return r;

	blockman_destroy(&info->blockman);

	blkdev_put(info->bdev);
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

// ripped from 2.6.15.1-kernel source/drivers/md/dm-table.c:
/*
 * Convert a device path to a dev_t.
 */
static int lookup_device(const char *path, dev_t *dev)
{
	int r;
	struct nameidata nd;
	struct inode *inode;

	if ((r = path_lookup(path, LOOKUP_FOLLOW, &nd)))
		return r;

	inode = nd.dentry->d_inode;
	if (!inode) {
		r = -ENOENT;
		goto out;
	}

	if (!S_ISBLK(inode->i_mode)) {
		r = -ENOTBLK;
		goto out;
	}

	*dev = inode->i_rdev;

 out:
	path_release(&nd);
	return r;
}

static int open_bdev(const char *path, int mode, struct block_device **bdev)
{
	static char *_claim_ptr = "i belong to ospfs";
	int r;
	dev_t dev;

	r = lookup_device(path, &dev);
	if (r) {
		printk(KERN_ERR "error from lookup_device()\n");
		return r;
	}
	*bdev = open_by_devnum(dev, mode);
	if (IS_ERR(*bdev)) {
		printk(KERN_ERR "error from open_by_devnum()\n");
		return PTR_ERR(*bdev);
	}
	r = bd_claim(*bdev, _claim_ptr);
	if (r)
		blkdev_put(*bdev);
	return r;
}

BD_t * linux_bd(const char * linux_bdev_path)
{
	struct linux_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	int r;
	
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}

	r = open_bdev(linux_bdev_path, READ, &info->bdev);
	if (r) {
		printk(KERN_ERR "open_bdev() error\n");
		free(info);
		free(bd);
		return NULL;
	}
	
	info->blockman = blockman_create(512);
	if (!info->blockman) {
		blkdev_put(info->bdev);
		free(info);
		free(bd);
		return NULL;
	}

	info->level = 0;
	init_waitqueue_head(&info->waitq);
	spin_lock_init(&info->wait_lock);
	
	BD_INIT(bd, linux_bd, info);
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}
