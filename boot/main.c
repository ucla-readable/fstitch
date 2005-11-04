#include <inc/x86.h>
#include <inc/elf.h>

#include <inc/pmap.h>
#include <inc/fs.h>
#include <lib/partition.h>

#define SECTSIZE 512
#define BLKSECTS (BLKSIZE / SECTSIZE)

/* this is a macro to force it to be inline - it is so small that this saves space */
#define notbusy() while((inb(0x1F7) & 0xC0) != 0x40)

#define outb_small(port, data) __asm__("movb %0,%%al\n\toutb %%al,%1" : : "n" (data), "n" (port) : "eax");

void
readsect(void * dst, uint32_t offset)
{
	notbusy();

	outb(0x1F2, 1);		// count = 1
	outb(0x1F3, offset);
	outb(0x1F4, offset >> 8);
	outb(0x1F5, offset >> 16);
	outb(0x1F6, (offset >> 24) | 0xE0);
	outb(0x1F7, 0x20);	// cmd 0x20 - read sectors

	notbusy();
	
	insl(0x1F0, dst, SECTSIZE/4);
}

/* find the first KudOS partition, or return 0 if none found */
uint32_t
find_kudos(uint32_t table_offset, uint32_t ext_offset)
{
	int i;
	struct pc_ptable * ptable = (struct pc_ptable *) (0x7E00 + PTABLE_OFFSET);
	
	readsect((void *) 0x7E00, table_offset);
	for(i = 0; i != 4; i++)
		if(ptable[i].type == PTABLE_KUDOS_TYPE)
			return table_offset + ptable[i].lba_start;
	for(i = 0; i != 4; i++)
		if(ptable[i].type == PTABLE_DOS_EXT_TYPE ||
		   ptable[i].type == PTABLE_W95_EXT_TYPE ||
		   ptable[i].type == PTABLE_LINUX_EXT_TYPE)
			return find_kudos(ext_offset + ptable[i].lba_start, ext_offset ? ext_offset : ptable[i].lba_start);
	return 0;
}

void
cmain(int extmem_kbytes)
{
	int i, offset = find_kudos(0, 0) + 1;
	
	for(i = 0; i != BLKSECTS - 1; i++)
		readsect((void *) (0x7E00 + SECTSIZE * i), offset++);
	
	/* call stage 2 */
	((void(*)(int)) 0x7E00)(extmem_kbytes);
	
	/* error, reboot */
	outb_small(0x92, 0x3);
	
	/* these would cause the bochs x86 emulator to go into debug mode */
	//outw(0x8A00, 0x8A00);
	//outw(0x8A00, 0x8AE0);
	
	for(;;);
}
