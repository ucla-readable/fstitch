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

#define DEBUG_LINUX_BD 0

#if DEBUG_LINUX_BD
#define KDprintk(x...) printk(x)
#else
#define KDprintk(x...)
#endif

struct linux_info {
	struct block_device *bdev;
	const char * path;

	wait_queue_head_t waitq; // wait for DMA to complete
	spinlock_t wait_lock; // lock for 'waitq'

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

struct linux_bio_private {
	struct linux_info * info;

	spinlock_t dma_done_lock; // lock for 'dma_done'
	int dma_done;

	uint32_t seq;
	bdesc_t * bdesc;
	uint32_t number;
	uint16_t count;
};

static uint32_t _seq = 0;

static void dump_page(unsigned char * p, int len, int off) {
	int lines = len / 16;
	int i;
	printk(KERN_ERR "begin dump:\n");
	for (i = 0; i < lines; i++) {
		int j;
		printk(KERN_ERR "%08x", off);
		for (j = 0; j < 16; j++) {
			if (j % 8) printk(" ");
			printk(" %02x", p[i*16 + j]);
		}
		printk("\n");
		off += 16;
	}
	printk(KERN_ERR "dump done\n");
}

static int bio_end_io_fn(struct bio *bio, unsigned int done, int error);

static bdesc_t * linux_bd_read_block(BD_t * object, uint32_t number,
                                     uint16_t count)
{
	DEFINE_WAIT(wait);
	int waited = 0;
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	bdesc_t * ret;
	struct bio *bio;
	struct bio_vec *bv;
	int vec_len;
	int r;
	int i;
	struct linux_bio_private private;
	static int infty = 10;

	KDprintk(KERN_ERR "entered read\n");
	if (!count || number + count > info->blockcount) {
		printk(KERN_ERR "bailing on read 1\n");
		return NULL;
	}

	ret = blockman_managed_lookup(info->blockman, number);
	if (ret)
	{
		assert(ret->count == count);
		KDprintk(KERN_ERR "already got it. done w/ read\n");
		return ret;
	}

	KDprintk(KERN_ERR "starting real read work\n");
	ret = bdesc_alloc(number, info->blocksize, count);
	if (ret == NULL)
		return NULL;
	bdesc_autorelease(ret);
	
	vec_len = (count * info->blocksize) / 4096;
	if ((count * info->blocksize) % 4096)
		vec_len++;
	assert(vec_len == 1);

	bio = bio_alloc(GFP_KERNEL, vec_len);
	if (!bio) {
		printk(KERN_ERR "bio_alloc() failed\n");
		return NULL;
	}
	for (i = 0; i < vec_len; i++) {
		bv = bio_iovec_idx(bio, i);
		bv->bv_page = alloc_page((GFP_KERNEL | GFP_DMA));
		if (!bv->bv_page) {
			printk(KERN_ERR "alloc_page() failed\n");
			return NULL;
		}
		bv->bv_len = 4096;
		bv->bv_offset = 0;
	}

	spin_lock_init(&private.dma_done_lock);
	spin_lock(&private.dma_done_lock);
	private.dma_done = 0;
	spin_unlock(&private.dma_done_lock);
	private.info = info;
	private.bdesc = ret;
	private.number = number;
	private.count = count;
	private.seq = _seq++;

	bio->bi_idx = 0;
	bio->bi_vcnt = vec_len;
	bio->bi_sector = number;
	bio->bi_size = info->blocksize * count;
	bio->bi_bdev = info->bdev;
	bio->bi_rw = READ;
	bio->bi_end_io = bio_end_io_fn;
	bio->bi_private = &private;

	spin_lock(&private.dma_done_lock);
	private.dma_done = 0;
	spin_unlock(&private.dma_done_lock);

	generic_make_request(bio);

	// wait for it to complete
	KDprintk(KERN_ERR "going to sleep! [%d]\n", private.seq);
	spin_lock(&private.dma_done_lock);
	while (private.dma_done == 0) {
		spin_unlock(&private.dma_done_lock);
		if (infty > 0) {
			infty--;
			KDprintk(KERN_ERR "dma not done. sleeping\n");
		}
		spin_lock(&info->wait_lock);
		prepare_to_wait(&info->waitq, &wait, TASK_INTERRUPTIBLE);
		spin_unlock(&info->wait_lock);
		schedule_timeout(500);
		waited = 1;
		spin_lock(&private.dma_done_lock);
	}
	spin_unlock(&private.dma_done_lock);
	if (waited)
		finish_wait(&info->waitq, &wait);
	KDprintk(KERN_ERR "woke up!\n");

	r = blockman_managed_add(info->blockman, ret);
	if (r < 0)
		return NULL;
	KDprintk(KERN_ERR "exiting read\n");
	return ret;
}

static int
bio_end_io_fn(struct bio *bio, unsigned int done, int error)
{
	struct linux_bio_private * private =
		(struct linux_bio_private *)(bio->bi_private);
	struct linux_info * info = private->info;
	int i;
	static int infty = 1;
	unsigned long flags;
	int dir = bio->bi_rw;

	assert(info);
	assert(info->waitq.task_list.next);
	assert(info->waitq.task_list.next->next);

	KDprintk(KERN_ERR "[%d] done w/ bio transfer\n", private->seq);
	if (bio->bi_size)
		return 1;
	KDprintk(KERN_ERR "[%d] done w/ bio transfer 2\n", private->seq);

	for (i = 0; i < bio->bi_vcnt; i++) {
		int len;
		unsigned char *p = (unsigned char *)
			(page_address(bio_iovec_idx(bio, i)->bv_page));

		assert(p);
		if (dir == READ) {
			len = 4096;
			if ((i+1) == bio->bi_vcnt) {
				len = (private->count * info->blocksize) % 4096;
				if (len == 0)
					len = 4096;
			}

			memcpy(private->bdesc->ddesc->data + (4096 * i), p, len);

			if (infty > 0) {
				infty--;
				dump_page(p, 256, info->blocksize * private->number);
			}
		}
		__free_page(bio_iovec_idx(bio, i)->bv_page);
		bio_iovec_idx(bio, i)->bv_page = NULL;
		bio_iovec_idx(bio, i)->bv_len = 0;
		bio_iovec_idx(bio, i)->bv_offset = 0;
	}

	if (dir == WRITE)
		free(private);
	bio_put(bio);

	if (dir == READ) {
		spin_lock_irqsave(&private->dma_done_lock, flags);
		private->dma_done = 1;
		spin_unlock_irqrestore(&private->dma_done_lock, flags);

		spin_lock_irqsave(&info->wait_lock, flags);
		wake_up_all(&info->waitq);
		spin_unlock_irqrestore(&info->wait_lock, flags);
	}

	return error;
}

static bdesc_t * linux_bd_synthetic_read_block(BD_t * object, uint32_t number,
                                               uint16_t count,
                                               bool * synthetic)
{
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		assert(bdesc->count == count);
		*synthetic = 0;
		return bdesc;
	}
	
	/* make sure it's a valid block */
	if(!count || number + count > info->blockcount)
		return NULL;
	
	bdesc = bdesc_alloc(number, info->blocksize, count);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		return NULL;
	
	*synthetic = 1;
	
	return bdesc;
}

static int linux_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	datadesc_t * ddesc = blockman_lookup(info->blockman, number);
	if(ddesc)
	{
		assert(!ddesc->changes);
		blockman_remove(ddesc);
	}
	return 0;
}

static int linux_bd_write_block(BD_t * object, bdesc_t * block)
{
	DEFINE_WAIT(wait);
	int waited = 0;
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	struct bio *bio;
	struct bio_vec *bv;
	int vec_len;
	int r;
	int i;
	struct linux_bio_private * private;
	static int infty = 10;
	
	KDprintk(KERN_ERR "entered write\n");
	if((info->blocksize * block->count) != block->ddesc->length) {
		panic("wrote block with bad length (%d bytes)\n",
			  block->ddesc->length);
		return -E_INVAL;
	}
	if (block->number >= info->blockcount) {
		panic("wrote bad block number\n");
		return -E_INVAL;
	}

	private = (struct linux_bio_private *)
		malloc(sizeof(struct linux_bio_private));
	assert(private);

	KDprintk(KERN_ERR "starting real work for the write\n");
	r = revision_tail_prepare(block, object);
	if (r != 0) {
		panic("revision_tail_prepare gave: %i\n", r);
		return r;
	}

	vec_len = block->ddesc->length / 4096;
	if (block->ddesc->length % 4096)
		vec_len++;
	assert(vec_len == 1);

	bio = bio_alloc(GFP_KERNEL, vec_len);
	if (!bio) {
		printk(KERN_ERR "bio_alloc() failed()\n");
		return -E_NO_MEM;
	}
	for (i = 0; i < vec_len; i++) {
		bv = bio_iovec_idx(bio, i);
		bv->bv_page = alloc_page(GFP_KERNEL | GFP_DMA);
		if (!bv->bv_page) {
			printk(KERN_ERR "alloc_page() failed\n");
			return -E_NO_MEM;
		}
		// this memcpy always writes to the beginning of the page,
		// which works fine if you just have one block, but is a
		// problem if you have more. right now you can only pass one
		// block to this function, so it's not a problem.
		memcpy(page_address(bv->bv_page),
			   block->ddesc->data,
			   block->ddesc->length);
		bv->bv_len = block->ddesc->length;
		bv->bv_offset = 0;
	}

	private->info = info;
	private->bdesc = block;
	private->number = block->number;
	private->count = block->count;
	private->seq = _seq++;

	bio->bi_idx = 0;
	bio->bi_vcnt = vec_len;
	bio->bi_sector = block->number;
	bio->bi_size = block->ddesc->length;
	bio->bi_bdev = info->bdev;
	bio->bi_rw = WRITE;
	bio->bi_end_io = bio_end_io_fn;
	bio->bi_private = private;

	KDprintk(KERN_ERR "issuing DMA write request [%d]\n", private->seq);
	generic_make_request(bio);

	// wait for it to complete
	/*
	printk(KERN_ERR "going to sleep!\n");
	spin_lock(&private.dma_done_lock);
	while (private.dma_done == 0) {
		spin_unlock(&private.dma_done_lock);
		if (infty > 0) {
			infty--;
			printk(KERN_ERR "dma not done. sleeping\n");
		}
		spin_lock(&info->wait_lock);
		prepare_to_wait(&info->waitq, &wait, TASK_INTERRUPTIBLE);
		spin_unlock(&info->wait_lock);
		schedule_timeout(500);
		waited = 1;
		spin_lock(&private.dma_done_lock);
	}
	spin_unlock(&private.dma_done_lock);
	if (waited)
		finish_wait(&info->waitq, &wait);
	printk(KERN_ERR "woke up!\n");
	*/

	r = revision_tail_acknowledge(block, object);
	if (r != 0) {
		panic("revision_tail_acknowledge gave error: %i\n", r);
		return r;
	}
	KDprintk(KERN_ERR "exiting write\n");
	return 0;
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
	static char *_claim_ptr = "I belong to kkfsd";
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
	info->blocksize = 512;
	info->blockcount = info->bdev->bd_disk->capacity;
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
