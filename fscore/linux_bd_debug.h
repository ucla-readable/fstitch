/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_LINUX_BD_DEBUG_H
#define __FSTITCH_FSCORE_LINUX_BD_DEBUG_H

/* Acceptable constants for UFS and ext2 linux-2.6.15 untar */
#define MAXBLOCKNO 557056
#define MAXWRITES  327680

struct linux_bd_writes {
	int32_t next; /* next free index of writes array */
	struct linux_bd_write {
		uint32_t blockno;
		uint32_t checksum;
		uint32_t ninflight; /* number of inflight writes upon issue */
		int32_t completed; /* write completion index */
	} writes[MAXWRITES]; /* array of write issues */
};

/* Return the checksum of a block of data. Just a simple checksum function. */
static __inline uint32_t block_checksum(const uint8_t * data, uint16_t length) __attribute__((always_inline));
static __inline uint32_t block_checksum(const uint8_t * data, uint16_t length)
{
	uint32_t checksum = 0;
	uint16_t i;
	for (i = 0; i < length; i++)
		checksum += data[i];
	return checksum;
}

#endif
