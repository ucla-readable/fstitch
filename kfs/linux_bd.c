#include <lib/error.h>
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

/* The old LINUX_BD_DEBUG_COLLECT_STATS is now DEBUG_TIMING */
#define DEBUG_TIMING 0
#include <kfs/kernel_timing.h>
KERNEL_TIMING(read);
KERNEL_TIMING(write);
KERNEL_TIMING(wait);

#ifdef CONFIG_MD
#error linux_bd is (apparently) incompatible with RAID/LVM
#endif

#define LINUX_BD_DEBUG_PRINT_EVERY_READ 0
#define DEBUG_WRITES 0

#define READ_AHEAD_COUNT 10

#define DEBUG_LINUX_BD 0
#if DEBUG_LINUX_BD
#define KDprintk(x...) printk(x)
#else
#define KDprintk(x...)
#endif

#if DEBUG_WRITES
# include <linux/debugfs.h>
# include <kfs/linux_bd_debug.h>
static struct linux_bd_writes debug_writes;
static atomic_t debug_writes_ninflight[MAXBLOCKNO];
static uint32_t debug_writes_completed;
static struct dentry * debug_writes_dentry;
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
	if (look_ahead_store[look_ahead_idx])
		bdesc_release(&look_ahead_store[look_ahead_idx]);
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

struct linux_info {
	struct block_device *bdev;
	const char * path;

	wait_queue_head_t waitq; // wait for DMA to complete

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
#if DEBUG_WRITES
	uint32_t issue;
#endif
};

static uint32_t _seq = 0;

static spinlock_t dma_outstanding_lock = SPIN_LOCK_UNLOCKED;
static int dma_outstanding = 0;

static int linux_bd_end_io(struct bio *bio, unsigned int done, int error);

static bdesc_t * linux_bd_read_block(BD_t * object, uint32_t number,
                                     uint16_t count)
{
	DEFINE_WAIT(wait);
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	struct bio *bio;
	struct bio_vec *bv;
	int vec_len;
	unsigned long flags;
	int r, i, j;
	struct linux_bio_private private[READ_AHEAD_COUNT];
	bdesc_t *blocks[READ_AHEAD_COUNT];
	int read_ahead_count = READ_AHEAD_COUNT;
	KERNEL_INTERVAL(read);

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
	spin_lock_irqsave(&dma_outstanding_lock, flags);
	dma_outstanding = 0;
	spin_unlock_irqrestore(&dma_outstanding_lock, flags);

	KDprintk(KERN_ERR "count: %d, bs: %d\n", count, info->blocksize);
	// assert((count * info->blocksize) <= 2048); Why was this assert here?
	// FIXME: 'count != 4' (and maybe READ_AHEAD_COUNT) is specific
	// to 2kB blocks
	if (count != 4)
		read_ahead_count = 1;
	TIMING_START(read);
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

		// FIXME: these error returns do not clean up
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
		// TODO: what happens if _seq wraps?
		private[j].seq = _seq++;
#if DEBUG_WRITES
		private[j].issue = -1;
#endif

		bio->bi_idx = 0;
		bio->bi_vcnt = vec_len;
		bio->bi_sector = j_number;
		bio->bi_size = info->blocksize * count;
		bio->bi_bdev = info->bdev;
		bio->bi_rw = READ;
		bio->bi_end_io = linux_bd_end_io;
		bio->bi_private = &private[j];

		spin_lock_irqsave(&dma_outstanding_lock, flags);
		dma_outstanding++;
		spin_unlock_irqrestore(&dma_outstanding_lock, flags);

		atomic_inc(&info->outstanding_io_count);

#if LINUX_BD_DEBUG_PRINT_EVERY_READ
		printk(KERN_ERR "%d\n", j_number);
#endif // LINUX_BD_DEBUG_PRINT_EVERY_READ
		generic_make_request(bio);
	}

	// wait for it to complete
	KDprintk(KERN_ERR "going to sleep! [%d]\n", private[0].seq);
	spin_lock_irqsave(&dma_outstanding_lock, flags);
	while (dma_outstanding > 0) {
		prepare_to_wait(&info->waitq, &wait, TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&dma_outstanding_lock, flags);
		schedule();
		/* FIXME: check signal_pending() */
		spin_lock_irqsave(&dma_outstanding_lock, flags);
	}
	spin_unlock_irqrestore(&dma_outstanding_lock, flags);
	finish_wait(&info->waitq, &wait);
	KDprintk(KERN_ERR "woke up!\n");

	TIMING_STOP(read, read);

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
linux_bd_end_io(struct bio *bio, unsigned int done, int error)
{
	struct linux_bio_private * private =
		(struct linux_bio_private *)(bio->bi_private);
	struct linux_info * info = private->info;
	unsigned long flags;
	int i, dir = bio->bi_rw & (1 << BIO_RW);

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

#if DEBUG_WRITES
	if (dir == WRITE && private->issue < MAXWRITES)
	{
		struct bio_vec * bv = bio_iovec_idx(bio, 0);
		struct linux_bd_write * write = &debug_writes.writes[private->issue];
		uint32_t pre_checksum = write->checksum;
		uint32_t post_checksum = block_checksum(page_address(bv->bv_page), bv->bv_len);
		if (pre_checksum != post_checksum)
			printf("pre- (0x%x) and post-write (0x%x) checksums differ for write %d (block %u)\n", pre_checksum, post_checksum, private->issue, write->blockno);
		write->completed = debug_writes_completed++;
		atomic_dec(&debug_writes_ninflight[write->blockno]);
	}
#endif

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
	{
		revision_tail_request_landing(private->bdesc);
		free(private);
	}
	bio_put(bio);

	if (dir == READ) {
		int do_wake_up = 0;
		spin_lock_irqsave(&dma_outstanding_lock, flags);
		if (!--dma_outstanding)
			do_wake_up = 1;
		spin_unlock_irqrestore(&dma_outstanding_lock, flags);

		if (do_wake_up)
			wake_up_all(&info->waitq);
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
	int vec_len, r, i;
	int revision_forward, revision_back;
	struct linux_bio_private * private;
	KERNEL_INTERVAL(write);
	
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
	if (r < 0) {
		panic("revision_tail_prepare gave: %i\n", r);
		return r;
	}
	revision_back = r;

#if DEBUG_WRITES
	private->issue = debug_writes.next;
	if (debug_writes.next < MAXWRITES)
	{
		struct linux_bd_write * write = &debug_writes.writes[debug_writes.next];
		write->blockno = block->number;
		write->checksum = block_checksum(block->ddesc->data, block->ddesc->length);
		/* NOTE: ninflight may overcount as any inflight writes could complete before we actually make the request below... */
		write->ninflight = atomic_inc_return(&debug_writes_ninflight[block->number]) - 1;
		debug_writes.next++;
	}
	else if (debug_writes.next == MAXWRITES)
	{
		printf("linux_bd: number of writes has exceeded maximum supported by debugging (%u)\n", MAXWRITES);
		debug_writes.next++;
	}
#endif

	vec_len = (block->ddesc->length + 4095) / 4096;
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
	bio->bi_end_io = linux_bd_end_io;
	bio->bi_private = private;

	if(block->ddesc->in_flight)
	{
		KERNEL_INTERVAL(wait);
		TIMING_START(wait);
		while(block->ddesc->in_flight)
		{
			revision_tail_wait_for_landing_requests();
			revision_tail_process_landing_requests();
		}
		TIMING_STOP(wait, wait);
	}

	r = revision_tail_schedule_flight();
	assert(!r);
	atomic_inc(&info->outstanding_io_count);

	KDprintk(KERN_ERR "issuing DMA write request [%d]\n", private->seq);
	TIMING_START(write);
	//printk(KERN_ERR "WRITE %d\n", block->number);
	generic_make_request(bio);

	// wait for it to complete
	/*
	printk(KERN_ERR "going to sleep!\n");
	spin_lock_irqsave(&private.dma_done_lock, flags);
	while (private.dma_done == 0) {
		spin_unlock_irqrestore(&private.dma_done_lock, flags);
		if (infty > 0) {
			infty--;
			printk(KERN_ERR "dma not done. sleeping\n");
		}
		prepare_to_wait(&info->waitq, &wait, TASK_INTERRUPTIBLE);
		schedule_timeout(500);
		waited = 1;
		spin_lock_irqsave(&private.dma_done_lock, flags);
	}
	spin_unlock_irqrestore(&private.dma_done_lock, flags);
	if (waited)
		finish_wait(&info->waitq, &wait);
	printk(KERN_ERR "woke up!\n");
	*/

	TIMING_STOP(write, write);

	r = revision_tail_inflight_ack(block, object);
	if (r < 0) {
		panic("revision_tail_acknowledge gave error: %i\n", r);
		return r;
	}
	revision_forward = r;

	if (revision_back != revision_forward)
		printf("%s(): block %u: revision_back (%d) != revision_forward (%d)\n", __FUNCTION__, block->number, revision_back, revision_forward);

	KDprintk(KERN_ERR "exiting write\n");
	return 0;
}

static int linux_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	/* FIXME: technically we should wait for all pending DMA writes to complete */
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

	TIMING_DUMP(read, "linux_bd read", "reads");
	TIMING_DUMP(write, "linux_bd write", "writes");
	TIMING_DUMP(wait, "linux_bd wait", "waits");

#if DEBUG_WRITES
	if (debug_writes_dentry)
	{
		debugfs_remove(debug_writes_dentry);
		debug_writes_dentry = NULL;
	}
#endif

	r = modman_rem_bd(bd);
	if (r < 0) return r;

	read_ahead_empty();
	while(revision_tail_flights_exist())
	{
		revision_tail_wait_for_landing_requests();
		revision_tail_process_landing_requests();
	}
	chdesc_reclaim_written();
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
	static char *_claim_ptr = "kkfsd/linux_bd";
	/* initialize to make the compiler happy */
	dev_t dev = 0;
	int r;

	r = lookup_device(path, &dev);
	if (r) {
		kdprintf(STDERR_FILENO, "error from lookup_device()\n");
		return r;
	}
	*bdev = open_by_devnum(dev, mode);
	if (IS_ERR(*bdev)) {
		kdprintf(STDERR_FILENO, "error from open_by_devnum()\n");
		return PTR_ERR(*bdev);
	}
	/* NOTE: bd_claim() will/may return -EBUSY if raid/lvm are on */
	r = bd_claim(*bdev, _claim_ptr);
	/* NOTE: bd_claim() will/may return -EBUSY if raid/lvm are on */
	if (r) {
		kdprintf(STDERR_FILENO, "error from bd_claim(): %d\n", r);
		blkdev_put(*bdev);
	}
	return r;
}

BD_t * linux_bd(const char * linux_bdev_path)
{
	struct linux_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	int r;

	if(!bd)
	{
		kdprintf(STDERR_FILENO, "malloc() for bd failed\n");
		return NULL;
	}
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		kdprintf(STDERR_FILENO, "malloc for info failed\n");
		free(bd);
		return NULL;
	}

	atomic_set(&info->outstanding_io_count, 0);

	r = open_bdev(linux_bdev_path, WRITE, &info->bdev);
	if (r) {
		kdprintf(STDERR_FILENO, "open_bdev() error\n");
		free(info);
		free(bd);
		return NULL;
	}
	
	info->blockman = blockman_create(512, NULL, NULL);
	if (!info->blockman) {
		kdprintf(STDERR_FILENO, "blockman_create() failed\n");
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
	
	BD_INIT(bd, linux_bd, info);
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		kdprintf(STDERR_FILENO, "modman_add_anon_bd() error\n");
		DESTROY(bd);
		return NULL;
	}
	
#if DEBUG_WRITES
	static struct debugfs_blob_wrapper debug_writes_blob = {
		.data = &debug_writes, .size = sizeof(debug_writes)
	};
	debug_writes_dentry = debugfs_create_blob("linux_bd_writes", 0444, NULL, &debug_writes_blob);
	if (IS_ERR(debug_writes_dentry))
	{
		printf("%s(): debugfs_create_blob(\"linux_bd_writes\") = error %d\n", __FUNCTION__, PTR_ERR(debug_writes_dentry));
		debug_writes_dentry = NULL;
	}
#endif
	
	return bd;
}
