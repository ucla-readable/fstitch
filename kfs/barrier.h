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
/* NOTE: this would be more efficient if we did not have to prepare/revert the
 * block for each range, so this should be looked into for improvement */
int barrier_partial_forward(BD_t * target, uint32_t number, BD_t * barrier, bdesc_t * block, uint32_t offset, uint32_t size);

#endif /* __KUDOS_KFS_BARRIER_H */
