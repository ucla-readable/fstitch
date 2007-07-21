#ifndef __KUDOS_KFS_CHDESC_UTIL_H
#define __KUDOS_KFS_CHDESC_UTIL_H

#include <kfs/chdesc.h>

/* mark a chdesc graph (i.e. set CHDESC_MARKED) */
void chdesc_mark_graph(chdesc_t * root);

/* unmark a chdesc graph (i.e. clear CHDESC_MARKED) */
void chdesc_unmark_graph(chdesc_t * root);

/* write an entire block without creating many layers of change descriptors */
int chdesc_rewrite_block(bdesc_t * block, BD_t * owner, void * data, chdesc_t ** head);

/* create change descriptors based on the diff of two data regions */
int chdesc_create_diff(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, chdesc_t ** head);
int chdesc_create_diff_set(bdesc_t * block, BD_t * owner, uint16_t offset, uint16_t length, const void * olddata, const void * newdata, chdesc_t ** tail, chdesc_pass_set_t * befores);

#endif /* __KUDOS_KFS_CHDESC_UTIL_H */
