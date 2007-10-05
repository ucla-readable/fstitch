/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_MODULES_LINUX_BD_H
#define __FSTITCH_MODULES_LINUX_BD_H

#include <fscore/bd.h>

BD_t * linux_bd(const char * linux_bdev_path, bool unsafe_disk_cache);

#endif /* __FSTITCH_MODULES_LINUX_BD_H */
