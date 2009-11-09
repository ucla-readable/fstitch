/* This file is part of Featherstitch. Featherstitch is copyright 2005-2009 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#include <fscore/linux_bd_debug.h>

#define BLKSIZE 2048

static struct linux_bd_writes writes;

static struct block {
	int32_t nwrites;
	int32_t last_write; /* issue index into writes.writes */
	uint32_t read_checksum;
} blocks[MAXBLOCKNO];

static uint32_t completes_map[MAXWRITES]; /* complete index -> issue index */

static void load_log(const char * log_filename)
{
	int fd;
	ssize_t size;
	int32_t i;
	fd = open(log_filename, O_RDONLY);
	assert(fd != -1);
	size = read(fd, &writes, sizeof(writes));
	if (size != sizeof(writes))
	{
		fprintf(stderr, "%s(): only read %zd bytes when %zu were expected\n", __FUNCTION__, size, (size_t) sizeof(writes));
		exit(1);
	}
	if (writes.next > MAXWRITES)
	{
		fprintf(stderr, "debug log exceeded debug space, aborting\n");
		exit(1);
	}
	for (i = 0; i < writes.next; i++)
	{
		uint32_t blockno = writes.writes[i].blockno;
		blocks[blockno].nwrites++;
		blocks[blockno].last_write = i;
		assert(!completes_map[writes.writes[i].completed]);
		completes_map[writes.writes[i].completed] = i;
	}
	close(fd);
}

static void compare_checksums(const char * disk_filename)
{
	uint8_t buf[BLKSIZE];
	int fd;
	int i, r;

	fd = open(disk_filename, O_RDONLY);
	assert(fd != -1);
	for (i = 0; i < MAXBLOCKNO; i++)
	{
		if (!blocks[i].nwrites)
			continue;
		r = lseek(fd, i * 512, SEEK_SET);
		assert(r != -1);
		r = read(fd, buf, BLKSIZE);
		assert(r == BLKSIZE);
		blocks[i].read_checksum = block_checksum(buf, BLKSIZE);
	}
	close(fd);
}

int main(int argc, char ** argv)
{
	uint32_t blockno;

	if (argc != 3)
	{
		fprintf(stderr, "About: check linux_bd writes\n");
		fprintf(stderr, "Usage: %s <linux_bd_writes> <disk_image>\n", argv[0]);
		exit(1);
	}

	load_log(argv[1]);
	compare_checksums(argv[2]);

	for (blockno = 0; blockno < MAXBLOCKNO; blockno++)
	{
		struct linux_bd_write * last_write = &writes.writes[blocks[blockno].last_write];
		uint32_t index;
		int32_t blockwriteno = 0;
		int checksum_match = 0;
		int issue_mismatch = 0;

		if (!blocks[blockno].nwrites)
			continue;
		if (blocks[blockno].read_checksum == last_write->checksum)
			continue;

		printf("block %u differs. written %u times. %d writes inflight. checksums: 0x%x (kfstitchd), 0x%x (read).\n", blockno, blocks[blockno].nwrites, last_write->ninflight, last_write->checksum, blocks[blockno].read_checksum);

		printf("block %u previous write checksum matches: ", blockno);
		for (index = 0; index < MAXWRITES; index++)
		{
			struct linux_bd_write * write = &writes.writes[index];
			if (write->blockno != blockno)
				continue;
			blockwriteno++;
			if (blocks[blockno].read_checksum == write->checksum)
			{
				checksum_match = 1;
				printf("%d ", blockwriteno);
			}
			if (blockwriteno == blocks[blockno].nwrites)
				break;
		}
		if (checksum_match)
			printf("of its %u writes", blocks[blockno].nwrites);
		else
			printf("none");
		printf("\n");
		
		printf("block %u issue->complete ordering differences: ", blockno);
		for (index = 0; index < writes.next; index++)
			if (writes.writes[index].blockno == blockno)
				if (index != writes.writes[index].completed)
				{
					issue_mismatch = 1;
					printf("%u->%u ", index, writes.writes[index].completed);
				}
		if (issue_mismatch)
			printf("of %u total writes", writes.next);
		else
			printf("none");
		printf("\n");
	}

	return 0;
}
