#ifndef __KUDOS_KFS_FIDMAN_CFS_H
#define __KUDOS_KFS_FIDMAN_CFS_H

// fidman_cfs notices when fids are no longer in use (by analyzing the pageref
// count associated with the fid) and calls close on fidman's frontend_cfs
// exactly when it has detected a fid is no longer in use.
// fidman_cfs also uses its knowledge of fids to allow create_fid() to
// not handout a fid still in use.

// Characterization of fidman:
// depman is characterized as Santa Claus. fidman, as a fairy god mother.
// Modules don't know of fidman, but fidman is there behind the scenes
// helping CFS modules out by letting them know when fids are no longer in use.


#include <kfs/cfs.h>

// The range used by fidman for mapping client Fd pages.
#define FIDMAN_CFS_FD_MAP ((void *) 0xB0000000)
#define FIDMAN_CFS_FD_END ((void *) 0xC0000000)

CFS_t * fidman_cfs(CFS_t * frontend_cfs);

#endif /* __KUDOS_KFS_FIDMAN_CFS_H */
