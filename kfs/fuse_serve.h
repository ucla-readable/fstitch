#ifndef __KUDOS_KFS_FUSE_SERVE_H
#define __KUDOS_KFS_FUSE_SERVE_H

#include <kfs/cfs.h>

// Add a mount at path for cfs.
// Can only be called before entering fuse_serve_loop().
int fuse_serve_add_mount(const char * path, CFS_t * cfs);

#define kfsd_add_mount(p, c) fuse_serve_add_mount(p, c)

typedef void (*unlock_callback_t)(void *, int);
int kfsd_unlock_callback(unlock_callback_t callback, void * data);

int fuse_serve_init(int argc, char ** argv);
int fuse_serve_loop();

#endif /* __KUDOS_KFS_FUSE_SERVE_H */
