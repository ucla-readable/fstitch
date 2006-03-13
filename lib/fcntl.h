#ifndef KUDOS_LIB_FCNTL_H
#define KUDOS_LIB_FCNTL_H

#if defined(KUDOS)
/* File open modes */
#define	O_RDONLY	0x0000		/* open for reading only */
#define	O_WRONLY	0x0001		/* open for writing only */
#define	O_RDWR		0x0002		/* open for reading and writing */
#define	O_ACCMODE	0x0003		/* mask for above modes */

#define	O_CREAT		0x0100		/* create if nonexistent */
#define	O_TRUNC		0x0200		/* truncate to zero length */
#define	O_EXCL		0x0400		/* error if already exists */
#define O_MKDIR		0x0800		/* create directory, not regular file */

#elif defined(UNIXUSER)
#include <fcntl.h>

#elif defined(__KERNEL__)
#include <linux/fcntl.h>

#else
#error Unknown target system
#endif

#endif /* !KUDOS_LIB_FCNTL_H */
