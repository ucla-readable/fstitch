#ifndef __KUDOS_KFS_MIRROR_BD_H
#define __KUDOS_KFS_MIRROR_BD_H

#include <kfs/bd.h>

BD_t * mirror_bd(BD_t * disk0, BD_t * disk1, uint32_t stride);

#endif /* __KUDOS_KFS_MIRROR_BD_H */
