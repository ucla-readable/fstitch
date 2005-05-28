#ifndef __KUDOS_KFS_BARRIER_H
#define __KUDOS_KFS_BARRIER_H

#include <inc/types.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>

/* forward chdescs which are at the bottom of one barrier zone to the top of the
 * next barrier zone, performing a revision prepare/revert and using synthetic
 * blocks to avoid unnecessary reads */
int barrier_simple_forward(BD_t * target, uint32_t number, BD_t * barrier, bdesc_t * block);

/* forward chdescs as above, but only those within the given range and to the
 * relative block offsets in the target block whose number is specified - very
 * much like the old depman_translate_chdesc(), which should be removed */
typedef struct partial_forward {
	BD_t *   target;
	uint32_t number;
	uint32_t offset;
	uint32_t size;
} partial_forward_t;

int barrier_partial_forward(partial_forward_t forwards[], size_t nforwards, BD_t * barrier, bdesc_t * block);

#endif /* __KUDOS_KFS_BARRIER_H */
