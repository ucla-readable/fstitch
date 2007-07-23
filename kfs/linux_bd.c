#include <lib/platform.h>
#include <lib/pool.h>

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

#define DEBUG_TIMING 0
#include <kfs/kernel_timing.h>
KERNEL_TIMING(read);
KERNEL_TIMING(write);
KERNEL_TIMING(wait);
#if DEBUG_TIMING
#include <lib/jiffies.h>
#endif

#ifdef CONFIG_MD
#warning linux_bd is (apparently) incompatible with RAID/LVM
#endif

#ifndef BIO_RW_FUA
#warning BIO_RW_FUA is not available: writes will be unsafe across power outage unless the disk write cache is disabled
/* WRITE is 1, so 1 << 0 == WRITE and thus WRITE | (1 << BIO_RW_FUA) == WRITE */
#define BIO_RW_FUA 0
#endif

#define PRINT_EVERY_READ 0
#define DEBUG_WRITES 0

#define READ_AHEAD_COUNT 32
#define READ_AHEAD_BUFFER 320

#define DEBUG_LINUX_BD 0
#if DEBUG_LINUX_BD
#define KDprintk(x...) printk(x)
#else
#define KDprintk(x...)
#endif

#if DEBUG_WRITES
#include <linux/debugfs.h>
#include <kfs/linux_bd_debug.h>
static struct linux_bd_writes debug_writes;
static atomic_t debug_writes_ninflight[MAXBLOCKNO];
static uint32_t debug_writes_completed;
static struct dentry * debug_writes_dentry;
#endif

#define RANDOM_REBOOT 0
#if RANDOM_REBOOT
# include <linux/random.h>
# include <linux/reboot.h>
#endif

#define LINUX_BLOCKSIZE 512

struct linux_info {
	struct block_device * bdev;
	const char * path;
	blockman_t * blockman;
	
	atomic_t outstanding_io_count;
	spinlock_t dma_outstanding_lock;
	int dma_outstanding;
	/* wait for DMA to complete */
	wait_queue_head_t waitq;
#if DEBUG_LINUX_BD
	uint32_t seq;
#endif
	
	int read_ahead_idx;
	bdesc_t * read_ahead[READ_AHEAD_BUFFER];
};

struct linux_bio_private {
	struct linux_info * info;
	bdesc_t * bdesc;
	uint32_t number;
	uint16_t count;
#if DEBUG_WRITES
	uint32_t issue;
#endif
#if DEBUG_LINUX_BD
	uint32_t seq;
#endif
};

DECLARE_POOL(bio_private, struct linux_bio_private);
static int n_linux_instances;

static int linux_bd_end_io(struct bio *bio, unsigned int done, int error)
{
	struct linux_bio_private * private = (struct linux_bio_private *) bio->bi_private;
	struct linux_info * info = private->info;
	int i, dir = bio->bi_rw & (1 << BIO_RW);
	unsigned long flags;
	
	assert(info);
	assert(info->waitq.task_list.next);
	assert(info->waitq.task_list.next->next);
	
	KDprintk(KERN_ERR "[%d] done w/ bio transfer (%d, %d)\n", private->seq, done, error);
	if(bio->bi_size)
	{
		/* Everyone else in the [2.6.12] Linux kernel returns 1 here;
		 * we follow their lead. No one inspects bi_end_io()'s return value,
		 * either. */
		/* Should we decrement info->outstanding_io_count at this exit?
		 * It sounds like non-zero bi_size may mean the I/O is not yet
		 * complete. So we'll not and hope it works out. */
		return 1;
	}
	KDprintk(KERN_ERR "[%d] done w/ bio transfer 2\n", private->seq);
	
#if DEBUG_WRITES
	if(dir == WRITE && private->issue < MAXWRITES)
	{
		struct bio_vec * bv = bio_iovec_idx(bio, 0);
		struct linux_bd_write * write = &debug_writes.writes[private->issue];
		uint32_t pre_checksum = write->checksum;
		uint32_t post_checksum = block_checksum(page_address(bv->bv_page), bv->bv_len);
		if(pre_checksum != post_checksum)
			printk("pre- (0x%x) and post-write (0x%x) checksums differ for write %d (block %u)\n", pre_checksum, post_checksum, private->issue, write->blockno);
		write->completed = debug_writes_completed++;
		if(write->blockno < MAXBLOCKNO)
			atomic_dec(&debug_writes_ninflight[write->blockno]);
	}
#endif
	
	assert(bio->bi_vcnt == 1);
	for(i = 0; i < bio->bi_vcnt; i++)
	{
		int len;
		void * p = page_address(bio_iovec_idx(bio, i)->bv_page);
		
		assert(p);
		if(dir == READ)
		{
			len = 4096;
			if(i + 1 == bio->bi_vcnt)
			{
				len = (private->count * LINUX_BLOCKSIZE) % 4096;
				if(!len)
					len = 4096;
			}
			memcpy(private->bdesc->ddesc->data + (4096 * i), p, len);
		}
		__free_page(bio_iovec_idx(bio, i)->bv_page);
		bio_iovec_idx(bio, i)->bv_page = NULL;
		bio_iovec_idx(bio, i)->bv_len = 0;
		bio_iovec_idx(bio, i)->bv_offset = 0;
	}
	
	if(dir == READ)
		private->bdesc->ddesc->synthetic = 0;
	else if(dir == WRITE)
	{
		revision_tail_request_landing(private->bdesc);
		bio_private_free(private);
	}
	bio_put(bio);
	
	if(dir == READ)
	{
		int do_wake_up = 0;
		spin_lock_irqsave(&info->dma_outstanding_lock, flags);
		if(!--info->dma_outstanding)
			do_wake_up = 1;
		spin_unlock_irqrestore(&info->dma_outstanding_lock, flags);
		
		if(do_wake_up)
			wake_up_all(&info->waitq);
	}
#if DEBUG_TIMING
	else
	{
		static int last_jif = 0;
		static int jif_writes = 0;
		int now = jiffy_time();
		if(!last_jif)
			last_jif = now;
		jif_writes++;
		if(now - last_jif >= HZ)
		{
			int wpds = jif_writes * HZ * 10 / (now - last_jif);
			printk("linux_bd: writes/sec = %d.%d\n", wpds / 10, wpds % 10);
			last_jif = now;
			jif_writes = 0;
		}
	}
#endif
	
	atomic_dec(&info->outstanding_io_count);
	return error;
}

static bdesc_t * linux_bd_read_block(BD_t * object, uint32_t number, uint16_t count)
{
	DEFINE_WAIT(wait);
	struct linux_info * info = (struct linux_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	struct bio * bio;
	struct bio_vec * bv;
	int vec_len;
	unsigned long flags;
	int r, i, j;
	struct linux_bio_private private[READ_AHEAD_COUNT];
	bdesc_t * blocks[READ_AHEAD_COUNT];
	KERNEL_INTERVAL(read);
	
	KDprintk(KERN_ERR "entered read (blk: %d, cnt: %d)\n", number, count);
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		assert(bdesc->count == count);
		if(!bdesc->ddesc->synthetic)
		{
			KDprintk(KERN_ERR "already got it. done w/ read\n");
			return bdesc;
		}
	}
	else if(!count || number + count > object->numblocks)
	{
		printk(KERN_ERR "bailing on read 1\n");
		return NULL;
	}
	
	KDprintk(KERN_ERR "starting real read work\n");
	spin_lock_irqsave(&info->dma_outstanding_lock, flags);
	info->dma_outstanding = 0;
	spin_unlock_irqrestore(&info->dma_outstanding_lock, flags);
	
	KDprintk(KERN_ERR "count: %d, bs: %d\n", count, LINUX_BLOCKSIZE);
	TIMING_START(read);
	for(j = 0; j < READ_AHEAD_COUNT; j++)
	{
		uint32_t j_number = number + (count * j);
		datadesc_t * dd;
	
		if(j_number + count > object->numblocks)
		{
			blocks[j] = NULL;
			continue;
		}
		
		dd = blockman_lookup(info->blockman, j_number);
		if(dd && !dd->synthetic)
		{
			blocks[j] = NULL;
			continue;
		}
		else if(dd)
		{
			if(bdesc->ddesc == dd)
				blocks[j] = bdesc;
			else
			{
				/* blockman_managed_lookup? */
				blocks[j] = bdesc_alloc_wrap(dd, j_number, dd->length / info->blockman->length);
				if(blocks[j] == NULL)
					return NULL;
				bdesc_autorelease(blocks[j]);
			}
		}
		else
		{
			blocks[j] = bdesc_alloc(j_number, LINUX_BLOCKSIZE, count);
			if(blocks[j] == NULL)
				return NULL;
			bdesc_autorelease(blocks[j]);
		}
		
		vec_len = (count * LINUX_BLOCKSIZE + 4095) / 4096;
		assert(vec_len == 1);
		
		/* FIXME: these error returns do not clean up */
		bio = bio_alloc(GFP_KERNEL, vec_len);
		if(!bio)
		{
			printk(KERN_ERR "bio_alloc() failed\n");
			return NULL;
		}
		for(i = 0; i < vec_len; i++)
		{
			bv = bio_iovec_idx(bio, i);
			bv->bv_page = alloc_page(GFP_KERNEL);
			if(!bv->bv_page)
			{
				printk(KERN_ERR "alloc_page() failed\n");
				return NULL;
			}
			bv->bv_len = LINUX_BLOCKSIZE * count;
			bv->bv_offset = 0;
		}
		
		private[j].info = info;
		private[j].bdesc = blocks[j];
		private[j].number = j_number;
		private[j].count = count;
#if DEBUG_LINUX_BD
		private[j].seq = info->seq++;
#endif
#if DEBUG_WRITES
		private[j].issue = -1;
#endif
		
		bio->bi_idx = 0;
		bio->bi_vcnt = vec_len;
		bio->bi_sector = j_number;
		bio->bi_size = LINUX_BLOCKSIZE * count;
		bio->bi_bdev = info->bdev;
		bio->bi_rw = READ;
		bio->bi_end_io = linux_bd_end_io;
		bio->bi_private = &private[j];
		
		spin_lock_irqsave(&info->dma_outstanding_lock, flags);
		info->dma_outstanding++;
		spin_unlock_irqrestore(&info->dma_outstanding_lock, flags);
		
		atomic_inc(&info->outstanding_io_count);
		
#if PRINT_EVERY_READ
		printk(KERN_ERR "%d\n", j_number);
#endif
		generic_make_request(bio);
	}
	
	/* wait for it to complete */
	KDprintk(KERN_ERR "going to sleep! [%d]\n", private[0].seq);
	spin_lock_irqsave(&info->dma_outstanding_lock, flags);
	while(info->dma_outstanding > 0)
	{
		prepare_to_wait(&info->waitq, &wait, TASK_INTERRUPTIBLE);
		spin_unlock_irqrestore(&info->dma_outstanding_lock, flags);
		schedule();
		/* FIXME: check signal_pending() */
		spin_lock_irqsave(&info->dma_outstanding_lock, flags);
	}
	spin_unlock_irqrestore(&info->dma_outstanding_lock, flags);
	finish_wait(&info->waitq, &wait);
	KDprintk(KERN_ERR "woke up!\n");
	
	TIMING_STOP(read, read);
	
	for(j = 0; j < READ_AHEAD_COUNT; j++)
	{
		if(!blocks[j])
			continue;
		if(j)
		{
			bdesc_retain(blocks[j]);
			if(info->read_ahead[info->read_ahead_idx])
				bdesc_release(&info->read_ahead[info->read_ahead_idx]);
			info->read_ahead[info->read_ahead_idx] = blocks[j];
			if(++info->read_ahead_idx == READ_AHEAD_BUFFER)
				info->read_ahead_idx = 0;
		}
		
		r = blockman_managed_add(info->blockman, blocks[j]);
		assert(r >= 0);
		if(r < 0)
			return NULL;
	}
	
	KDprintk(KERN_ERR "exiting read\n");
	return blocks[0];
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
	if(!count || number + count > object->numblocks)
		return NULL;
	
	bdesc = bdesc_alloc(number, LINUX_BLOCKSIZE, count);
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
	struct bio * bio;
	struct bio_vec * bv;
	int vec_len, r, i;
	int revision_forward, revision_back;
	struct linux_bio_private * private;
	KERNEL_INTERVAL(write);
	
#if RANDOM_REBOOT
	u32 ru32;
	if ((ru32 = random32()) < 100000)
	{
		printf("DEATH TO YOUR BOOT (random says %u)\n", ru32);
		emergency_restart();
	}
#endif

	KDprintk(KERN_ERR "entered write (blk: %d, cnt: %d)\n", block->number, block->count);
	if((LINUX_BLOCKSIZE * block->count) != block->ddesc->length)
	{
		kpanic("wrote block with bad length (%d bytes)\n", block->ddesc->length);
		return -EINVAL;
	}
	if(block->number >= object->numblocks)
	{
		kpanic("wrote bad block number\n");
		return -EINVAL;
	}
	
	private = bio_private_alloc();
	assert(private);
	
	KDprintk(KERN_ERR "starting real work for the write\n");
	r = revision_tail_prepare(block, object);
	if(r < 0)
	{
		kpanic("revision_tail_prepare gave: %i\n", r);
		return r;
	}
	revision_back = r;
	
#if DEBUG_WRITES
	private->issue = debug_writes.next;
	if(debug_writes.next < MAXWRITES)
	{
		struct linux_bd_write * write = &debug_writes.writes[debug_writes.next];
		write->blockno = block->number;
		write->checksum = block_checksum(block->ddesc->data, block->ddesc->length);
		/* NOTE: ninflight may overcount as any inflight writes could complete before we actually make the request below... */
		if(block->number < MAXBLOCKNO)
			write->ninflight = atomic_inc_return(&debug_writes_ninflight[block->number]) - 1;
		else
			write->ninflight = -1;
		debug_writes.next++;
	}
	else if(debug_writes.next == MAXWRITES)
	{
		printk("linux_bd: number of writes has exceeded maximum supported by debugging (%u)\n", MAXWRITES);
		debug_writes.next++;
	}
#endif
	
	vec_len = (block->ddesc->length + 4095) / 4096;
	assert(vec_len == 1);
	
	bio = bio_alloc(GFP_KERNEL, vec_len);
	if(!bio)
	{
		printk(KERN_ERR "bio_alloc() failed()\n");
		return -ENOMEM;
	}
	for(i = 0; i < vec_len; i++)
	{
		bv = bio_iovec_idx(bio, i);
		bv->bv_page = alloc_page(GFP_KERNEL);
		if(!bv->bv_page)
		{
			printk(KERN_ERR "alloc_page() failed\n");
			return -ENOMEM;
		}
		/* this memcpy always writes to the beginning of the page,
		 * which works fine if you just have one block, but is a
		 * problem if you have more. right now you can only pass one
		 * block to this function, so it's not a problem. */
		memcpy(page_address(bv->bv_page), block->ddesc->data, block->ddesc->length);
		bv->bv_len = block->ddesc->length;
		bv->bv_offset = 0;
	}
	
	private->info = info;
	private->bdesc = block;
	private->number = block->number;
	private->count = block->count;
#if DEBUG_LINUX_BD
	private->seq = info->seq++;
#endif
	
	bio->bi_idx = 0;
	bio->bi_vcnt = vec_len;
	bio->bi_sector = block->number;
	bio->bi_size = block->ddesc->length;
	bio->bi_bdev = info->bdev;
	bio->bi_rw = WRITE | (1 << BIO_RW_FUA);
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
	generic_make_request(bio);
	
	TIMING_STOP(write, write);
	
	r = revision_tail_inflight_ack(block, object);
	if(r < 0)
	{
		kpanic("revision_tail_acknowledge gave error: %i\n", r);
		return r;
	}
	revision_forward = r;
	
	if(revision_back != revision_forward)
		printk("%s(): block %u: revision_back (%d) != revision_forward (%d)\n", __FUNCTION__, block->number, revision_back, revision_forward);
	
	KDprintk(KERN_ERR "exiting write\n");
	return 0;
}

static int linux_bd_flush(BD_t * object, uint32_t block, chdesc_t * ch)
{
	/* FIXME: technically we should wait for all pending DMA writes to complete */
	return FLUSH_EMPTY;
}

static chdesc_t ** linux_bd_get_write_head(BD_t * object)
{
	return NULL;
}

static int32_t linux_bd_get_block_space(BD_t * object)
{
	return 0;
}

int linux_bd_destroy(BD_t * bd)
{
	struct linux_info * info = (struct linux_info *) OBJLOCAL(bd);
	bool wait_printed = 0;
	int r;
	
	while(atomic_read(&info->outstanding_io_count))
	{
		if(!wait_printed)
		{
			printk("%s: waiting for %d outstanding I/Os\n", __FUNCTION__, atomic_read(&info->outstanding_io_count));
			wait_printed = 1;
		}
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 10);
	}
	
	TIMING_DUMP(read, "linux_bd read", "reads");
	TIMING_DUMP(write, "linux_bd write", "writes");
	TIMING_DUMP(wait, "linux_bd wait", "waits");
	
#if DEBUG_WRITES
	if(debug_writes_dentry)
	{
		debugfs_remove(debug_writes_dentry);
		debug_writes_dentry = NULL;
	}
#endif
	
	r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	
	for(r = 0; r < READ_AHEAD_BUFFER; r++)
		if(info->read_ahead[r])
			bdesc_release(&info->read_ahead[r]);
	
	while(revision_tail_flights_exist())
	{
		revision_tail_wait_for_landing_requests();
		revision_tail_process_landing_requests();
	}
	chdesc_reclaim_written();
	blockman_destroy(&info->blockman);

	n_linux_instances--;
	if(!n_linux_instances)
		bio_private_free_all();
	bd_release(info->bdev);
	blkdev_put(info->bdev);
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	
	return 0;
}

/* Ripped from linux-2.6.15.1/drivers/md/dm-table.c:
 * Convert a device path to a dev_t. */
static int lookup_device(const char * path, dev_t * dev)
{
	struct nameidata nd;
	struct inode *inode;
	int r;
	
	r = path_lookup(path, LOOKUP_FOLLOW, &nd);
	if(r)
		return r;
	
	inode = nd.dentry->d_inode;
	if(!inode)
	{
		r = -ENOENT;
		goto out;
	}
	
	if(!S_ISBLK(inode->i_mode))
	{
		r = -ENOTBLK;
		goto out;
	}
	
	*dev = inode->i_rdev;
	
out:
	path_release(&nd);
	return r;
}

static int open_bdev(const char *path, int mode, struct block_device ** bdev)
{
	static char *_claim_ptr = "kkfsd/linux_bd";
	/* initialize to make the compiler happy */
	dev_t dev = 0;
	int r;
	
	r = lookup_device(path, &dev);
	if(r)
	{
		printk("error from lookup_device()\n");
		return r;
	}
	
	*bdev = open_by_devnum(dev, mode);
	if(IS_ERR(*bdev))
	{
		printk("error from open_by_devnum()\n");
		return PTR_ERR(*bdev);
	}
	
	/* NOTE: bd_claim() will/may return -EBUSY if RAID/LVM are on */
	r = bd_claim(*bdev, _claim_ptr);
	if(r)
	{
		printk("error from bd_claim(): %d\n", r);
		blkdev_put(*bdev);
	}
	
	return r;
}

BD_t * linux_bd(const char * linux_bdev_path)
{
	struct linux_info * info;
	BD_t * bd = malloc(sizeof(*bd));
	int r;
	
#if !BIO_RW_FUA
	printk("Warning: not compiled with BIO_RW_FUA: writes will not be safe unless the disk write cache is disabled\n");
#endif
	
	if(!bd)
	{
		printk("malloc() for bd failed\n");
		return NULL;
	}
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		printk("malloc for info failed\n");
		free(bd);
		return NULL;
	}
	
	atomic_set(&info->outstanding_io_count, 0);
	
	r = open_bdev(linux_bdev_path, WRITE, &info->bdev);
	if(r)
	{
		printk("open_bdev() error\n");
		free(info);
		free(bd);
		return NULL;
	}
	
	info->blockman = blockman_create(512, NULL, NULL);
	if(!info->blockman)
	{
		printk("blockman_create() failed\n");
		bd_release(info->bdev);
		blkdev_put(info->bdev);
		free(info);
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, linux_bd, info);
	
	bd->blocksize = LINUX_BLOCKSIZE;
	bd->numblocks = info->bdev->bd_disk->capacity;
	bd->atomicsize = LINUX_BLOCKSIZE;
	info->read_ahead_idx = 0;
	for(r = 0; r < READ_AHEAD_BUFFER; r++)
		info->read_ahead[r] = NULL;
	info->dma_outstanding = 0;
	spin_lock_init(&info->dma_outstanding_lock);
#if DEBUG_LINUX_BD
	info->seq = 0;
#endif
	init_waitqueue_head(&info->waitq);
	bd->level = 0;
	bd->graph_index = 0;
	if(bd->graph_index >= NBDINDEX)
	{
		DESTROY(bd);
		return NULL;
	}
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
	{
		printk("modman_add_anon_bd() error\n");
		DESTROY(bd);
		return NULL;
	}
	
#if DEBUG_WRITES
	static struct debugfs_blob_wrapper debug_writes_blob = {
		.data = &debug_writes, .size = sizeof(debug_writes)
	};
	debug_writes_dentry = debugfs_create_blob("linux_bd_writes", 0444, NULL, &debug_writes_blob);
	if(IS_ERR(debug_writes_dentry))
	{
		printk("%s(): debugfs_create_blob(\"linux_bd_writes\") = error %d\n", __FUNCTION__, (int) PTR_ERR(debug_writes_dentry));
		debug_writes_dentry = NULL;
	}
#endif
	
	n_linux_instances++;
	
	return bd;
}
