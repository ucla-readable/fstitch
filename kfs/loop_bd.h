#ifndef __KUDOS_KFS_LOOP_BD_H
#define __KUDOS_KFS_LOOP_BD_H

#include <inc/types.h>
#include <kfs/lfs.h>
#include <kfs/bd.h>

BD_t * loop_bd(LFS_t * lfs, const char * file);

#endif /* __KUDOS_KFS_LOOP_BD_H */
