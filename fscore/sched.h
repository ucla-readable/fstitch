/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_SCHED_H
#define __FSTITCH_FSCORE_SCHED_H

typedef void (*sched_callback)(void * arg);

int  sched_register(const sched_callback fn, void * arg, int32_t freq_jiffies);
int  sched_unregister(const sched_callback fn, void * arg);

int  fstitchd_sched_init(void);

void sched_run_callbacks(void);
void sched_run_cleanup(void);

#endif /* __FSTITCH_FSCORE_SCHED_H */
