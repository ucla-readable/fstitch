#ifndef __KUDOS_KFS_JOURNAL_LFS_H
#define __KUDOS_KFS_JOURNAL_LFS_H

#include <kfs/bd.h>
#include <kfs/lfs.h>

LFS_t * journal_lfs(LFS_t * journal, LFS_t * fs, BD_t * fs_queue);

// Return the maximum average bandwidth in kB/s
size_t journal_lfs_max_bandwidth(const LFS_t * journal);

#endif /* __KUDOS_KFS_JOURNAL_LFS_H */
