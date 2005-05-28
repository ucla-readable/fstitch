#include <inc/types.h>
#include <inc/x86.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/blockman.h>
#include <kfs/revision.h>
#include <kfs/modman.h>
#include <kfs/ide4k_pio_bd.h>

static const uint16_t ide4k_base[2] = {0x1F0, 0x170};
static const uint16_t ide4k_reset[2] = {0x3F6, 0x376};
static const char * ide4k_names[2][2] = {{"ide4k_pio_hda", "ide4k_pio_hdb"}, {"ide4k_pio_hdc", "ide4k_pio_hdd"}};

struct ide4k_info {
	uint8_t controller;
	uint8_t disk;
	uint16_t level;
	uint32_t length;
	blockman_t * blockman;
};

static int ide4k_notbusy(uint8_t controller)
{
	uint16_t base = ide4k_base[controller];
	int start_jiffies = env->env_jiffies;
	/* wait for disk not busy */
	while((inb(base + 7) & 0xC0) != 0x40)
		if(800 <= env->env_jiffies - start_jiffies)
		{
			uint16_t reset = ide4k_reset[controller];
			printf("Warning: IDE operation timed out on controller %d\n", controller);
			/* reset the drive */
			outb(reset, 0x0E);
			sleep(2);
			outb(reset, 0x0A);
			return -1;
		}
	return 0;
}

#define SECTSIZE 4096

static int ide4k_read(uint8_t controller, uint8_t disk, uint32_t sector, void * dst, uint8_t count)
{
	uint16_t base = ide4k_base[controller];
	
	if(ide4k_notbusy(controller) == -1)
		return -1;
	
	sector *= 8;
	outb(base + 2, count*8);
	outb(base + 3, sector & 0xFF);
	outb(base + 4, (sector >> 8) & 0xFF);
	outb(base + 5, (sector >> 16) & 0xFF);
	outb(base + 6, 0xE0 | ((disk & 1) << 4) | ((sector >> 24) & 0x0F));
	/* command 0x20 means read sector */
	outb(base + 7, 0x20);
	
	if(ide4k_notbusy(controller) == -1)
		return -1;
		
	insl(base + 0, dst, count * SECTSIZE / 4);
	return 0;
}

static int ide4k_write(uint8_t controller, uint8_t disk, uint32_t sector, const void * src, uint8_t count)
{
	uint16_t base = ide4k_base[controller];
	
	if(ide4k_notbusy(controller) == -1)
		return -1;
	
	sector *= 8;
	outb(base + 2, count*8);
	outb(base + 3, sector & 0xFF);
	outb(base + 4, (sector >> 8) & 0xFF);
	outb(base + 5, (sector >> 16) & 0xFF);
	outb(base + 6, 0xE0 | ((disk & 1) << 4) | ((sector >> 24) & 0x0F));
	/* command 0x30 means write sector */
	outb(base + 7, 0x30);
	
	if(ide4k_notbusy(controller) == -1)
		return -1;

	outsl(base + 0, src, count * SECTSIZE / 4);
	return 0;
}

static uint32_t ide4k_size(uint8_t controller, uint8_t disk)
{
	uint16_t base = ide4k_base[controller];
	uint16_t id[256];
	
	if(ide4k_notbusy(controller) == -1)
		return -1;
	
	outb(base + 6, 0xE0 | ((disk & 1) << 4));
	/* command 0xEC means identify drive */
	outb(base + 7, 0xEC);
	
	if(ide4k_notbusy(controller) == -1)
		return -1;
	insl(base + 0, id, 128);
	
	return id[57] | (((uint32_t) id[58]) << 16);
}

static uint32_t ide4k_pio_tune(uint8_t controller, uint8_t disk)
{
	uint16_t base = ide4k_base[controller];

	if(ide4k_notbusy(controller) == -1)
		return -1;

	// PIO Mode 4 magic, needs refinement
	outb(base + 2, 0x0C);
	outb(base + 1, 0x03);
	outb(base + 7, 0xEF);

	if(ide4k_notbusy(controller) == -1)
		printf("Error setting controller %d to PIO MODE 4\n", controller);


	return 0;
}

static int ide4k_pio_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct ide4k_info * info = (struct ide4k_info *) OBJLOCAL(bd);
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

static int ide4k_pio_bd_get_status(void * object, int level, char * string, size_t length)
{
	/* no status to report */
	snprintf(string, length, "");
	return 0;
}

static uint32_t ide4k_pio_bd_get_numblocks(BD_t * object)
{
	return ((struct ide4k_info *) OBJLOCAL(object))->length;
}

static uint16_t ide4k_pio_bd_get_blocksize(BD_t * object)
{
	return SECTSIZE;
}

static uint16_t ide4k_pio_bd_get_atomicsize(BD_t * object)
{
	return SECTSIZE;
}

static bdesc_t * ide4k_pio_bd_read_block(BD_t * object, uint32_t number)
{
	struct ide4k_info * info = (struct ide4k_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
		return bdesc;
	
	/* make sure it's a valid block */
	if(number >= ((struct ide4k_info *) OBJLOCAL(object))->length)
		return NULL;
	
	bdesc = bdesc_alloc(number, SECTSIZE);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	/* read it */
	if(ide4k_read(info->controller, info->disk, number, bdesc->ddesc->data, 1) == -1)
		return NULL;
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;

	return bdesc;
}

static bdesc_t * ide4k_pio_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	struct ide4k_info * info = (struct ide4k_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		*synthetic = 0;
		return bdesc;
	}
	
	/* make sure it's a valid block */
	if(number >= ((struct ide4k_info *) OBJLOCAL(object))->length)
		return NULL;
	
	bdesc = bdesc_alloc(number, SECTSIZE);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	*synthetic = 1;
	
	return bdesc;
}

static int ide4k_pio_bd_cancel_block(BD_t * object, uint32_t number)
{
	return 0;
}

static int ide4k_pio_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct ide4k_info * info = (struct ide4k_info *) OBJLOCAL(object);
	
	/* make sure it's a whole block */
	if(block->ddesc->length != SECTSIZE)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= ((struct ide4k_info *) OBJLOCAL(object))->length)
		return -E_INVAL;
	
	/* prepare the block for writing */
	revision_tail_prepare(block, object);
	
	/* write it */
	if(ide4k_write(info->controller, info->disk, block->number, block->ddesc->data, 1) == -1) {
		/* the write failed; don't remove any change descriptors... */
		revision_tail_revert(block, object);
		return -E_TIMEOUT;
	}
	
	/* acknowledge the write as successful */
	revision_tail_acknowledge(block, object);
	
	return 0;
}

static int ide4k_pio_bd_sync(BD_t * object, bdesc_t * block)
{
	return 0;
}

static uint16_t ide4k_pio_bd_get_devlevel(BD_t * object)
{
	return ((struct ide4k_info *) OBJLOCAL(object))->level;
}

static int ide4k_pio_bd_destroy(BD_t * bd)
{
	struct ide4k_info * info = (struct ide4k_info *) OBJLOCAL(bd);
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	
	blockman_destroy(&info->blockman);
	free(OBJLOCAL(bd));
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * ide4k_pio_bd(uint8_t controller, uint8_t disk)
{
	struct ide4k_info * info;
	BD_t * bd;
	uint32_t length;
	
	/* check for valid controller/disk values */
	if(controller != 0 && controller != 1)
		return NULL;
	if(disk != 0 && disk != 1)
		return NULL;
	
	length = ide4k_size(controller, disk);
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
	
	BD_INIT(bd, ide4k_pio_bd, info);
	
	info->blockman = blockman_create();
	if(!info->blockman) {
		free(info);
		free(bd);
		return NULL;
	}
	
	info->controller = controller;
	info->disk = disk;
	info->length = length;
	info->level = 0;
	ide4k_pio_tune(controller, disk);

	if(modman_add_bd(bd, ide4k_names[controller][disk]))
	{
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}
