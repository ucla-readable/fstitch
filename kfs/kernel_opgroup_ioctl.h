#ifndef __KUDOS_KFS_KERNEL_OPGROUP_IOCTL_H
#define __KUDOS_KFS_KERNEL_OPGROUP_IOCTL_H

#define OPGROUP_DEVICE "opgroup"
#define OPGROUP_MAJOR 223

#define OPGROUP_IOCTL_CREATE     1
#define OPGROUP_IOCTL_SYNC       2
#define OPGROUP_IOCTL_ADD_DEPEND 3
#define OPGROUP_IOCTL_ENGAGE     4
#define OPGROUP_IOCTL_DISENGAGE  5
#define OPGROUP_IOCTL_RELEASE    6
#define OPGROUP_IOCTL_ABANDON    7
#define OPGROUP_IOCTL_LABEL      8

struct opgroup_ioctl_cmd {
	int opgroup_a;
	int opgroup_b;
	int flags;
	const char * str;
};
typedef struct opgroup_ioctl_cmd opgroup_ioctl_cmd_t;

#endif /* __KUDOS_KFS_KERNEL_OPGROUP_IOCTL_H */
