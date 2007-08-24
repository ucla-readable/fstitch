#ifndef __FSTITCH_FSCORE_FUSE_SERVE_H
#define __FSTITCH_FSCORE_FUSE_SERVE_H

#include <fscore/cfs.h>

// Add a mount at path for cfs.
// Can only be called before entering fuse_serve_loop().
int fuse_serve_add_mount(const char * path, CFS_t * cfs);

#define fstitchd_add_mount(p, c) fuse_serve_add_mount(p, c)

typedef void (*unlock_callback_t)(void *, int);
int fstitchd_unlock_callback(unlock_callback_t callback, void * data);

int fuse_serve_init(int argc, char ** argv);
int fuse_serve_loop();

#endif /* __FSTITCH_FSCORE_FUSE_SERVE_H */
