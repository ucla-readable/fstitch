#ifndef __KUDOS_KFS_KERNEL_OPGROUP_SCOPES_H
#define __KUDOS_KFS_KERNEL_OPGROUP_SCOPES_H

#include <linux/types.h>
#include <kfs/opgroup.h>

int kernel_opgroup_scopes_init(void);

opgroup_scope_t * process_opgroup_scope(pid_t pid);

#endif /* __KUDOS_KFS_KERNEL_OPGROUP_SCOPES_H */
