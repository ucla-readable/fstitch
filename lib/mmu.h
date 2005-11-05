#ifndef KUDOS_LIB_MMU_H
#define KUDOS_LIB_MMU_H

#if defined(KUDOS)
#include <inc/mmu.h>
#else
#warning Assuming 4KB page size
#define PGSIZE 4096
#endif

#endif /* !KUDOS_LIB_MMU_H */
