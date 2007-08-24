#ifndef __FSTITCH_FSCORE_UNIX_FILE_BD_H
#define __FSTITCH_FSCORE_UNIX_FILE_BD_H

#if !defined(UNIXUSER)
#error requires unixuser
#endif

#include <fscore/bd.h>

BD_t * unix_file_bd(const char *fname, uint16_t blocksize);

#endif /* __FSTITCH_FSCORE_UNIX_FILE_BD_H */
