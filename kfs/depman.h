#ifndef __KUDOS_KFS_DEPMAN_BD_H
#define __KUDOS_KFS_DEPMAN_BD_H

#include <kfs/chdesc.h>
#include <kfs/bdesc.h>

/* initialize the dependency manager */
int depman_init(void);

/* forward a chdesc through bdesc translation automatically, from bdesc_retain() */
int depman_forward_chdesc(bdesc_t * from, bdesc_t * to);

/* explicitly translate a chdesc when necessary, like for block size alterations that do not happen automatically in bdesc_retain() */
int depman_translate_chdesc(bdesc_t * from, bdesc_t * to, uint32_t offset, uint32_t size);

/* add a chdesc subgraph to the dependency manager - this and all reachable chdescs with reference count 0 */
int depman_add_chdesc(chdesc_t * root);

/* remove an individual chdesc from the dependency manager */
int depman_remove_chdesc(chdesc_t * chdesc);

/* query the dependency manager */
const chdesc_t * depman_get_deps(bdesc_t * block);

#endif /* __KUDOS_KFS_DEPMAN_BD_H */
