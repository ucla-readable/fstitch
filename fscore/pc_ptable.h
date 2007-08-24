#ifndef __FSTITCH_FSCORE_PC_PTABLE_BD_H
#define __FSTITCH_FSCORE_PC_PTABLE_BD_H

#include <fscore/bd.h>

void * pc_ptable_init(BD_t * bd);
int pc_ptable_count(void * info);
uint8_t pc_ptable_type(void * info, int index);
BD_t * pc_ptable_bd(void * info, int index);
void pc_ptable_free(void * info);

#endif /* __FSTITCH_FSCORE_PC_PTABLE_BD_H */
