#ifndef __KUDOS_KFS_FIDMAN_H
#define __KUDOS_KFS_FIDMAN_H

#include <linux/pagemap.h>
#include <kfs/fdesc.h>

// fidman manages fids:
// - gives fids unique ids
// - associates a fid with an fdesc
// - prevents envs that do not have a fid from using the fid's fdesc
// - prevents fids from being closed until the last user closes the fid
// - closes fids when no clients remain

#define MAX_OPEN_FIDS 512

// The range used by fidman for mapping client Fd pages
#define FIDMAN_FD_MAP (FIDMAN_FD_END - MAX_OPEN_FIDS*PAGE_SIZE)
#define FIDMAN_FD_END ((void *) 0xC0000000)

// Return a kfsd-unique fid
int create_fid(fdesc_t * fdesc);
// Release the given fid
int release_fid(int fid);

// Set *fdesc to the fdesc_t* corresponding to fid
// Also protects fid against use by environments without access to fid
int fid_fdesc(int fid, fdesc_t ** fdesc);
// Set *fdesc to the fdesc_t* corresponding to fid iff fid is closeable
bool fid_closeable_fdesc(int fid, fdesc_t ** fdesc);

#endif /* __KUDOS_KFS_FIDMAN_H */
