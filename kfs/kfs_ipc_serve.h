#ifndef __KUDOS_KFS_KFS_IPC_SERVE_H
#define __KUDOS_KFS_KFS_IPC_SERVE_H

#include <inc/env.h>

int  kfs_ipc_serve_init(void);
void kfs_ipc_serve_run(envid_t whom, void * pg, int perm, uint32_t cur_cappa);

#endif // __KUDOS_KFS_KFS_IPC_SERVE_H
