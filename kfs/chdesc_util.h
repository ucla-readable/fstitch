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

/* write an entire block without creating many layers of change descriptors */
int chdesc_rewrite_block(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head, chdesc_t ** tail);

/* reassign the block pointer in a NOOP chdesc */
int chdesc_noop_reassign(chdesc_t * noop, bdesc_t * block);

/* roll back and apply collections of change descriptors in the proper order */
int chdesc_rollback_collection(int count, chdesc_t ** chdescs, void ** order);
int chdesc_apply_collection(int count, chdesc_t ** chdescs, void ** order);
void chdesc_order_destroy(void ** order);

/* detach dependencies and dependents of change descriptors */
int chdesc_detach_dependencies(chdesc_t * chdesc);
int chdesc_detach_dependents(chdesc_t * chdesc);

/* duplicate, morph, split, and merge change descriptors */
int chdesc_duplicate(chdesc_t * original, int count, bdesc_t ** blocks);
int chdesc_split(chdesc_t * original, int count);
int chdesc_merge(int count, chdesc_t ** chdescs, chdesc_t ** head, chdesc_t ** tail);

#endif /* __KUDOS_KFS_CHDESC_UTIL_H */
