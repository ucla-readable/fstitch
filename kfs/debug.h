#ifndef KUDOS_KFS_DEBUG_H
#define KUDOS_KFS_DEBUG_H

#include <kfs/bdesc.h>

#define KFS_DEBUG 0

#if KFS_DEBUG

#include <kfs/debug_opcode.h>

#ifdef __KERNEL__
#define DEBUG_PROC_FILENAME "kkfsd_debug"
#define DEBUG_PROC_SIZE (4 * 1024 * 1024)
#define DEBUG_COUNT_FILENAME "kkfsd_count"
#elif defined(UNIXUSER)
#define DEBUG_FILENAME "uukfsd_debug"
#endif

#define KFS_DEBUG_MARK 0
#define KFS_DEBUG_DISABLE 1
#define KFS_DEBUG_ENABLE 2

#define KFS_DEBUG_INIT() kfs_debug_init()
#define KFS_DEBUG_SEND(module, opcode, ...) kfs_debug_send(module, opcode, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define KFS_DEBUG_COMMAND(command, module) kfs_debug_command(command, module, __FILE__, __LINE__, __FUNCTION__)
#define KFS_DEBUG_COUNT() kfs_debug_count()

int kfs_debug_init(void);
int kfs_debug_send(uint16_t module, uint16_t opcode, const char * file, int line, const char * function, ...);
void kfs_debug_command(uint16_t command, uint16_t module, const char * file, int line, const char * function);

int kfs_debug_count(void);

#else /* KFS_DEBUG */

#define KFS_DEBUG_INIT() 0
#define KFS_DEBUG_SEND(module, opcode, ...) ((void) 0)
#define KFS_DEBUG_COMMAND(command, module) do {} while(0)
#define KFS_DEBUG_COUNT() 0

#endif /* KFS_DEBUG */

#endif /* KUDOS_KFS_DEBUG_H */
