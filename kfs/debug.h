#ifndef KUDOS_KFS_DEBUG_H
#define KUDOS_KFS_DEBUG_H

#include <inc/types.h>

#define KFS_DEBUG 0

#if KFS_DEBUG

#include <kfs/debug_opcode.h>

#define KFS_DEBUG_HOST "timbuktu.cs.ucla.edu"
#define KFS_DEBUG_PORT 15166

#define KFS_DEBUG_INIT() kfs_debug_init(KFS_DEBUG_HOST, KFS_DEBUG_PORT)
#define KFS_DEBUG_SEND(module, opcode, ...) kfs_debug_send(__FILE__, __LINE__, __FUNCTION__, module, opcode, __VA_ARGS__)

int kfs_debug_init(const char * host, uint16_t port);
int kfs_debug_send(const char * file, int line, const char * function, uint32_t module, uint32_t opcode, ...);

#else /* KFS_DEBUG */

#define KFS_DEBUG_INIT() 0
#define KFS_DEBUG_SEND(module, opcode, ...) ((void) 0)

#endif /* KFS_DEBUG */

#endif /* KUDOS_KFS_DEBUG_H */
