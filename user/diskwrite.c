#include <fs/fs.h>
#include <inc/lib.h>

extern uint32_t find_kudos(uint8_t * buffer, uint32_t table_offset, uint32_t ext_offset);

// fs/fs.c:fs_init with a few small changes
static void
find_fs(int *pdiskno, uint32_t *ppart_length, uint32_t *fs_offset)
{
	uint8_t buffer[512];
	extern uint32_t part_length;
	extern int diskno;
	
	static_assert(sizeof(struct File) == 256);

	for(*pdiskno = 0; *pdiskno < 2; (*pdiskno)++)
	{
		printf("Trying disk %d...\n", *pdiskno);
		diskno = *pdiskno;

		/* "no partition, allow whole disk" */
		part_length = 0;
		
		/* find the partition */
		*fs_offset = find_kudos(buffer, 0, 0);
		*ppart_length = part_length;
		
		printf("Disk offset: %d\n", *fs_offset);
		
		if(1) //(!read_super())
			break;
	}
	if(*pdiskno == 2)
		panic("no valid filesystems found");
	printf("using filesystem on disk %d\n", *pdiskno);

	//check_write_block();
	//read_bitmap();
}


uint8_t blk[BLKSIZE];

void
umain(int argc, char **argv)
{
	int disk_no;
	uint32_t fs_offset;
	uint32_t partition_length;

	int tot_n, n;
	uint32_t blockno = 0;
	uint32_t sector;
	int r;

	if (argc != 1)
	{
		printf("Usage: %s\n", argv[0]);
		printf("About: write the data from stdin to the partition/disk containing the current fileystem.\n");
		printf("Example: diskwrite < small_fs.img\n");
		printf("         get 192.168.0.2/fs.img -q | diskwrite\n");
		exit();
	}

	if ((r = sys_grant_io(0)) < 0)
	{
		fprintf(STDERR_FILENO, "sys_grant_io: %e\n", r);
		exit();
	}

	find_fs(&disk_no, &partition_length, &fs_offset);

	// Wait a bit before starting (and stopping the fs) in case we were
	// started "diskwrite < fs.img", so that the shell has a chance to
	// close its fds:
	if ((r = sleep(50)) < 0)
		fprintf(STDERR_FILENO, "sleep: %e\n");

	for (;; blockno++)
	{
		// zero the blk in case this is the last block and it is partial
		memset(blk, 0, sizeof(blk));
		tot_n = 0;
		do
		{
			n = read(STDIN_FILENO, blk+tot_n, sizeof(blk)-tot_n);
			tot_n += n;
		} while (tot_n < sizeof(blk) && n != 0);

		if (tot_n == 0)
			break;

		if (tot_n != sizeof(blk))
			printf("Read %d bytes for blockno %d, not %d as expected\n", tot_n, blockno, sizeof(blk));

		if (blockno == 0)
		{
			// In case the user starts diskwrite and feeds a few characters in
			// (typing or a web 404, for example), exit
			if (tot_n != sizeof(blk))
			{
				fprintf(STDERR_FILENO, "Input had less than one block of data, exiting without modifying the disk\n");
				break;
			}

			// Shutdown the filesystem only after we've read the first block
			// to help ensure whomever is feeding diskwrite data is loaded
			if ((r = fs_shutdown()) < 0)
			{
				fprintf(STDERR_FILENO, "Unable to shutdown fs, exiting\n");
				break;
			}
		}

		sector = blockno * BLKSECTS;
		if (sector >= partition_length && partition_length)
			panic("writing sector 0x%08x past end of partition", sector);
		ide_write(disk_no, sector + fs_offset, blk, BLKSECTS);
	}

	if (blockno > 0)
		blockno--; // wrote blockno-1 blocks
	printf("Wrote %d blocks\n", blockno);

	if (blockno > 0)
	{
		const char reboot_msg[] = "** Rebooting in 2 seconds **\n";
		printf(reboot_msg);
		if (!iscons(STDOUT_FILENO))
			printf_c(reboot_msg);

		if ((r = sleep(2*100)) < 0)
			fprintf(STDERR_FILENO, "sleep: %e\n", r);
		sys_reboot();
	}
	// This will fail, why?:
	//if ((r = spawnl("fs", "fs", (const char **) 0)) < 0)
	//	fprintf(STDERR_FILENO, "spawn fs: %e\n", r);
}
