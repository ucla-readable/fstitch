#ifndef __KUDOS_KFS_SCHED_H
#define __KUDOS_KFS_SCHED_H

#include <lib/types.h>

typedef void (*sched_callback)(void * arg);

int  sched_register(const sched_callback fn, void * arg, int32_t freq_jiffies);
int  sched_unregister(const sched_callback fn, void * arg);

int  kfsd_sched_init(void);

void sched_run_callbacks(void);
void sched_run_cleanup(void);

#endif /* __KUDOS_KFS_SCHED_H */
