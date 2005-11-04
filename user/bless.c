#include <inc/lib.h>
#include <inc/x86.h>
#include <lib/partition.h>

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

void umain(void)
{
	uint8_t buffer[SECTSIZE];
	struct pc_ptable * ptable = (struct pc_ptable *) &buffer[PTABLE_OFFSET];
	
	int i;
	int ext2 = -1;
	int kudos = -1;
	
	sys_grant_io(0);
	
	/* read the first sector, which contains the partition table */
	ide_read(0, 0, buffer, 1);
	
	for(i = 0; i != 4; i++)
	{
		/* 0x83 is Linux ext2/3 */
		if(ptable[i].type == 0x83 && ext2 == -1)
			ext2 = i;
		if(ptable[i].type == PTABLE_KUDOS_TYPE && kudos == -1)
			kudos = i;
	}
	
	/* only swap the flags if KudOS is bootable and Linux is not */
	if(ext2 != -1 && kudos != -1)
		if(ptable[ext2].boot && !ptable[kudos].boot)
		{
			ptable[ext2].boot ^= ptable[kudos].boot;
			ptable[kudos].boot ^= ptable[ext2].boot;
			ptable[ext2].boot ^= ptable[kudos].boot;
			printf("Blessing KudOS partition.\n");
			ide_write(0, 0, buffer, 1);
		}
}
