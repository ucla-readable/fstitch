#include <inc/types.h>
#include <inc/x86.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/ide_pio_bd.h>

static const uint16_t ide_base[2] = {0x1F0, 0x170};
static const char * ide_names[2][2] = {{"hda", "hdb"}, {"hdc", "hdd"}};

struct ide_info {
	uint8_t controller;
	uint8_t disk;
	uint32_t length;
};

static int ide_notbusy(uint8_t controller)
{
	uint16_t base = ide_base[controller];
	int start_jiffies = env->env_jiffies;
	/* wait for disk not busy */
	while((inb(base + 7) & 0xC0) != 0x40)
		if(1000 <= env->env_jiffies - start_jiffies)
		{
			printf("Warning: IDE operation timed out on controller %d\n", controller);
			return -1;
		}
	return 0;
}

#define SECTSIZE 512

static int ide_read(uint8_t controller, uint8_t disk, uint32_t sector, void * dst, uint8_t count)
{
	uint16_t base = ide_base[controller];
	
	if(ide_notbusy(controller) == -1)
		return -1;
	
	outb(base + 2, count);
	outb(base + 3, sector & 0xFF);
	outb(base + 4, (sector >> 8) & 0xFF);
	outb(base + 5, (sector >> 16) & 0xFF);
	outb(base + 6, 0xE0 | ((disk & 1) << 4) | ((sector >> 24) & 0x0F));
	/* command 0x20 means read sector */
	outb(base + 7, 0x20);
	
	while(count--)
	{
		if(ide_notbusy(controller) == -1)
			return -1;
		
		insl(base + 0, dst, SECTSIZE / 4);
		dst += SECTSIZE;
	}
	return 0;
}

static int ide_write(uint8_t controller, uint8_t disk, uint32_t sector, const void * src, uint8_t count)
{
	uint16_t base = ide_base[controller];
	
	if(ide_notbusy(controller) == -1)
		return -1;
	
	outb(base + 2, count);
	outb(base + 3, sector & 0xFF);
	outb(base + 4, (sector >> 8) & 0xFF);
	outb(base + 5, (sector >> 16) & 0xFF);
	outb(base + 6, 0xE0 | ((disk & 1) << 4) | ((sector >> 24) & 0x0F));
	/* command 0x30 means write sector */
	outb(base + 7, 0x30);
	
	while(count--)
	{
		if(ide_notbusy(controller) == -1)
			return -1;
	
		outsl(base + 0, src, SECTSIZE / 4);
		src += SECTSIZE;
	}
	return 0;
}

static uint32_t ide_size(uint8_t controller, uint8_t disk)
{
	uint16_t base = ide_base[controller];
	uint16_t id[SECTSIZE / 2];
	
	if(ide_notbusy(controller) == -1)
		return -1;
	
	outb(base + 6, 0xE0 | ((disk & 1) << 4));
	/* command 0xEC means identify drive */
	outb(base + 7, 0xEC);
	
	if(ide_notbusy(controller) == -1)
		return -1;
	insl(base + 0, id, SECTSIZE / 4);
	
	return id[57] | (((uint32_t) id[58]) << 16);
}
	
static uint32_t ide_pio_bd_get_numblocks(BD_t * object)
{
	return ((struct ide_info *) object->instance)->length;
}

static uint16_t ide_pio_bd_get_blocksize(BD_t * object)
{
	return SECTSIZE;
}

static uint16_t ide_pio_bd_get_atomicsize(BD_t * object)
{
	return SECTSIZE;
}

static bdesc_t * ide_pio_bd_read_block(BD_t * object, uint32_t number)
{
	struct ide_info * info = (struct ide_info *) object->instance;
	bdesc_t * bdesc;
	
	/* make sure it's a valid block */
	if(number >= ((struct ide_info *) object->instance)->length)
		return NULL;
	
	bdesc = bdesc_alloc(object, number, 0, SECTSIZE);
	if(!bdesc)
		return NULL;
	
	/* read it */
	if(ide_read(info->controller, info->disk, number, bdesc->ddesc->data, 1) == -1)
		return NULL;
	
	return bdesc;
}

static int ide_pio_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct ide_info * info = (struct ide_info *) object->instance;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != SECTSIZE)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= ((struct ide_info *) object->instance)->length)
		return -E_INVAL;
	
	/* write it */
	if(ide_write(info->controller, info->disk, block->number, block->ddesc->data, 1) == -1)
		return -E_TIMEOUT;
	
	/* drop the hot potato */
	bdesc_drop(&block);
	
	return 0;
}

static int ide_pio_bd_sync(BD_t * object, bdesc_t * block)
{
	/* drop the hot potato */
	if(block)
		bdesc_drop(&block);
	return 0;
}

static int ide_pio_bd_destroy(BD_t * bd)
{
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	free(bd->instance);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * ide_pio_bd(uint8_t controller, uint8_t disk)
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
	bd->instance = info;
	
	ASSIGN(bd, ide_pio_bd, get_numblocks);
	ASSIGN(bd, ide_pio_bd, get_blocksize);
	ASSIGN(bd, ide_pio_bd, get_atomicsize);
	ASSIGN(bd, ide_pio_bd, read_block);
	ASSIGN(bd, ide_pio_bd, write_block);
	ASSIGN(bd, ide_pio_bd, sync);
	ASSIGN_DESTROY(bd, ide_pio_bd, destroy);
	
	info->controller = controller;
	info->disk = disk;
	info->length = length;
	
	if(modman_add_bd(bd, ide_names[controller][disk]))
	{
		DESTROY(bd);
		return NULL;
	}
	
	return bd;
}
