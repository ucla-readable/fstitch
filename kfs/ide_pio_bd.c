#include <inc/lib.h>
#include <stdlib.h>
#include <string.h>
#include <inc/irq.h>
#include <inc/x86.h>
#include <lib/types.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/blockman.h>
#include <kfs/revision.h>
#include <kfs/modman.h>
#include <kfs/debug.h>
#include <kfs/ide_pio_bd.h>

static const uint8_t ide_irq[2] = {14, 15};
static const uint16_t ide_base[2] = {0x1F0, 0x170};
static const uint16_t ide_reset[2] = {0x3F6, 0x376};
static const char * ide_names[2][2] = {{"ide_pio_hda", "ide_pio_hdb"}, {"ide_pio_hdc", "ide_pio_hdd"}};

struct ide_info {
	uint8_t controller;
	uint8_t disk;
	uint16_t level;
	uint32_t length;
	blockman_t * blockman;
	uint8_t ra_count;
	uint32_t ra_sector;
	uint8_t * ra_cache;
};

static struct {
	int users;
	union {
		void * dst;
		const void * src;
	};
	uint8_t count;
	uint8_t done;
	uint8_t write;
	uint8_t busy;
} ide_requests[2] = {{users: 0, busy: 0}};

static int ide_notbusy(uint8_t controller)
{
	uint16_t base = ide_base[controller];
	int start_jiffies = env->env_jiffies;
	/* wait for disk not busy */
	while((inb(base + 7) & 0xC0) != 0x40)
	{
		if(8 * HZ <= env->env_jiffies - start_jiffies)
		{
			uint16_t reset = ide_reset[controller];
			printf("Warning: ATA operation timed out on controller %d\n", controller);
			/* reset the drive */
			outb(reset, 0x0E);
			jsleep(HZ / 50);
			outb(reset, 0x0A);
			return -1;
		}
		/* yield after checking the time, so we don't
		 * time out merely because of the scheduler */
		sys_yield();
	}
	return 0;
}

#define IDE_SECTSIZE 512

static void ide_irq_handler(int irq)
{
	uint8_t controller;
	uint16_t base;
	if(irq == ide_irq[0])
		controller = 0;
	else if(irq == ide_irq[1])
		controller = 1;
	else
	{
		printf("Unhandled ATA interrupt %d!\n", irq);
		return;
	}
	base = ide_base[controller];
	if(!ide_requests[controller].busy)
	{
		printf("Unexpected ATA interrupt %d\n", irq);
		return;
	}
	/* the controller expects us to query the status register... */
	if(ide_notbusy(controller) == -1)
	{
		printf("Extra ATA interrupt %d\n", irq);
		return;
	}
	if(ide_requests[controller].write)
	{
		/* handle a write interrupt */
		ide_requests[controller].done++;
		if(ide_requests[controller].count--)
		{
			/* need to send the next block */
			outsl(base + 0, ide_requests[controller].src, IDE_SECTSIZE / 4);
			ide_requests[controller].src += IDE_SECTSIZE;
		}
		else
		{
			/* FIXME: notify the user? */
			ide_requests[controller].busy = 0;
		}
	}
	else
	{
		/* handle a read interrupt */
		ide_requests[controller].done++;
		insl(base + 0, ide_requests[controller].dst, IDE_SECTSIZE / 4);
		ide_requests[controller].dst += IDE_SECTSIZE;
		if(!--ide_requests[controller].count)
		{
			/* FIXME: notify the user? */
			ide_requests[controller].busy = 0;
		}
	}
}

static int ide_read(uint8_t controller, uint8_t disk, uint32_t sector, void * dst, uint8_t count)
{
	uint16_t base = ide_base[controller];
	
	if(ide_notbusy(controller) == -1)
		return -1;
	
	ide_requests[controller].dst = dst;
	ide_requests[controller].count = count;
	ide_requests[controller].done = 0;
	ide_requests[controller].write = 0;
	ide_requests[controller].busy = 1;
	
	outb(base + 2, count);
	outb(base + 3, sector & 0xFF);
	outb(base + 4, (sector >> 8) & 0xFF);
	outb(base + 5, (sector >> 16) & 0xFF);
	outb(base + 6, 0xE0 | ((disk & 1) << 4) | ((sector >> 24) & 0x0F));
	/* command 0x20 means read sector */
	outb(base + 7, 0x20);
	
	while(ide_requests[controller].busy)
		sys_yield();
	return 0;
}

static int ide_write(uint8_t controller, uint8_t disk, uint32_t sector, const void * src, uint8_t count)
{
	uint16_t base = ide_base[controller];
	
	if(ide_notbusy(controller) == -1)
		return -1;
	
	ide_requests[controller].src = src;
	ide_requests[controller].count = count;
	ide_requests[controller].done = -1;
	ide_requests[controller].write = 1;
	ide_requests[controller].busy = 1;
	
	outb(base + 2, count);
	outb(base + 3, sector & 0xFF);
	outb(base + 4, (sector >> 8) & 0xFF);
	outb(base + 5, (sector >> 16) & 0xFF);
	outb(base + 6, 0xE0 | ((disk & 1) << 4) | ((sector >> 24) & 0x0F));
	/* command 0x30 means write sector */
	outb(base + 7, 0x30);
	
	/* simulate the first interrupt */
	ide_irq_handler(ide_irq[controller]);
	
	while(ide_requests[controller].busy)
		sys_yield();
	return 0;
}

static char * ide_string(char * string, const uint16_t * ide, int length)
{
	uint16_t * string_u16 = (uint16_t *) string;
	string[length] = 0;
	length /= 2;
	while(length-- > 0)
		string_u16[length] = (ide[length] << 8) | (ide[length] >> 8);
	return string;
}

/* FIXME: use interrupts to do the read, make sure we don't clobber the controller (use ide_requests) */
static uint32_t ide_size(uint8_t controller, uint8_t disk)
{
	uint16_t base = ide_base[controller];
	uint16_t id[256];
	char string[41];
	
	if(ide_notbusy(controller) == -1)
		return -1;
	
	outb(base + 6, 0xE0 | ((disk & 1) << 4));
	/* command 0xEC means identify drive */
	outb(base + 7, 0xEC);
	
	if(ide_notbusy(controller) == -1)
		return -1;
	insl(base + 0, id, IDE_SECTSIZE / 4);
	
	/* print out some drive information */
	printf("ATA %s %s:", controller ? "secondary" : "primary", disk ? "slave" : "master");
	printf(" %s\n", ide_string(string, &id[27], 40));
	printf("  %d cylinders, %d heads, %d sectors\n", id[1], id[3], id[6]);
	printf("  Serial number: %s\n", ide_string(string, &id[10], 20));
	printf("  Firmware revision: %s\n", ide_string(string, &id[23], 8));
	printf("  DMA supported: %s", ((id[49] >> 8) & 1) ? "yes" : "no");
	printf("  LBA supported: ");
	if((id[49] >> 9) & 1)
	{
		uint32_t lba_sectors = id[60] | (((uint32_t) id[61]) << 16);
		/* in QEMU, this value is the size of the filesystem image
		 * and not necessarily that of the configured disk geometry */
		printf("yes\n  LBA sectors: %d\n", lba_sectors);
	}
	else
		printf("no\n");
	
	if(id[53] & 1)
	{
		/* id[54-58] are valid */
		printf("  Total sectors: %d\n", id[57] | (((uint32_t) id[58]) << 16));
		return id[57] | (((uint32_t) id[58]) << 16);
	}
	
	printf("  Total sectors (CHS): %d\n", (uint32_t) id[1] * (uint32_t) id[3] * (uint32_t) id[6]);
	return (uint32_t) id[1] * (uint32_t) id[3] * (uint32_t) id[6];
}

/* FIXME: make sure we don't clobber the controller doing this (use ide_requests) */
static uint32_t ide_pio_tune(uint8_t controller, uint8_t disk)
{
	uint16_t base = ide_base[controller];
	
	if(ide_notbusy(controller) == -1)
		return -1;
	
	// PIO Mode 4 magic, needs refinement
	outb(base + 2, 0x0C);
	outb(base + 1, 0x03);
	outb(base + 7, 0xEF);
	
	if(ide_notbusy(controller) == -1)
		printf("Error setting controller %d to PIO MODE 4\n", controller);
	
	return 0;
}

static int ide_pio_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct ide_info * info = (struct ide_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "controller: %d, drive: %d, count: %d, atomic: 512", info->controller, info->disk, info->length);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "(%d, %d), count: %d", info->controller, info->disk, info->length);
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "controller: %d, drive: %d, count: %d", info->controller, info->disk, info->length);
	}
	return 0;
}

static int ide_pio_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}

static uint32_t ide_pio_bd_get_numblocks(BD_t * object)
{
	return ((struct ide_info *) OBJLOCAL(object))->length;
}

static uint16_t ide_pio_bd_get_blocksize(BD_t * object)
{
	return IDE_SECTSIZE;
}

static uint16_t ide_pio_bd_get_atomicsize(BD_t * object)
{
	return IDE_SECTSIZE;
}

static bdesc_t * ide_pio_bd_read_block(BD_t * object, uint32_t number)
{
	struct ide_info * info = (struct ide_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	int need_to_read = 0;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
		return bdesc;
	
	/* make sure it's a valid block */
	if(number >= ((struct ide_info *) OBJLOCAL(object))->length)
		return NULL;
	
	bdesc = bdesc_alloc(number, IDE_SECTSIZE);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	if(info->ra_count == 0)
	{
		/* read it */
		if(ide_read(info->controller, info->disk, number, bdesc->ddesc->data, 1) == -1)
			return NULL;
	}
	else
	{
		/* read ahead */
		if(info->ra_sector != 0)
		{
			/* we have something in the cache */
			if(info->ra_sector <= number && number < info->ra_sector+info->ra_count)
				/* cache hit */
				memcpy(bdesc->ddesc->data, info->ra_cache + IDE_SECTSIZE * (number - info->ra_sector), IDE_SECTSIZE);
			else
				/* cache miss */
				need_to_read = 1;
		}
		else
			/* nothing in the cache */
			need_to_read = 1;
		
		if(need_to_read)
		{
			if(number == 0)
			{
				info->ra_sector = 0;
				if(ide_read(info->controller, info->disk, number, bdesc->ddesc->data, 1) == -1)
					return NULL;
			}
			else
			{
				/* read it */
				if(ide_read(info->controller, info->disk, number, info->ra_cache, info->ra_count) == -1)
					return NULL;
				memcpy(bdesc->ddesc->data, info->ra_cache, IDE_SECTSIZE);
				info->ra_sector = number;
			}
		}
	}
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return bdesc;
}

static bdesc_t * ide_pio_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	struct ide_info * info = (struct ide_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		*synthetic = 0;
		return bdesc;
	}
	
	/* make sure it's a valid block */
	if(number >= ((struct ide_info *) OBJLOCAL(object))->length)
		return NULL;
	
	bdesc = bdesc_alloc(number, IDE_SECTSIZE);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	*synthetic = 1;
	
	return bdesc;
}

static int ide_pio_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct ide_info * info = (struct ide_info *) OBJLOCAL(object);
	datadesc_t * ddesc = blockman_lookup(info->blockman, number);
	if(ddesc)
		blockman_remove(ddesc);
	return 0;
}

static int ide_pio_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct ide_info * info = (struct ide_info *) OBJLOCAL(object);
	
	/* make sure it's a whole block */
	if(block->ddesc->length != IDE_SECTSIZE)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= ((struct ide_info *) OBJLOCAL(object))->length)
		return -E_INVAL;
	
	/* prepare the block for writing */
	revision_tail_prepare(block, object);
	
	KFS_DEBUG_DBWAIT(block);
	
	/* write it */
	if(ide_write(info->controller, info->disk, block->number, block->ddesc->data, 1) == -1)
	{
		/* the write failed; don't remove any change descriptors... */
		revision_tail_revert(block, object);
		return -E_TIMEOUT;
	}
	
	/* acknowledge the write as successful */
	revision_tail_acknowledge(block, object);
	
	return 0;
}

static int ide_pio_bd_sync(BD_t * object, uint32_t block, chdesc_t * ch)
{
	return 0;
}

static uint16_t ide_pio_bd_get_devlevel(BD_t * object)
{
	return ((struct ide_info *) OBJLOCAL(object))->level;
}

static int ide_pio_bd_destroy(BD_t * bd)
{
	struct ide_info * info = (struct ide_info *) OBJLOCAL(bd);
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	
	if(!--ide_requests[info->controller].users)
		request_irq(ide_irq[info->controller], NULL);
	free(info->ra_cache);
	blockman_destroy(&info->blockman);
	free(info);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * ide_pio_bd(uint8_t controller, uint8_t disk, uint8_t readahead)
{
	struct ide_info * info;
	BD_t * bd;
	uint32_t length;
	
	/* check for valid controller/disk values */
	if(controller != 0 && controller != 1)
		return NULL;
	if(disk != 0 && disk != 1)
		return NULL;
	
	length = ide_size(controller, disk);
	if(length == -1)
		return NULL;

	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	info->ra_count = readahead;
	info->ra_cache = malloc(IDE_SECTSIZE * info->ra_count);
	if(!info->ra_cache)
	{
		free(info);
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, ide_pio_bd, info);
	
	info->blockman = blockman_create();
	if(!info->blockman)
	{
		free(info->ra_cache);
		free(info);
		free(bd);
		return NULL;
	}
	
	info->controller = controller;
	info->disk = disk;
	info->length = length;
	info->level = 0;
	info->ra_sector = 0;
	ide_pio_tune(controller, disk);
	
	if(modman_add_bd(bd, ide_names[controller][disk]))
	{
		DESTROY(bd);
		return NULL;
	}
	
	if(!ide_requests[controller].users++)
	{
		if(request_irq(ide_irq[controller], ide_irq_handler) < 0)
		{
			/* DESTROY() will decrement users again */
			DESTROY(bd);
			return NULL;
		}
	}
	
	return bd;
}
