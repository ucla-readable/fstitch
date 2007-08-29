/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_KERNEL_PATCHGROUP_IOCTL_H
#define __FSTITCH_FSCORE_KERNEL_PATCHGROUP_IOCTL_H

#define PATCHGROUP_DEVICE "patchgroup"
#define PATCHGROUP_MAJOR 223

#define PATCHGROUP_IOCTL_CREATE     1
#define PATCHGROUP_IOCTL_SYNC       2
#define PATCHGROUP_IOCTL_ADD_DEPEND 3
#define PATCHGROUP_IOCTL_ENGAGE     4
#define PATCHGROUP_IOCTL_DISENGAGE  5
#define PATCHGROUP_IOCTL_RELEASE    6
#define PATCHGROUP_IOCTL_ABANDON    7
#define PATCHGROUP_IOCTL_LABEL      8

struct patchgroup_ioctl_cmd {
	int patchgroup_a;
	int patchgroup_b;
	int flags;
	const char * str;
};
typedef struct patchgroup_ioctl_cmd patchgroup_ioctl_cmd_t;

#endif /* __FSTITCH_FSCORE_KERNEL_PATCHGROUP_IOCTL_H */
