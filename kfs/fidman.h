#ifndef __KUDOS_KFS_FIDMAN_H
#define __KUDOS_KFS_FIDMAN_H

#define MAX_OPEN_FIDS 512

// Return a kfsd-unique fid
int create_fid(void);
// Release the given fid
int release_fid(int fid);

#endif /* __KUDOS_KFS_FIDMAN_H */
