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
#include <linux/types.h>
#include <asm/atomic.h>

#ifndef __KERNEL__
#error linux_bd must be compiled for the linux kernel
#endif

#define LINUX_BD_DEBUG_PRINT_EVERY_READ 0
#define DEBUG_LINUX_BD 0
#define LINUX_BD_DEBUG_COLLECT_STATS 0

#define READ_AHEAD_COUNT 10

#if DEBUG_LINUX_BD
#define KDprintk(x...) printk(x)
#else
#define KDprintk(x...)
#endif

int linux_bd_destroy(BD_t * bd);

#if 0
static void
bad_coffee(char *p)
{
	uint32_t *x = (uint32_t *)p;
	int i;
	for (i = 0; i < 1024; i++)
		x[i] = 0xFE0FDCBA; // little endian BADC0FFE
}
#endif

// READ AHEAD
static bdesc_t *look_ahead_store[100];
static uint32_t look_ahead_idx = 0;

static void
read_ahead_insert(bdesc_t *b)
{
	bdesc_retain(b);
	if (look_ahead_store[look_ahead_idx]) {
		bdesc_release(&look_ahead_store[look_ahead_idx]);
	}
	look_ahead_store[look_ahead_idx] = b;
	look_ahead_idx++;
	if (look_ahead_idx == 100)
		look_ahead_idx = 0;
}

static void
read_ahead_empty(void)
{
	int i;
	for (i = 0; i < 100; i++)
		if (look_ahead_store[i]) {
			bdesc_release(&look_ahead_store[i]);
			look_ahead_store[i] = 0;
		}
}

// STATS
#if LINUX_BD_DEBUG_COLLECT_STATS
static struct timespec stat_read_total = {0, 0};
static struct timespec stat_read_min = {99, 0};
static struct timespec stat_read_max = {0, 0};
static uint32_t stat_read_cnt = 0;
static struct timespec stat_write_total = {0, 0};
static struct timespec stat_write_min = {99, 0};
static struct timespec stat_write_max = {0, 0};
static uint32_t stat_write_cnt = 0;

static void
stat_dump(void)
{
	printk(KERN_ERR "READ: min:   %ld.%09ld\n", stat_read_min.tv_sec,
		   stat_read_min.tv_nsec);
	printk(KERN_ERR "READ: max:   %ld.%09ld\n", stat_read_max.tv_sec,
		   stat_read_max.tv_nsec);
	printk(KERN_ERR "READ: total: %ld.%09ld\n", stat_read_total.tv_sec,
		   stat_read_total.tv_nsec);
	printk(KERN_ERR "READ: %d reads\n", stat_read_cnt);
	printk(KERN_ERR "WRITE: min:   %ld.%09ld\n", stat_write_min.tv_sec,
		   stat_write_min.tv_nsec);
	printk(KERN_ERR "WRITE: max:   %ld.%09ld\n", stat_write_max.tv_sec,
		   stat_write_max.tv_nsec);
	printk(KERN_ERR "WRITE: total: %ld.%09ld\n", stat_write_total.tv_sec,
		   stat_write_total.tv_nsec);
	printk(KERN_ERR "WRITE: %d writes\n", stat_write_cnt);
}

static void
stat_update_min(struct timespec *oldmin, struct timespec *sample)
{
	if (oldmin->tv_sec < sample->tv_sec) return;
	if ((oldmin->tv_sec > sample->tv_sec) ||
		(oldmin->tv_nsec > sample->tv_nsec)) {
		oldmin->tv_sec = sample->tv_sec;
		oldmin->tv_nsec = sample->tv_nsec;
	}
}

static void
stat_update_max(struct timespec *oldmax, struct timespec *sample)
{
	if (oldmax->tv_sec > sample->tv_sec) return;
	if ((oldmax->tv_sec < sample->tv_sec) ||
		(oldmax->tv_nsec < sample->tv_nsec)) {
		oldmax->tv_sec = sample->tv_sec;
		oldmax->tv_nsec = sample->tv_nsec;
	}
}

// sum += sample
static void
stat_add(struct timespec *sum, struct timespec *sample)
{
	sum->tv_sec += sample->tv_sec;
	sum->tv_nsec += sample->tv_nsec;
	if (sum->tv_nsec >= NSEC_PER_SEC) {
		sum->tv_sec++;
		sum->tv_nsec -= NSEC_PER_SEC;
	}
}

// base -= sample
static void
stat_sub(struct timespec *base, struct timespec *sample)
{
	base->tv_sec -= sample->tv_sec;
	base->tv_nsec -= sample->tv_nsec;
	if (base->tv_nsec < 0) {
		base->tv_sec--;
		base->tv_nsec += NSEC_PER_SEC;
	}
}

static void
stat_process_read(struct timespec *start, struct timespec *stop)
{
	stat_sub(stop, start); // stop -= start;
	stat_update_min(&stat_read_min, stop);
	stat_update_max(&stat_read_max, stop);
	stat_add(&stat_read_total, stop);
	stat_read_cnt++;
}

static void
stat_process_write(struct timespec *start, struct timespec *stop)
{
	stat_sub(stop, start); // stop -= start;
	stat_update_min(&stat_write_min, stop);
	stat_update_max(&stat_write_max, stop);
	stat_add(&stat_write_total, stop);
	stat_write_cnt++;
}
#endif // LINUX_BD_DEBUG_COLLECT_STATS

struct linux_info {
	struct block_device *bdev;
	const char * path;

	wait_queue_head_t waitq; // wait for DMA to complete
	spinlock_t wait_lock; // lock for 'waitq'

	atomic_t outstanding_io_count;

	uint32_t blockcount;
	uint16_t blocksize;
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

	uint32_t seq;
	bdesc_t * bdesc;
	uint32_t number;
	uint16_t count;
};

static uint32_t _seq = 0;

static spinlock_t dma_outstanding_lock;
static int dma_outstanding = 0;

static int bio_end_io_fn(struct bio *bio, unsigned int done, int error);

static bdesc_t * linux_bd_read_block(BD_t * object, uint32_t number,
                                     uint16_t count)
{
	DEFINE_WAIT(wait);
	int waited = 0;
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	struct bio *bio;
	struct bio_vec *bv;
	int vec_len;
	int r, i, j;
	struct linux_bio_private private[READ_AHEAD_COUNT];
	static int infty = 10;
	bdesc_t *blocks[READ_AHEAD_COUNT];
#if LINUX_BD_DEBUG_COLLECT_STATS
	struct timespec start;
	struct timespec stop;
#endif
	int read_ahead_count = READ_AHEAD_COUNT;

	KDprintk(KERN_ERR "entered read (blk: %d, cnt: %d)\n", number, count);

	bdesc = blockman_managed_lookup(info->blockman, number);
	if (bdesc) {
		assert(bdesc->count == count);
		if (!bdesc->ddesc->synthetic) {
			KDprintk(KERN_ERR "already got it. done w/ read\n");
			return bdesc;
		}
	} else {
		if (!count || number + count > info->blockcount) {
			printk(KERN_ERR "bailing on read 1\n");
			return NULL;
		}
	}

	KDprintk(KERN_ERR "starting real read work\n");
	spin_lock_init(&dma_outstanding_lock);
	spin_lock(&dma_outstanding_lock);
	dma_outstanding = 0;
	spin_unlock(&dma_outstanding_lock);

	KDprintk(KERN_ERR "count: %d, bs: %d\n", count, info->blocksize);
	// assert((count * info->blocksize) <= 2048); Why was this assert here?
	if (count != 4)
		read_ahead_count = 1;
#if LINUX_BD_DEBUG_COLLECT_STATS
	start = current_kernel_time();
#endif
	for (j = 0; j < read_ahead_count; j++) {
		uint32_t j_number = number + (count * j);
		datadesc_t * dd;

		dd = blockman_lookup(info->blockman, j_number);
		if (dd && !dd->synthetic) {
			blocks[j] = NULL;
			continue;
		} else if (dd) {
			if(bdesc->ddesc == dd)
				blocks[j] = bdesc;
			else {
				/* blockman_managed_lookup? */
				blocks[j] = bdesc_alloc_wrap(dd, j_number, dd->length / info->blockman->length);
				if (blocks[j] == NULL)
					return NULL;
				bdesc_autorelease(blocks[j]);
			}
		} else {
			blocks[j] = bdesc_alloc(j_number, info->blocksize, count);
			if (blocks[j] == NULL)
				return NULL;
			bdesc_autorelease(blocks[j]);
		}
	
		vec_len = (count * info->blocksize + 4095) / 4096;
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
			//bad_coffee(page_address(bv->bv_page));
			bv->bv_len = info->blocksize * count;
			bv->bv_offset = 0;
		}

		private[j].info = info;
		private[j].bdesc = blocks[j];
		private[j].number = j_number;
		private[j].count = count;
		private[j].seq = _seq++;

		bio->bi_idx = 0;
		bio->bi_vcnt = vec_len;
		bio->bi_sector = j_number;
		bio->bi_size = info->blocksize * count;
		bio->bi_bdev = info->bdev;
		bio->bi_rw = READ;
		bio->bi_end_io = bio_end_io_fn;
		bio->bi_private = &private[j];

		spin_lock(&dma_outstanding_lock);
		dma_outstanding++;
		spin_unlock(&dma_outstanding_lock);

		atomic_inc(&info->outstanding_io_count);

#if LINUX_BD_DEBUG_PRINT_EVERY_READ
		printk(KERN_ERR "%d\n", j_number);
#endif // LINUX_BD_DEBUG_PRINT_EVERY_READ
		generic_make_request(bio);
	}

	// wait for it to complete
	KDprintk(KERN_ERR "going to sleep! [%d]\n", private[0].seq);
	spin_lock(&dma_outstanding_lock);
	while (dma_outstanding > 0) {
		spin_unlock(&dma_outstanding_lock);
		if (infty > 0) {
			infty--;
			KDprintk(KERN_ERR "dma not done. sleeping\n");
		}
		spin_lock(&info->wait_lock);
		prepare_to_wait(&info->waitq, &wait, TASK_INTERRUPTIBLE);
		spin_unlock(&info->wait_lock);
		schedule_timeout(500);
		waited = 1;
		spin_lock(&dma_outstanding_lock);
	}
	spin_unlock(&dma_outstanding_lock);
	if (waited)
		finish_wait(&info->waitq, &wait);
	KDprintk(KERN_ERR "woke up!\n");

#if LINUX_BD_DEBUG_COLLECT_STATS
	stop = current_kernel_time();
	stat_process_read(&start, &stop);
#endif

	for (j = 0; j < read_ahead_count; j++) {
		if (j != 0) {
			if (blocks[j] == NULL) continue;
			read_ahead_insert(blocks[j]);
		}
	
		r = blockman_managed_add(info->blockman, blocks[j]);
		assert(r >= 0);
		if (r < 0)
			return NULL;
	}
	KDprintk(KERN_ERR "exiting read\n");
	return blocks[0];
}

static int
bio_end_io_fn(struct bio *bio, unsigned int done, int error)
{
	struct linux_bio_private * private =
		(struct linux_bio_private *)(bio->bi_private);
	struct linux_info * info = private->info;
	int i;
	unsigned long flags;
	int dir = bio->bi_rw;

	assert(info);
	assert(info->waitq.task_list.next);
	assert(info->waitq.task_list.next->next);

	KDprintk(KERN_ERR "[%d] done w/ bio transfer (%d, %d)\n", private->seq,
			 done, error);
	if (bio->bi_size)
	{
		/* Everyone else in the [2.6.12] linux kernel returns one here;
		 * we follow their lead. No one inspects bi_end_io()'s return value,
		 * either. */
		/* Should we decrement info->outstanding_io_count at this exit?
		 * It sounds like non-zero bi_size may mean the i/o is not yet
		 * complete. So we'll not and hope it works out. */
		return 1;
	}
	KDprintk(KERN_ERR "[%d] done w/ bio transfer 2\n", private->seq);

	assert(bio->bi_vcnt == 1);
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
		}
		//bad_coffee(p);
		__free_page(bio_iovec_idx(bio, i)->bv_page);
		bio_iovec_idx(bio, i)->bv_page = NULL;
		bio_iovec_idx(bio, i)->bv_len = 0;
		bio_iovec_idx(bio, i)->bv_offset = 0;
	}

	if (dir == READ)
		private->bdesc->ddesc->synthetic = 0;
	else if (dir == WRITE)
		free(private);
	bio_put(bio);

	if (dir == READ) {
		int do_wake_up = 0;
		spin_lock_irqsave(&dma_outstanding_lock, flags);
		dma_outstanding--;
		if (dma_outstanding == 0)
			do_wake_up = 1;
		spin_unlock_irqrestore(&dma_outstanding_lock, flags);

		if (do_wake_up) {
			spin_lock_irqsave(&info->wait_lock, flags);
			wake_up_all(&info->waitq);
			spin_unlock_irqrestore(&info->wait_lock, flags);
		}
	}

	atomic_dec(&info->outstanding_io_count);
	return error;
}

static bdesc_t * linux_bd_synthetic_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		assert(bdesc->count == count);
		return bdesc;
	}
	
	/* make sure it's a valid block */
	if(!count || number + count > info->blockcount)
		return NULL;
	
	bdesc = bdesc_alloc(number, info->blocksize, count);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	bdesc->ddesc->synthetic = 1;
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		return NULL;
	
	return bdesc;
}

static int linux_bd_write_block(BD_t * object, bdesc_t * block)
{
	DEFINE_WAIT(wait);
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	struct bio *bio;
	struct bio_vec *bv;
	int vec_len;
	int r;
	int i;
	struct linux_bio_private * private;
#if LINUX_BD_DEBUG_COLLECT_STATS
	struct timespec start;
	struct timespec stop;
#endif
	
	KDprintk(KERN_ERR "entered write (blk: %d, cnt: %d)\n",
			 block->number, block->count);
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

	atomic_inc(&info->outstanding_io_count);

	KDprintk(KERN_ERR "issuing DMA write request [%d]\n", private->seq);
#if LINUX_BD_DEBUG_COLLECT_STATS
	start = current_kernel_time();
#endif
	//printk(KERN_ERR "WRITE %d\n", block->number);
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

#if LINUX_BD_DEBUG_COLLECT_STATS
	stop = current_kernel_time();
	stat_process_write(&start, &stop);
#endif

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

int linux_bd_destroy(BD_t * bd)
{
	struct linux_info * info = (struct linux_info *) OBJLOCAL(bd);
	int r;
	bool wait_printed = 0;

	while (atomic_read(&info->outstanding_io_count))
	{
		if (!wait_printed)
		{
			kdprintf(STDOUT_FILENO, "%s: waiting for %d outstanding I/Os\n", __FUNCTION__, atomic_read(&info->outstanding_io_count));
			wait_printed = 1;
		}
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 10);
	}

#if LINUX_BD_DEBUG_COLLECT_STATS
	stat_dump();
#endif

	r = modman_rem_bd(bd);
	if (r < 0) return r;

	read_ahead_empty();
	blockman_destroy(&info->blockman);

	bd_release(info->bdev);
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

	atomic_set(&info->outstanding_io_count, 0);

	r = open_bdev(linux_bdev_path, WRITE, &info->bdev);
	if (r) {
		printk(KERN_ERR "open_bdev() error\n");
		free(info);
		free(bd);
		return NULL;
	}
	
	info->blockman = blockman_create(512, NULL, NULL);
	if (!info->blockman) {
		bd_release(info->bdev);
		blkdev_put(info->bdev);
		free(info);
		free(bd);
		return NULL;
	}

	memset(look_ahead_store, 0, sizeof(look_ahead_store));
	bd->level = 0;
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
