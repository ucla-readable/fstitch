#ifndef __KUDOS_KFS_FIDCLOSER_CFS_H
#define __KUDOS_KFS_FIDCLOSER_CFS_H

// fidcloser_cfs helps out its frontend_cfs by determining when fids
// are no longer in use, by analyzing the pageref count associated
// with the fid, and calling close on fidcloser's frontend_cfs exactly
// when fidcloser has detected a fid is no longer in use.

// fidcloser is a fidfairy:
// Modules don't know of fidfairies, but they are there behind the scenes
// helping CFS modules out.

#include <kfs/cfs.h>

// The range used by fidcloser for mapping client Fd pages.
#define FIDCLOSER_CFS_FD_MAP ((void *) 0xB0000000)
#define FIDCLOSER_CFS_FD_END ((void *) 0xC0000000)

CFS_t * fidcloser_cfs(CFS_t * frontend_cfs);

#endif /* __KUDOS_KFS_FIDCLOSER_CFS_H */
