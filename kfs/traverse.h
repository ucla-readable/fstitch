#ifndef __KUDOS_KFS_TRAVERSE_H
#define __KUDOS_KFS_TRAVERSE_H

#include <kfs/cfs.h>
#include <kfs/inode.h>
#include <lib/vector.h>

#define MAXPATHLEN 1024

struct mount_entry {
	const char * path;
	CFS_t * cfs;
};
typedef struct mount_entry mount_entry_t;

int traverse_init(void);
void traverse_shutdown();

int path_to_inode(const char * path, CFS_t ** cfs, inode_t * ino);
int path_to_parent_and_name(const char * path, CFS_t ** cfs, inode_t * parent, char ** filename);

// Return the mount table as a vector of mount_entry_t*
vector_t * get_mount_table();

#endif // __KUDOS_KFS_TRAVERSE_H
