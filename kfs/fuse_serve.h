#ifndef __KUDOS_KFS_FUSE_SERVE_H
#define __KUDOS_KFS_FUSE_SERVE_H

#include <kfs/cfs.h>

void    set_frontend_cfs(CFS_t * cfs);
CFS_t * get_frontend_cfs(void);

int fuse_serve_init(int argc, char ** argv);
int fuse_serve_loop();

#endif /* __KUDOS_KFS_FUSE_SERVE_H */
