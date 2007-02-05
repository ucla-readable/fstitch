#ifndef __KUDOS_KFS_KFSD
#define __KUDOS_KFS_KFSD

#include <lib/types.h>

// When a shutdown_module callback will be made
#define SHUTDOWN_PREMODULES  1 // before modules are deconstructed
#define SHUTDOWN_POSTMODULES 2 // after modules are deconstructed

typedef void (*kfsd_shutdown_module)(void * arg);
int _kfsd_register_shutdown_module(const char * name, kfsd_shutdown_module fn, void * arg, int when);
#define kfsd_register_shutdown_module(fn, arg, when) _kfsd_register_shutdown_module(#fn, fn, arg, when)

void kfsd_request_shutdown(void);
int kfsd_is_running(void);

/* every userspace request gets a unique ID */
void kfsd_next_request_id(void);
uint32_t kfsd_get_request_id(void);

#endif // not __KUDOS_KFS_KFSD
