#ifndef __KUDOS_KFS_BSD_PTABLE_BD_H
#define __KUDOS_KFS_BSD_PTABLE_BD_H

#include <lib/types.h>
#include <kfs/bd.h>

void * bsd_ptable_init(BD_t * bd);
int bsd_ptable_count(void * info);
uint8_t bsd_ptable_type(void * info, int index);
BD_t * bsd_ptable_bd(void * info, int index);
void bsd_ptable_free(void * info);

#endif /* __KUDOS_KFS_BSD_PTABLE_BD_H */
