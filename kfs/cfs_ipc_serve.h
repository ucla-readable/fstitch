#ifndef __KUDOS_KFS_CFS_IPC_SERVE_H
#define __KUDOS_KFS_CFS_IPC_SERVE_H

#include <kfs/cfs.h>
#include <inc/env.h>
#include <kfs/ipc_serve.h> // for IPCSERVE_REQVA

#define PAGESNDVA (IPCSERVE_REQVA - PGSIZE)

void    set_frontend_cfs(CFS_t * cfs);
CFS_t * get_frontend_cfs(void);

int  cfs_ipc_serve_init(void);
void cfs_ipc_serve_run(envid_t whom, void * pg, int perm, uint32_t cur_cappa);

// Return a ptr to the current page associated with the open() call.
// NULL on error.
const void * cfs_ipc_serve_cur_page(void);

// Return the envid associated with the current requenst.
// 0 indicates intra-kfsd calls.
envid_t cfs_ipc_serve_cur_envid(void);

// Return the capability page's physical address associated with the current
// request. 0 indicates intra-kfsd privilege.
uint32_t cfs_ipc_serve_cur_cappa(void);

void cfs_ipc_serve_set_cur_cappa(uint32_t);


#endif // not __KUDOS_KFS_CFS_IPC_SERVE_H
