#ifndef __KUDOS_KFS_WT_CACHE_BD_H
#define __KUDOS_KFS_WT_CACHE_BD_H

#include <inc/types.h>
#include <kfs/bd.h>

BD_t * wt_cache_bd(BD_t * disk, uint32_t blocks);

#endif /* __KUDOS_KFS_WT_CACHE_BD_H */
