/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_MODULES_UNIX_FILE_BD_H
#define __FSTITCH_MODULES_UNIX_FILE_BD_H

#if !defined(UNIXUSER)
#error requires unixuser
#endif

#include <fscore/bd.h>

BD_t * unix_file_bd(const char *fname, uint16_t blocksize);

#endif /* __FSTITCH_MODULES_UNIX_FILE_BD_H */
