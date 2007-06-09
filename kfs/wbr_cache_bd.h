#ifndef __KUDOS_KFS_WBR_CACHE_BD_H
#define __KUDOS_KFS_WBR_CACHE_BD_H

#include <kfs/bd.h>

BD_t * wbr_cache_bd(BD_t * disk, uint32_t soft_dblocks, uint32_t soft_blocks);

#endif /* __KUDOS_KFS_WBR_CACHE_BD_H */
