#ifndef __KUDOS_KFS_FIDFAIRY_CFS_H
#define __KUDOS_KFS_FIDFAIRY_CFS_H

// fidfairy_cfs helps out its frontend_cfs by determining when fids
// are no longer in use, by analyzing the pageref count associated
// with the fid, and calling close on fidfairy's frontend_cfs exactly
// when fidfairy has detected a fid is no longer in use.

// Characterization of fidfairy:
// depman is characterized as Santa Claus. fidfairy, as a fairy god mother.
// Modules don't know of fidfairy, but fidfairy is there behind the scenes
// helping CFS modules out.


#include <kfs/cfs.h>

// The range used by fidfairy for mapping client Fd pages.
#define FIDFAIRY_CFS_FD_MAP ((void *) 0xB0000000)
#define FIDFAIRY_CFS_FD_END ((void *) 0xC0000000)

CFS_t * fidfairy_cfs(CFS_t * frontend_cfs);

#endif /* __KUDOS_KFS_FIDFAIRY_CFS_H */
