/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef _PARTITION_H_
#define _PARTITION_H_ 1

#define PTABLE_OFFSET           446
#define PTABLE_FSTITCH_TYPE       0xF8
#define PTABLE_DOS_EXT_TYPE     0x05
#define PTABLE_W95_EXT_TYPE     0x0F
#define PTABLE_LINUX_TYPE       0x83
#define PTABLE_LINUX_EXT_TYPE   0x85
#define PTABLE_EZDRIVE_TYPE	0x55
#define PTABLE_FREEBSD_TYPE     0xA5
#define PTABLE_OPENBSD_TYPE     0xA6
#define PTABLE_NETBSD_TYPE      0xA9

#define PTABLE_MAGIC		((uint8_t *) "\x55\xAA")
#define PTABLE_MAGIC_OFFSET	510

struct pc_ptable {
	uint8_t boot;
	uint8_t chs_begin[3];
	uint8_t type;
	uint8_t chs_end[3];
	uint32_t lba_start;
	uint32_t lba_length;
};

#define CHS_HEAD(chs) ((chs)[0])
#define CHS_SECTOR(chs) ((chs)[1] & 0x3F)
#define CHS_CYLINDER(chs) ((((uint16_t) (chs)[1] & 0xC0) << 2) + (chs)[2])

#endif
