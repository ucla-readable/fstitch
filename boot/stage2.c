#include <inc/x86.h>
#include <inc/elf.h>

#include <inc/pmap.h>
#include <inc/fs.h>
#include <inc/isareg.h>
#include <lib/partition.h>
#include <inc/multiboot.h>

#include <kern/kclock.h>

#define SECTSIZE 512
#define BLKSECTS (BLKSIZE / SECTSIZE)

#define SCRATCH ((void *) 0x10000)
#define ELF ((struct Elf *) 0x10000)

/* for reading the kernel from the filesystem */
#define SUPER ((struct Super *) 0x11000)	/* filesystem superblock */
#define D_IND ((uint32_t *) 0x12000)		/* / indirect block list */
#define D_DATA ((struct File *) 0x13000)	/* / directory data */
#define K_IND ((uint32_t *) 0x14000)		/* /kernel indirect block list */

unsigned mc146818_read(void * sc, unsigned reg)
{
	outb(IO_RTC, reg);
	return inb(IO_RTC + 1);
}

static int nvram_read(int r)
{
	return mc146818_read(NULL, r) | (mc146818_read(NULL, r + 1) << 8);
}

static void notbusy(void)
{
	/* wait for disk not busy */
	while((inb(0x1F7) & 0xC0) != 0x40);
}

static void readsect(void * dst, uint32_t count, uint32_t offset)
{
	notbusy();

	outb(0x1F2, count);
	outb(0x1F3, offset);
	outb(0x1F4, offset >> 8);
	outb(0x1F5, offset >> 16);
	outb(0x1F6, (offset >> 24) | 0xE0);
	/* cmd 0x20 - read sectors */
	outb(0x1F7, 0x20);

	while(count--)
	{
		notbusy();
		
		insl(0x1F0, dst, SECTSIZE / 4);
		dst += SECTSIZE;
	}
}

static void writesect(void * src, uint32_t count, uint32_t offset)
{
	notbusy();

	outb(0x1F2, count);
	outb(0x1F3, offset);
	outb(0x1F4, offset >> 8);
	outb(0x1F5, offset >> 16);
	outb(0x1F6, (offset >> 24) | 0xE0);
	/* cmd 0x30 - write sectors */
	outb(0x1F7, 0x30);

	while(count--)
	{
		notbusy();
	
		outsl(0x1F0, src, SECTSIZE / 4);
		src += SECTSIZE;
	}
}

typedef void (*kreader_t)(void *, uint32_t, uint32_t, uint32_t);

/* read 'count' bytes at 'offset' from kernel into virtual address 'va' */
static void readseg(uint32_t va, uint32_t count, uint32_t offset, uint32_t partition, kreader_t read, uint32_t index)
{
	uint32_t i;

	va &= 0xFFFFFF;

	/* round down to sector boundary; offset will round later */
	i = va % BLKSIZE;
	count += i;
	va -= i;

	/* translate from bytes to sectors */
	offset /= BLKSIZE;
	count = (count + BLKSIZE - 1) / BLKSIZE;

	/* If this is too slow, we could read lots of sectors at a time. We'd
	 * write more to memory than asked, but it doesn't matter -- we load in
	 * increasing order. */
	for(i = 0; i < count; i++)
	{
		read((uint8_t*) va, offset + i, partition, index);
		va += BLKSIZE;
	}
}

/* find the first KudOS partition, or return 0 if none found */
static uint32_t find_kudos(uint32_t table_offset, uint32_t ext_offset)
{
	int i;
	struct pc_ptable * ptable = (struct pc_ptable *) (SCRATCH + PTABLE_OFFSET);
	
	readsect(SCRATCH, 1, table_offset);
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

static void raw_read_kernel(void * dst, uint32_t offset, uint32_t partition, uint32_t index)
{
	/* kernel starts at disk sector BLKSECTS relative to beginning of partition */
	readsect(dst, BLKSECTS, partition + BLKSECTS * (1 + offset));
}

static int strcmp(const unsigned char * s1, const unsigned char * s2)
{
	while(*s1 && *s1 == *s2)
		s1++, s2++;
	return *s1 - *s2;
}

static void josfs_setup_file(struct File * file, uint32_t * indirect, uint32_t partition)
{
	if(file->f_indirect)
	{
		readsect(indirect, BLKSECTS, partition + BLKSECTS * file->f_indirect);
	}
	else
		for(partition = 0; partition != BLKSIZE / sizeof(*indirect); partition++)
			indirect[partition]++;
}

static int josfs_read_file(struct File * file, uint32_t * indirect, uint32_t block, void * dst, uint32_t partition)
{
	if(block < NDIRECT)
		block = file->f_direct[block];
	else if(block < NINDIRECT)
		block = indirect[block];
	else
		return -1;
	if(!block)
		return -1;
	readsect(dst, BLKSECTS, partition + BLKSECTS * block);
	return 0;
}

static void josfs_read_kernel(void * dst, uint32_t offset, uint32_t partition, uint32_t index)
{
	josfs_read_file(&D_DATA[index], K_IND, offset, dst, partition);
}

void stage2(int extmem_kbytes)
{
	uint32_t entry, i;
	struct Proghdr* ph;
	
	/* put the multiboot structure right after ourselves */
	extern char end[];
	struct multiboot * mb_info = (struct multiboot *) end;
	
	kreader_t read = raw_read_kernel;
	uint32_t partition, index = 0;
	
	/* read the first sector, which contains the partition table */
	readsect(SCRATCH, 1, 0);
	
	/* hack for remote KudOS testing: set Linux bootable instead of KudOS for next boot */
	{
		struct pc_ptable * ptable = (struct pc_ptable *) (SCRATCH + PTABLE_OFFSET);
		int ext2 = -1;
		int kudos = -1;
		
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
			if(!ptable[ext2].boot && ptable[kudos].boot)
			{
				ptable[ext2].boot ^= ptable[kudos].boot;
				ptable[kudos].boot ^= ptable[ext2].boot;
				ptable[ext2].boot ^= ptable[kudos].boot;
				writesect(SCRATCH, 1, 0);
			}
	}
	
	partition = find_kudos(0, 0);
	
	readsect(SUPER, BLKSECTS, partition + BLKSECTS);
	/* if this looks like a filesystem, try using it (otherwise, use old method) */
	if(SUPER->s_magic == FS_MAGIC)
	{
		uint32_t block;
		int name;
		const char * kernel_names[] = {"kernel.new", "kernel", "kernel.old"};
		
		/* set up filesystem data */
		josfs_setup_file(&SUPER->s_root, D_IND, partition);
		
		/* search for a suitable kernel */
		for(name = 0; name != sizeof(kernel_names) / sizeof(kernel_names[0]); name++)
			for(block = 0; !josfs_read_file(&SUPER->s_root, D_IND, block, D_DATA, partition); block++)
				for(index = 0; index != BLKFILES; index++)
					if(!strcmp(kernel_names[name], &D_DATA[index].f_name[0]))
					{
						/* if we found it, use it */
						josfs_setup_file(&D_DATA[index], K_IND, partition);
						read = josfs_read_kernel;
						goto out;
					}
		/* this is a filesystem, because we had FS_MAGIC, but we did not
		 * find a kernel... the old method will fail, because it will
		 * have FS_MAGIC instead of ELF_MAGIC - so we just return now */
		return;
	}
	out:
	
	// read 1st page of kernel - note read(), not readsect() or readseg()
	read(ELF, 0, partition, index);
	
	if(ELF->e_magic != ELF_MAGIC)
		return;
	
	// look at ELF header - ignores ph flags
	entry = ELF->e_entry;
	ph = (struct Proghdr*) (SCRATCH + ELF->e_phoff);
	for (i = 0; i < ELF->e_phnum; i++, ph++)
		readseg(ph->p_va, ph->p_memsz, ph->p_offset, partition, read, index);
	
	entry &= 0xFFFFFF;
	
	if(extmem_kbytes)
	{
		/* only memory fields are valid */
		mb_info->flags = MULTIBOOT_FLAG_MEMORY;
		mb_info->mem_lower = nvram_read(NVRAM_BASELO);
		mb_info->mem_upper = extmem_kbytes;
		
		/* same as below, but force multiboot information in eax and ebx */
		__asm__("jmp *%%ecx" : : "a" (MULTIBOOT_EAX_MAGIC), "b" (mb_info), "c" (entry));
	}
	else
		((void(*)(void)) entry)();
	
	/* DOES NOT RETURN */
}
