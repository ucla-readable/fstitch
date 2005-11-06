#ifndef __KUDOS_KFS_IPC_SERVE_H
#define __KUDOS_KFS_IPC_SERVE_H

#include <lib/mmu.h>
#include <kfs/fidcloser_cfs.h>

// VA at which to receive page mappings containing client reqs.
// Just before the range used by the UHFS module for mapping client pages.
#define IPCSERVE_REQVA (FIDCLOSER_CFS_FD_MAP - PGSIZE)

int  ipc_serve_init(void);
void ipc_serve_run(void);

#endif // __KUDOS_KFS_IPC_SERVE_H
