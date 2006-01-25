#ifndef __KUDOS_KFS_FUSE_SERVE_INODE_H
#define __KUDOS_KFS_FUSE_SERVE_INODE_H

#include <fuse_lowlevel.h>

// FAIL_INO is the fuse_ino_t returned to indicate failure
#define FAIL_INO ((fuse_ino_t) 0)

void inodes_shutdown(void);
int inodes_init(void);

int add_inode(fuse_ino_t parent, const char * local_name, fuse_ino_t * pino);
void remove_inode(fuse_ino_t ino);

const char * inode_fname(fuse_ino_t ino);
fuse_ino_t inode_parent(fuse_ino_t ino);
fuse_ino_t lname_inode(fuse_ino_t parent, const char * name);

char * fname(fuse_ino_t parent, const char * local_name);

#endif /* __KUDOS_KFS_FUSE_SERVE_INODE_H */
