#ifndef __KUDOS_KFS_FUSE_SERVE_H
#define __KUDOS_KFS_FUSE_SERVE_H

#include <kfs/cfs.h>

void    set_frontend_cfs(CFS_t * cfs);
CFS_t * get_frontend_cfs(void);

void fuse_serve_loop(int argc, char ** argv);

#endif /* __KUDOS_KFS_FUSE_SERVE_H */
