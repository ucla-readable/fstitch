/* This file is part of Featherstitch. Featherstitch is copyright 2005-2008 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_LIB_PATCHGROUP_TRACE_H
#define __FSTITCH_LIB_PATCHGROUP_TRACE_H

#include <sys/types.h>

#include <fscore/patchgroup.h>
#include <fscore/kernel_patchgroup_ioctl.h>

#define PGT_MAGIC 0x5BB3BD6D
#define PGT_VERSION 0

struct pgt_header {
	unsigned int magic;
	unsigned int version;
} __attribute__((packed));

struct pgt_all {
	int type;
	pid_t pid;
	time_t time;
} __attribute__((packed));

struct pgt_create {
	/* set type to PATCHGROUP_IOCTL_CREATE */
	struct pgt_all all;
	patchgroup_id_t id;
} __attribute__((packed));

struct pgt_add_depend {
	/* set type to PATCHGROUP_IOCTL_ADD_DEPEND */
	struct pgt_all all;
	patchgroup_id_t after;
	patchgroup_id_t before;
} __attribute__((packed));

struct pgt_release {
	/* set type to PATCHGROUP_IOCTL_RELEASE */
	struct pgt_all all;
	patchgroup_id_t id;
} __attribute__((packed));

struct pgt_abandon {
	/* set type to PATCHGROUP_IOCTL_ABANDON */
	struct pgt_all all;
	patchgroup_id_t id;
} __attribute__((packed));

struct pgt_label {
	/* set type to -1 */
	struct pgt_all all;
	patchgroup_id_t id;
	int label_len;
	/* not null-terminated */
	char label[0];
} __attribute__((packed));

#endif /* __FSTITCH_LIB_PATCHGROUP_TRACE_H */
