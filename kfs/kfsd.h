#ifndef __KUDOS_KFS_KFSD
#define __KUDOS_KFS_KFSD

typedef void (*kfsd_shutdown_module)(void * arg);
int kfsd_register_shutdown_module(kfsd_shutdown_module fn, void * arg);

void kfsd_shutdown(void);

#endif // not __KUDOS_KFS_KFSD
