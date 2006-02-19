#ifndef __KUDOS_KFS_FIDMAN_H
#define __KUDOS_KFS_FIDMAN_H

#include <kfs/fdesc.h>

#define MAX_OPEN_FIDS 512

// Return a kfsd-unique fid
int create_fid(fdesc_t * fdesc);
// Release the given fid
int release_fid(int fid);
// Return the fdesc_t* corresponding to fid
fdesc_t * fid_fdesc(int fid);

#endif /* __KUDOS_KFS_FIDMAN_H */
