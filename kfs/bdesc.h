#ifndef __KUDOS_KFS_BDESC_H
#define __KUDOS_KFS_BDESC_H

#include <inc/types.h>

/* struct BD needs bdesc, so we avoid the cycle */
struct BD;

struct bdesc;
typedef struct bdesc bdesc_t;

struct bdesc {
	struct BD * bd;
	uint32_t number, refs;
	uint16_t offset, length;
	uint8_t * data;
};

#endif /* __KUDOS_KFS_BDESC_H */
