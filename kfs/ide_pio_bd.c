#include <inc/types.h>
#include <inc/x86.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/ide_pio_bd.h>

struct ide_info {
	uint8_t controller;
	uint8_t disk;
	uint32_t length;
};

static void ide_notbusy(void)
{
	/* wait for disk not busy */
	while((inb(0x1F7) & 0xC0) != 0x40);
}

#define SECTSIZE 512

static void ide_read(uint32_t disk, uint32_t sector, void * dst, uint8_t count)
{
	ide_notbusy();
	
	outb(0x1F2, count);
	outb(0x1F3, sector & 0xFF);
	outb(0x1F4, (sector >> 8) & 0xFF);
	outb(0x1F5, (sector >> 16) & 0xFF);
	outb(0x1F6, 0xE0 | ((disk & 1) << 4) | ((sector >> 24) & 0x0F));
	/* command 0x20 means read sector */
	outb(0x1F7, 0x20);
	
	while(count--)
	{
		ide_notbusy();
		
		insl(0x1F0, dst, SECTSIZE / 4);
		dst += SECTSIZE;
	}
}

static void ide_write(uint32_t disk, uint32_t sector, const void * src, uint8_t count)
{
	ide_notbusy();
	
	outb(0x1F2, count);
	outb(0x1F3, sector & 0xFF);
	outb(0x1F4, (sector >> 8) & 0xFF);
	outb(0x1F5, (sector >> 16) & 0xFF);
	outb(0x1F6, 0xE0 | ((disk & 1) << 4) | ((sector >> 24) & 0x0F));
	/* command 0x30 means write sector */
	outb(0x1F7, 0x30);
	
	while(count--)
	{
		ide_notbusy();
	
		outsl(0x1F0, src, SECTSIZE / 4);
		src += SECTSIZE;
	}
}

static uint32_t ide_size(uint32_t disk)
{
	uint16_t id[SECTSIZE / 2];
	
	ide_notbusy();
	
	outb(0x1F6, 0xE0 | ((disk & 1) << 4));
	/* command 0xEC means identify drive */
	outb(0x1F7, 0xEC);
	
	ide_notbusy();
	insl(0x1F0, id, SECTSIZE / 4);
	
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
	bdesc_t * bdesc;
	
	/* make sure it's a valid block */
	if(number >= ((struct ide_info *) object->instance)->length)
		return NULL;
	
	bdesc = bdesc_alloc(object, number, 0, SECTSIZE);
	if(!bdesc)
		return NULL;
	
	/* read it */
	ide_read(((struct ide_info *) object->instance)->disk, number, bdesc->ddesc->data, 1);
	
	return bdesc;
}

static int ide_pio_bd_write_block(BD_t * object, bdesc_t * block)
{
	/* make sure this is the right block device */
	if(block->bd != object)
		return -1;
	
	/* make sure it's a whole block */
	if(block->offset || block->length != SECTSIZE)
		return -1;
	
	/* make sure it's a valid block */
	if(block->number >= ((struct ide_info *) object->instance)->length)
		return -1;
	
	/* write it */
	ide_write(((struct ide_info *) object->instance)->disk, block->number, block->ddesc->data, 1);
	
	/* drop the hot potato */
	bdesc_drop(&block);
	
	return 0;
}

static int ide_pio_bd_sync(BD_t * object, bdesc_t * block)
{
	/* drop the hot potato */
	if (block)
		bdesc_drop(&block);
	else
		printf("ide_pio_bd_sync: hey, where's my hot potato?\n");
	return 0;
}

static int ide_pio_bd_destroy(BD_t * bd)
{
	free(bd->instance);
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * ide_pio_bd(uint32_t disk)
{
	struct ide_info * info;
	BD_t * bd = malloc(sizeof(*bd));
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
	
	info->controller = 0;
	info->disk = disk;
	info->length = ide_size(disk);
	
	return bd;
}
