#ifndef KUDOS_KFS_DEBUG_H
#define KUDOS_KFS_DEBUG_H

#include <inc/types.h>

#define KFS_DEBUG 0

#if KFS_DEBUG

#define KFS_DEBUG_BINARY 1

#include <kfs/debug_opcode.h>

#define KFS_DEBUG_HOST "timbuktu.cs.ucla.edu"
#define KFS_DEBUG_PORT 15166

#define KFS_DEBUG_MARK 0
#define KFS_DEBUG_DISABLE 1
#define KFS_DEBUG_ENABLE 2

#define KFS_DEBUG_INIT() kfs_debug_init(KFS_DEBUG_HOST, KFS_DEBUG_PORT)
#define KFS_DEBUG_SEND(module, opcode, ...) kfs_debug_send(module, opcode, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define KFS_DEBUG_COMMAND(command, module) kfs_debug_command(command, module, __FILE__, __LINE__, __FUNCTION__)
#define KFS_DEBUG_NET_COMMAND() kfs_debug_net_command()

int kfs_debug_init(const char * host, uint16_t port);
int kfs_debug_send(uint16_t module, uint16_t opcode, const char * file, int line, const char * function, ...);
void kfs_debug_command(uint16_t command, uint16_t module, const char * file, int line, const char * function);
void kfs_debug_net_command(void);

#else /* KFS_DEBUG */

#define KFS_DEBUG_INIT() 0
#define KFS_DEBUG_SEND(module, opcode, ...) ((void) 0)
#define KFS_DEBUG_COMMAND(command, module) do {} while(0)
#define KFS_DEBUG_NET_COMMAND() do {} while(0)

#endif /* KFS_DEBUG */

#endif /* KUDOS_KFS_DEBUG_H */
