#ifndef KUDOS_LIB_MMU_H
#define KUDOS_LIB_MMU_H

#if defined(KUDOS)
#include <inc/mmu.h>

#elif defined(UNIXUSER)

#if defined(__linux__)
#include <asm/page.h>
#define PGSIZE PAGE_SIZE
#elif defined(__MACH__)
#include <sys/vm.h>
#define PGSIZE PAGE_SIZE
#else
#warning Assuming 4KB page size
#define PGSIZE 4096
#endif

#else
#error Unknown target system
#endif

#endif /* !KUDOS_LIB_MMU_H */
