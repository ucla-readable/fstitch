/* See COPYRIGHT for copyright information. */

#ifndef KUDOS_INC_ERROR_H
#define KUDOS_INC_ERROR_H

// Error codes -- keep in sync with list in lib/printfmt.c.
#define E_UNSPECIFIED	1	// Unspecified or unknown problem
#define E_BAD_ENV       2       // Environment doesn't exist or otherwise
				// cannot be used in requested action
#define E_INVAL		3	// Invalid parameter
#define E_NO_MEM	4	// Request failed due to memory shortage
#define E_NO_FREE_ENV   5       // Attempt to create a new environment beyond
				// the maximum allowed
#define E_IPC_NOT_RECV  6	// Attempt to send to env that is not recving
#define E_IPC_FAILED_CAP 7	// Sent-to-env responded that the sent capability is bad

#define E_EOF		8	// Unexpected end of file
#define	E_NO_DISK	9	// No free space left on disk
#define E_MAX_OPEN	10	// Too many files are open
#define E_NOT_FOUND	11 	// File or block not found
#define E_BAD_PATH	12	// Bad path
#define E_FILE_EXISTS	13	// File already exists
#define E_NOT_EXEC	14	// File not a valid executable
#define E_NOT_DIR	15	// File is not a directory

#define E_BUSY		16	// Device is busy
#define E_NO_DEV	17	// No such device
#define E_PERM		18	// Permission denied
#define E_TIMEOUT	19	// Timed out

#define E_BAD_SYM 20 // Elf symbol doesn't exist
#define E_SYMTBL  21 // No elf symbol/symbol string table loaded in kernel

#define E_NET_ABRT 22 // Net connection aborted
#define E_NET_RST  23 // Net connection reset
#define E_NET_CONN 24 // No connection
#define E_NET_USE  25 // Net address in use
#define E_NET_IF   26 // Net low-level netif error

#define MAXERROR	26

#endif	// !KUDOS_INC_ERROR_H */
