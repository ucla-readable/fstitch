/* This file is part of Featherstitch. Featherstitch is copyright 2008 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_KERNEL_SERVE_SYSCALLS_H
#define __FSTITCH_FSCORE_KERNEL_SERVE_SYSCALLS_H

void shadow_syscalls(void);
void restore_syscalls(void);

#endif // !__FSTITCH_FSCORE_KERNEL_SERVE_SYSCALLS_H
