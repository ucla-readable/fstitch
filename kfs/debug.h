#ifndef KUDOS_KFS_DEBUG_H
#define KUDOS_KFS_DEBUG_H

#include <lib/types.h>

#include <kfs/bdesc.h>

#define KFS_DEBUG 0

#if KFS_DEBUG

#include <kfs/debug_opcode.h>

/* KFS Debug IO systems */
#define KFS_DEBUG_IO_TCP 0
#define KFS_DEBUG_IO_STREAMS 1
#define KFS_DEBUG_IO_PROC 2

/* Select KFS Debug IO system */
#if defined(KUDOS)
# define KFS_DEBUG_IO KFS_DEBUG_IO_TCP
#elif defined(UNIXUSER)
/* streams (fast) or tcp (non-buffered so slow, but writes are immediate
 * and interactive commands are supported */
# define KFS_DEBUG_IO KFS_DEBUG_IO_STREAMS
#elif defined(__KERNEL__)
# define KFS_DEBUG_IO KFS_DEBUG_IO_PROC
#endif

#if KFS_DEBUG_IO == KFS_DEBUG_IO_TCP
# define KFS_DEBUG_HOST "127.0.0.1"
# define KFS_DEBUG_PORT 15166
#elif KFS_DEBUG_IO == KFS_DEBUG_IO_STREAMS
# define KFS_DEBUG_FILE "debug.log"
#elif KFS_DEBUG_IO == KFS_DEBUG_IO_PROC
# define DEBUG_PROC_FILENAME "kkfsd_debug"
# define DEBUG_PROC_SIZE (4 * 1024 * 1024)
#endif

#define KFS_DEBUG_MARK 0
#define KFS_DEBUG_DISABLE 1
#define KFS_DEBUG_ENABLE 2

#define KFS_DEBUG_INIT() kfs_debug_init()
#define KFS_DEBUG_SEND(module, opcode, ...) kfs_debug_send(module, opcode, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define KFS_DEBUG_COMMAND(command, module) kfs_debug_command(command, module, __FILE__, __LINE__, __FUNCTION__)

#define KFS_DEBUG_COUNT() kfs_debug_count()
#define KFS_DEBUG_DBWAIT(block) kfs_debug_dbwait(__FUNCTION__, block)

int kfs_debug_init(void);
int kfs_debug_send(uint16_t module, uint16_t opcode, const char * file, int line, const char * function, ...);
void kfs_debug_command(uint16_t command, uint16_t module, const char * file, int line, const char * function);

int kfs_debug_count(void);
void kfs_debug_dbwait(const char * function, bdesc_t * block);

#else /* KFS_DEBUG */

#define KFS_DEBUG_INIT() 0
#define KFS_DEBUG_SEND(module, opcode, ...) ((void) 0)
#define KFS_DEBUG_COMMAND(command, module) do {} while(0)

#define KFS_DEBUG_COUNT() 0
#define KFS_DEBUG_DBWAIT(block) do {} while(0)

#endif /* KFS_DEBUG */

#endif /* KUDOS_KFS_DEBUG_H */
