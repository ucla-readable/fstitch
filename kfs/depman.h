#ifndef __KUDOS_KFS_DEPMAN_BD_H
#define __KUDOS_KFS_DEPMAN_BD_H

#include <kfs/chdesc.h>
#include <kfs/bdesc.h>

int depman_forward_chdesc(bdesc_t * from, bdesc_t * to);

int depman_add_chdesc(chdesc_t * chdesc);
int depman_remove_chdesc(chdesc_t * chdesc);
chdesc_t * depman_get_deps(bdesc_t * block);

#endif /* __KUDOS_KFS_DEPMAN_BD_H */
