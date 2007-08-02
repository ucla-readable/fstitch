#ifndef __KUDOS_KFS_KFSD_INIT
#define __KUDOS_KFS_KFSD_INIT

#define ALLOW_JOURNAL 1
#define ALLOW_UNLINK 1
#define ALLOW_UNSAFE_DISK_CACHE 1
#define ALLOW_CRASHSIM 1

// Bring kfsd's modules up.
int kfsd_init(int nwbblocks);

#endif // not __KUDOS_KFS_KFSD_INIT
