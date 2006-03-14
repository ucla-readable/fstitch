#ifndef __KUDOS_KFS_KERNEL_SERVE_H
#define __KUDOS_KFS_KERNEL_SERVE_H

#include <linux/kernel.h>
#include <kfs/cfs.h>

int kernel_serve_add_mount(const char * path, CFS_t * cfs);

#define kfsd_add_mount(p, c) kernel_serve_add_mount(p, c)

int kernel_serve_init(spinlock_t * kfsd_lock);

#endif // !__KUDOS_KFS_KERNEL_SERVE_H
