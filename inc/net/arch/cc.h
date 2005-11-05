#ifndef KUDOS_INC_LWIP_CC_H
#define KUDOS_INC_LWIP_CC_H

// Architecture environment, some compiler specific, some
// environment specific (probably should move env stuff 
// to sys_arch.h.)

// Define platform endianness

#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif /* BYTE_ORDER */


// Typedefs for the types used by lwip -
//   u8_t, s8_t, u16_t, s16_t, u32_t, s32_t, mem_ptr_t

#include <inc/types.h>
typedef unsigned char u_char;

typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;

typedef uint16_t  u_short;
typedef uint32_t  u_int;

typedef uintptr_t mem_ptr_t;

#include <inc/mmu.h>
struct sio_dev {
	u16_t com_addr;
	u8_t  buf_container[2*PGSIZE];
	u8_t *buf;
	u8_t  sioread;
};
#define __sio_fd_t_defined
typedef struct sio_dev*  sio_fd_t;

#define LWIP_DEBUG

// Compiler hints for packing lwip's structures -
//   PACK_STRUCT_FIELD(x)
//   PACK_STRUCT_STRUCT
//   PACK_STRUCT_BEGIN
//   PACK_STRUCT_END

// From the unix port:
#define PACK_STRUCT_FIELD(x) x __attribute__((packed))
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END


// Platform specific diagnostic output -
//   LWIP_PLATFORM_DIAG(x)    - non-fatal, print a message.
//   LWIP_PLATFORM_ASSERT(x)  - fatal, print message and abandon execution.

#include <inc/assert.h>
#include <inc/lib.h>
#define LWIP_PLATFORM_DIAG(x) do {printf x ;} while (0)
//#define LWIP_PLATFORM_ASSERT(x) do {panic("lwIP assert at %s:%d: %s", __FILE__, __LINE__, x);} while (0)
#define LWIP_PLATFORM_ASSERT(x) do {kdprintf(STDERR_FILENO, "ASSERT (ignoring), %s:%d: %s\n", __FILE__, __LINE__, x);} while (0)


// "lightweight" synchronization mechanisms -
//   SYS_ARCH_DECL_PROTECT(x) - declare a protection state variable.
//   SYS_ARCH_PROTECT(x)      - enter protection mode.
//   SYS_ARCH_UNPROTECT(x)    - leave protection mode.

// TODO: Do we need to implement these? Perhaps not since we don't have threading?
#define SYS_ARCH_DECL_PROTECT(x)
#define SYS_ARCH_PROTECT(x)
#define SYS_ARCH_UNPROTECT(x)


// If the compiler does not provide memset() this file must include a
// definition of it, or include a file which defines it.

#include <inc/string.h>


// This file must either include a system-local <errno.h> which defines
// the standard *nix error codes, or it should #define LWIP_PROVIDE_ERRNO
// to make lwip/arch.h define the codes which are used throughout.

#define LWIP_PROVIDE_ERRNO

#include "arch/sys_arch.h"

#endif /* !KUDOS_INC_LWIP_CC_H */
