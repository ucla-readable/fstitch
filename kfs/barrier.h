#ifndef __KUDOS_KFS_BARRIER_H
#define __KUDOS_KFS_BARRIER_H

#include <lib/types.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>

int barrier_single_forward(BD_t * target, uint32_t number, BD_t * barrier, bdesc_t * block);

typedef struct {
	BD_t *   target;
	uint32_t number;
	/* these fields are used internally */
	bool _synthetic;
	bdesc_t * _block;
} multiple_forward_t;

int barrier_multiple_forward(multiple_forward_t forwards[], size_t nforwards, BD_t * barrier, bdesc_t * block);

#endif /* __KUDOS_KFS_BARRIER_H */
