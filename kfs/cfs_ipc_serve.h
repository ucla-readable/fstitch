#ifndef __KUDOS_KFS_CFS_IPC_SERVE_H
#define __KUDOS_KFS_CFS_IPC_SERVE_H

#include <kfs/cfs.h>

void    set_frontend_cfs(CFS_t * cfs);
CFS_t * get_frontend_cfs(void);

int  cfs_ipc_serve_init(void);
void cfs_ipc_serve_run(void);

// Return a ptr to the current page associated with the open() call.
// NULL on error.
void * cfs_ipc_serve_cur_page(void);

// Return the capability page's physical address associated with the current
// request.
uint32_t cfs_ipc_serve_cur_cappa(void);

#endif // not __KUDOS_KFS_CFS_IPC_SERVE_H
