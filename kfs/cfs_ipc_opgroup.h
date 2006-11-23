#ifndef __KUDOS_KFS_CFS_IPC_OPGROUP_H
#define __KUDOS_KFS_CFS_IPC_OPGROUP_H

// CFS IPC opgroup support; three pieces:
// - cfs ipc opgroup scopes: manipulate opgroup scopes through CFS IPC
// - cfs ipc opgroups: manipulate opgroups through CFS IPC
// - opgroupscope_tracker_cfs: CFS_t module that tracks a CFS IPC request's
//   opgroup scope

#include <inc/env.h>
#include <kfs/cfs_ipc_serve.h> // for PAGESNDVA
#include <kfs/opgroup.h>

// The range used for mapping cfs client opgroup scope pages
#define CFS_IPC_OPGROUP_SCOPE_END ((void *) PAGESNDVA)
#define CFS_IPC_OPGROUP_SCOPE_CAPPGS (CFS_IPC_OPGROUP_SCOPE_END - NENV*PGSIZE)


// Tracks environments' opgroup scopes and sets opgroup_scope_set_current()
// accordingly
CFS_t * opgroupscope_tracker_cfs(CFS_t * frontend_cfs);


// Manipulate opgroup scopes
int cfs_ipc_opgroup_scope_create(envid_t envid, const void * scope_page, uintptr_t scope_page_envid_va);
int cfs_ipc_opgroup_scope_copy(envid_t parent, envid_t child, const void * child_scope_page, uintptr_t scope_page_child_va);


// Manipulate opgroups

opgroup_id_t cfs_ipc_opgroup_create(envid_t envid, int flags);
int cfs_ipc_opgroup_add_depend(envid_t envid, opgroup_id_t after, opgroup_id_t before);

int cfs_ipc_opgroup_engage(envid_t envid, opgroup_id_t opgroup);
int cfs_ipc_opgroup_disengage(envid_t envid, opgroup_id_t opgroup);

int cfs_ipc_opgroup_release(envid_t envid, opgroup_id_t opgroup);
int cfs_ipc_opgroup_abandon(envid_t envid, opgroup_id_t opgroup);


#endif /* __KUDOS_KFS_CFS_IPC_OPGROUP_H */
