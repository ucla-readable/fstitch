#ifndef _CFS_IPC_CLIENT_H_
#define _CFS_IPC_CLIENT_H_

#include <inc/serial_cfs.h>

int cfs_open(const char *fname, int mode, void *refpg);
int cfs_close(int fid);
int cfs_read(int fid, uint32_t offset, uint32_t size, char *data);
int cfs_write(int fid, uint32_t offset, uint32_t size, const char *data);
int cfs_truncate(int fid, uint32_t size);
int cfs_unlink(const char *name);
int cfs_link(const char *oldname, const char *newname);
int cfs_rename(const char *oldname, const char *newname);
int cfs_mkdir(const char *name);
int cfs_rmdir(const char *name);
int cfs_get_features(const char *name, void *dump);
int cfs_get_metadata(const char *name, int id, struct Scfs_metadata *md);
int cfs_set_metadata(const char *name, struct Scfs_metadata *md);
int cfs_sync(const char *name);
int cfs_shutdown(void);
int cfs_debug(void);

#endif // not _CFS_IPC_CLIENT_H_
