#ifndef __KUDOS_KFS_ELEVATOR_CACHE_BD_H
#define __KUDOS_KFS_ELEVATOR_CACHE_BD_H

#include <lib/types.h>
#include <kfs/bd.h>

BD_t * elevator_cache_bd(BD_t * disk, uint32_t blocks, uint32_t optimistic_count, uint32_t max_gap_size);

uint32_t elevator_cache_dirty_count(BD_t * bd);

#endif /* __KUDOS_KFS_ELEVATOR_CACHE_BD_H */
