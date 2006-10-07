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
int chdesc_rewrite_block(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head);

/* reassign the block pointer in a NOOP chdesc */
int chdesc_noop_reassign(chdesc_t * noop, bdesc_t * block);

/* roll back and apply collections of change descriptors in the proper order */
int chdesc_rollback_collection(int count, chdesc_t ** chdescs, void ** order);
int chdesc_apply_collection(int count, chdesc_t ** chdescs, void ** order);
void chdesc_order_destroy(void ** order);

/* detach befores and afters of change descriptors */
int chdesc_detach_befores(chdesc_t * chdesc);
int chdesc_detach_afters(chdesc_t * chdesc);

/* duplicate, split, and merge change descriptors */
int chdesc_duplicate(chdesc_t * original, int count, bdesc_t ** blocks);
int chdesc_split(chdesc_t * original, int count);
int chdesc_merge(int count, chdesc_t ** chdescs, chdesc_t ** head);

/* create change descriptors based on the diff of two data regions */
int chdesc_create_diff(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, chdesc_t ** head);

/* Create two noops, one of which prevents the other from being satified. */
int chdesc_create_blocked_noop(chdesc_t ** noophead, chdesc_t ** drain_plug);

#endif /* __KUDOS_KFS_CHDESC_UTIL_H */
