#ifndef _CFS_IPC_CLIENT_H_
#define _CFS_IPC_CLIENT_H_

#include <inc/serial_cfs.h>

int cfs_open(char *fname, int mode);
int cfs_close(int fid);
int cfs_read(int fid, uint32_t offset, uint32_t size, char *data);
int cfs_write(int fid, uint32_t offset, uint32_t size, char *data);
int cfs_truncate(int fid, uint32_t size);
int cfs_unlink(char *name);
int cfs_link(char *oldname, char *newname);
int cfs_rename(char *oldname, char *newname);
int cfs_mkdir(char *name);
int cfs_rmdir(char *name);
int cfs_get_features(char *name, void *dump);
int cfs_get_metadata(char *name, int id, struct Scfs_metadata *md);
int cfs_set_metadata(char *name, struct Scfs_metadata *md);
int cfs_sync(char *name);
int cfs_shutdown(void);
    
#endif // not _CFS_IPC_CLIENT_H_
