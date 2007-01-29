/* See COPYRIGHT for copyright information. */

#ifndef KUDOS_INC_ERROR_H
#define KUDOS_INC_ERROR_H

#include <linux/errno.h>

/* Notice that E_UNSPECIFIED and E_EOF are the same value: neither really exists
 * on Linux. Alo, this value happens to be EPERM, "operation not permitted". */
#define E_UNSPECIFIED	1
#define E_EOF		1
#define E_INVAL		EINVAL
#define E_FAULT		EFAULT
#define E_NO_MEM	ENOMEM
#define	E_NO_DISK	ENOSPC
#define E_MAX_OPEN	ENFILE
#define E_NOT_FOUND	ENOENT
#define E_FILE_EXISTS	EEXIST
#define E_NOT_EXEC	ENOEXEC
#define E_NOT_DIR	ENOTDIR
#define E_NOT_EMPTY	ENOTEMPTY
#define E_BUSY		EBUSY
#define E_NO_DEV	ENODEV
#define E_PERM		EPERM
#define E_ACCES		EACCES
#define E_NO_SYS	ENOSYS
#define E_NAME_TOO_LONG	ENAMETOOLONG

/* not available on KudOS */
#define E_INTR		EINTR

#endif	// !KUDOS_INC_ERROR_H */
