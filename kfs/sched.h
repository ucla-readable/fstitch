#ifndef __KUDOS_KFS_SCHED_H
#define __KUDOS_KFS_SCHED_H

#include <inc/types.h>

typedef void (*sched_callback)(void * arg);

int  sched_register(const sched_callback fn, void * arg, int32_t freq_centisecs);
int  sched_unregister(const sched_callback fn);

int  sched_init();
void sched_loop();

#endif /* __KUDOS_KFS_SCHED_H */
