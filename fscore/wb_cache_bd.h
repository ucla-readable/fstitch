#ifndef __FSTITCH_FSCORE_WB_CACHE_BD_H
#define __FSTITCH_FSCORE_WB_CACHE_BD_H

#include <fscore/bd.h>

BD_t * wb_cache_bd(BD_t * disk, uint32_t blocks);

uint32_t wb_cache_dirty_count(BD_t * bd);

#endif /* __FSTITCH_FSCORE_WB_CACHE_BD_H */
