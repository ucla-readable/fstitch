#ifndef __KUDOS_KFS_CHDESC_UTIL_H
#define __KUDOS_KFS_CHDESC_UTIL_H

#include <kfs/chdesc.h>

/* mark a chdesc graph (i.e. set CHDESC_MARKED) */
void chdesc_mark_graph(chdesc_t * root);

/* unmark a chdesc graph (i.e. clear CHDESC_MARKED) */
void chdesc_unmark_graph(chdesc_t * root);

/* push all change descriptors at this block device on a block (i.e. data) descriptor to a new block device and block */
int chdesc_push_down(BD_t * current_bd, bdesc_t * current_block, BD_t * target_bd, bdesc_t * target_block);

/* move a chdesc to a new bdesc (at a barrier) */
int chdesc_move(chdesc_t * chdesc, bdesc_t * destination, BD_t * target_bd, uint16_t source_offset);
void chdesc_finish_move(bdesc_t * destination);

/* reassign the block pointer in a NOOP chdesc */
int chdesc_noop_reassign(chdesc_t * noop, bdesc_t * block);

#endif /* __KUDOS_KFS_CHDESC_UTIL_H */
