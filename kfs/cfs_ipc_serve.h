#ifndef __KUDOS_KFS_CFS_IPC_SERVE_H
#define __KUDOS_KFS_CFS_IPC_SERVE_H

int register_frontend_cfs(CFS_t * cfs);

int cfs_ipc_serve();
void cfs_ipc_serve_run();

#endif // not __KUDOS_KFS_CFS_IPC_SERVE_H
