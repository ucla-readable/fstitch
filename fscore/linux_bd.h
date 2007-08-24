#ifndef __FSTITCH_FSCORE_LINUX_BD_H
#define __FSTITCH_FSCORE_LINUX_BD_H

#include <fscore/bd.h>

BD_t * linux_bd(const char * linux_bdev_path, bool unsafe_disk_cache);

#endif /* __FSTITCH_FSCORE_LINUX_BD_H */
