#ifndef __KUDOS_KFS_LINUX_BD_H
#define __KUDOS_KFS_LINUX_BD_H

#include <kfs/bd.h>

BD_t * linux_bd(const char * linux_bdev_path, bool unsafe_disk_cache);

#endif /* __KUDOS_KFS_LINUX_BD_H */
