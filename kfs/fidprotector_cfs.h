#ifndef __KUDOS_KFS_FIDPROTECTOR_CFS_H
#define __KUDOS_KFS_FIDPROTECTOR_CFS_H

// fidprotector_cfs helps out its frontend_cfs by allowing fid-using
// CFS requests through only when the requesting env sent the capability
// associated with the given fid.

#include <kfs/cfs.h>

CFS_t * fidprotector_cfs(CFS_t * frontend_cfs);

#endif /* __KUDOS_KFS_FIDPROTECTOR_CFS_H */
