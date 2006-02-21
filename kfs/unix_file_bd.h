#ifndef __KUDOS_KFS_UNIX_FILE_BD_H
#define __KUDOS_KFS_UNIX_FILE_BD_H

#if !defined(UNIXUSER)
#error requires unixuser
#endif

#include <kfs/bd.h>

BD_t * unix_file_bd(const char *fname, uint16_t blocksize);

#endif /* __KUDOS_KFS_UNIX_FILE_BD_H */
