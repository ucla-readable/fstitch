#ifndef __KUDOS_KFS_ORDER_PRESERVER_BD_H
#define __KUDOS_KFS_ORDER_PRESERVER_BD_H

// order_preserver_bd creates change descriptors on each received write_block()
// so that the blocks will be written in the order they are received
// by order_preserver_bd.

#include <inc/types.h>
#include <kfs/bd.h>

BD_t * order_preserver_bd(BD_t * disk);

#endif /* __KUDOS_KFS_ORDER_PRESERVER_BD_H */
