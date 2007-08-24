#ifndef FSTITCH_FSCORE_DEBUG_H
#define FSTITCH_FSCORE_DEBUG_H

#define FSTITCH_DEBUG 0

#if FSTITCH_DEBUG

#include <fscore/debug_opcode.h>

#ifdef __KERNEL__
#define DEBUG_PROC_FILENAME "kfstitchd_debug"
#define DEBUG_PROC_SIZE (4 * 1024 * 1024)
#define DEBUG_COUNT_FILENAME "kfstitchd_count"
#elif defined(UNIXUSER)
#define DEBUG_FILENAME "uufstitchd_debug"
#endif

#define FSTITCH_DEBUG_MARK 0
#define FSTITCH_DEBUG_DISABLE 1
#define FSTITCH_DEBUG_ENABLE 2

#define FSTITCH_DEBUG_INIT() fstitch_debug_init()
#define FSTITCH_DEBUG_SEND(module, opcode, ...) fstitch_debug_send(module, opcode, __FILE__, __LINE__, __FUNCTION__, __VA_ARGS__)
#define FSTITCH_DEBUG_COMMAND(command, module) fstitch_debug_command(command, module, __FILE__, __LINE__, __FUNCTION__)
#define FSTITCH_DEBUG_COUNT() fstitch_debug_count()

int fstitch_debug_init(void);
int fstitch_debug_send(uint16_t module, uint16_t opcode, const char * file, int line, const char * function, ...);
void fstitch_debug_command(uint16_t command, uint16_t module, const char * file, int line, const char * function);

int fstitch_debug_count(void);

#else /* FSTITCH_DEBUG */

#define FSTITCH_DEBUG_INIT() 0
#define FSTITCH_DEBUG_SEND(module, opcode, ...) ((void) 0)
#define FSTITCH_DEBUG_COMMAND(command, module) do {} while(0)
#define FSTITCH_DEBUG_COUNT() 0

#endif /* FSTITCH_DEBUG */

#endif /* FSTITCH_FSCORE_DEBUG_H */
