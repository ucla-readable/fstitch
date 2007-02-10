#ifndef __KUDOS_KFS_WB2_CACHE_BD_H
#define __KUDOS_KFS_WB2_CACHE_BD_H

#include <lib/types.h>
#include <kfs/bd.h>

BD_t * wb2_cache_bd(BD_t * disk, uint32_t soft_dblocks, uint32_t soft_blocks);

#endif /* __KUDOS_KFS_WB2_CACHE_BD_H */
