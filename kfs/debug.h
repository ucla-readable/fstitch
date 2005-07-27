#ifndef KUDOS_KFS_DEBUG_H
#define KUDOS_KFS_DEBUG_H

#include <inc/types.h>

#define KFS_DEBUG 0

#if KFS_DEBUG

#define KFS_DEBUG_BINARY 1

#include <kfs/debug_opcode.h>

#define KFS_DEBUG_HOST "timbuktu.cs.ucla.edu"
#define KFS_DEBUG_PORT 15166

#define KFS_DEBUG_INIT() kfs_debug_init(KFS_DEBUG_HOST, KFS_DEBUG_PORT)
#define KFS_DEBUG_SEND(module, opcode, ...) kfs_debug_send(module, opcode, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define KFS_DEBUG_IGNORE(module) kfs_debug_ignore(module, 1)

int kfs_debug_init(const char * host, uint16_t port);
int kfs_debug_send(uint16_t module, uint16_t opcode, const char * file, int line, const char * function, ...);
int kfs_debug_ignore(uint16_t module, bool ignore);

#else /* KFS_DEBUG */

#define KFS_DEBUG_INIT() 0
#define KFS_DEBUG_SEND(module, opcode, ...) ((void) 0)
#define KFS_DEBUG_IGNORE(module) ((void) 0)

#endif /* KFS_DEBUG */

#endif /* KUDOS_KFS_DEBUG_H */
