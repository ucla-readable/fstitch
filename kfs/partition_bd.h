#ifndef __KUDOS_KFS_PARTITION_BD_H
#define __KUDOS_KFS_PARTITION_BD_H

#include <inc/types.h>
#include <kfs/bd.h>

BD_t * partition_bd(BD_t * disk, uint32_t start, uint32_t length);

#endif /* __KUDOS_KFS_PARTITION_BD_H */
