/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_DIRENT_H
#define __FSTITCH_FSCORE_DIRENT_H

#include <fscore/inode.h>

#define DIRENT_MAXNAMELEN 255

//FIXME: d_filesize does not seem to be used in Unix_User or Kernel, however its
//existence does result in an extra call to get_inode in get_dirent just to set its
//value.  This does have an impact on read performance.  KudOS does use this feature
//for ls.c however. Using a casual test with kbench and ufs, tar and rm speeds improved
//by roughly half a second when d_filesize was ignored.
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

#endif /* __FSTITCH_FSCORE_DIRENT_H */
