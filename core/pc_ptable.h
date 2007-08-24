#ifndef __KUDOS_KFS_PC_PTABLE_BD_H
#define __KUDOS_KFS_PC_PTABLE_BD_H

#include <kfs/bd.h>

void * pc_ptable_init(BD_t * bd);
int pc_ptable_count(void * info);
uint8_t pc_ptable_type(void * info, int index);
BD_t * pc_ptable_bd(void * info, int index);
void pc_ptable_free(void * info);

#endif /* __KUDOS_KFS_PC_PTABLE_BD_H */
