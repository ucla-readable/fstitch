#ifndef __KUDOS_KFS_FUSE_SERVE_INODE_H
#define __KUDOS_KFS_FUSE_SERVE_INODE_H

#include <fuse_lowlevel.h>

// FAIL_INO is the fuse_ino_t returned to indicate failure
#define FAIL_INO ((fuse_ino_t) 0)

typedef struct inodes inodes_t;

inodes_t * fuse_serve_inodes_create(void);
void fuse_serve_inodes_destroy(inodes_t * i);

// Set the current inodes_t*
void fuse_serve_inodes_set_cur(inodes_t * i);
// Clear the current inodes_t*
void fuse_serve_inodes_clear_cur(void);


// These functions operate using the current inodes_t*

int add_inode(fuse_ino_t parent, const char * local_name, fuse_ino_t * pino);
void remove_inode(fuse_ino_t ino);

const char * inode_fname(fuse_ino_t ino);
fuse_ino_t inode_parent(fuse_ino_t ino);
fuse_ino_t lname_inode(fuse_ino_t parent, const char * name);

char * fname(fuse_ino_t parent, const char * local_name);

#endif /* __KUDOS_KFS_FUSE_SERVE_INODE_H */
