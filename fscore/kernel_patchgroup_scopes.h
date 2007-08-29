/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_KERNEL_PATCHGROUP_SCOPES_H
#define __FSTITCH_FSCORE_KERNEL_PATCHGROUP_SCOPES_H

#include <fscore/patchgroup.h>

struct task_struct;

int kernel_patchgroup_scopes_init(void);

patchgroup_scope_t * process_patchgroup_scope(const struct task_struct * task);

#endif /* __FSTITCH_FSCORE_KERNEL_PATCHGROUP_SCOPES_H */
