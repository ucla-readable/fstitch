/* See COPYRIGHT for copyright information. */

#ifndef KUDOS_INC_ERROR_H
#define KUDOS_INC_ERROR_H

#if defined(KUDOS)

// Error codes -- keep in sync with list in lib/printfmt.c.
#define E_UNSPECIFIED	1	// Unspecified or unknown problem
#define E_BAD_ENV       2       // Environment doesn't exist or otherwise
				// cannot be used in requested action
#define E_INVAL		3	// Invalid parameter
#define E_FAULT		4	// Bad memory passed to kernel
#define E_NO_MEM	5	// Request failed due to memory shortage
#define E_NO_FREE_ENV   6       // Attempt to create a new environment beyond
				// the maximum allowed
#define E_IPC_NOT_RECV  7	// Attempt to send to env that is not recving
#define E_IPC_FAILED_CAP 8	// Sent-to-env responded that the sent capability is bad

#define E_EOF		9	// Unexpected end of file
#define	E_NO_DISK	10	// No free space left on disk
#define E_MAX_OPEN	11	// Too many files are open
#define E_NOT_FOUND	12 	// File or block not found
#define E_NAME_TOO_LONG	13	// Bad path
#define E_BAD_PATH	14	// Bad path
#define E_FILE_EXISTS	15	// File already exists
#define E_NOT_EXEC	16	// File not a valid executable
#define E_NOT_DIR	17	// File is not a directory
#define E_NOT_EMPTY	18	// Directory not empty

#define E_BUSY		19	// Device is busy
#define E_NO_DEV	20	// No such device
#define E_PERM		21	// Operation not permitted
#define E_ACCES		22	// Permission denied
#define E_TIMEOUT	23	// Timed out

#define E_BAD_SYM	24	// Elf symbol doesn't exist
#define E_SYMTBL	25	// No elf symbol/symbol string table loaded in kernel

#define E_NET_ABRT	26	// Net connection aborted
#define E_NET_RST	27	// Net connection reset
#define E_NET_CONN	28	// No connection
#define E_NET_USE	29	// Net address in use
#define E_NET_IF	30	// Net low-level netif error

#define MAXERROR	30

#else

#if defined(UNIXUSER)
#include <errno.h>
#elif defined(__KERNEL__)
#include <linux/errno.h>
#else
#error Unknown target
#endif

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

#endif

#if defined(UNIXUSER)
#define E_NET_ABRT	ECONNABORTED // Net connection aborted
#define E_NET_USE	EADDRINUSE // Net address in use
#endif

#endif	// !KUDOS_INC_ERROR_H */
