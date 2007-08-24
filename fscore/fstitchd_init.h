#ifndef __FSTITCH_FSCORE_FSTITCHD_INIT
#define __FSTITCH_FSCORE_FSTITCHD_INIT

#define ALLOW_JOURNAL 1
#define ALLOW_UNLINK 1
#define ALLOW_UNSAFE_DISK_CACHE 1
#define ALLOW_CRASHSIM 1

// Bring fstitchd's modules up.
int fstitchd_init(int nwbblocks);

#endif // not __FSTITCH_FSCORE_FSTITCHD_INIT
