#ifndef __KUDOS_KFS_KERNEL_OPGROUP_SCOPES_H
#define __KUDOS_KFS_KERNEL_OPGROUP_SCOPES_H

#include <kfs/opgroup.h>

struct task_struct;

int kernel_opgroup_scopes_init(void);

opgroup_scope_t * process_opgroup_scope(const struct task_struct * task);

#endif /* __KUDOS_KFS_KERNEL_OPGROUP_SCOPES_H */
