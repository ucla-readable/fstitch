#ifndef __KUDOS_KFS_DIRENT_H
#define __KUDOS_KFS_DIRENT_H

#include <inc/types.h>

#define DIRENT_MAXNAMELEN 128

struct dirent {
	uint32_t d_fileno;
	uint32_t d_filesize;
	uint16_t d_reclen;
	uint8_t d_type;
	uint8_t d_namelen;
	char d_name[DIRENT_MAXNAMELEN + 1];
};
typedef struct dirent dirent_t;

// int getdirentries(int fd, char * buf, int nbytes, long * basep);

#endif /* __KUDOS_KFS_DIRENT_H */
