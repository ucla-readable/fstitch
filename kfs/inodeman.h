#ifndef __KUDOS_KFS_INODEMAN_H
#define __KUDOS_KFS_INODEMAN_H

#include <kfs/cfs.h>
#include <kfs/inode.h>
#include <lib/vector.h>

#define MAXPATHLEN 1024
#define mount_table_t vector_t

struct mount_entry {
	const char * path;
	CFS_t * cfs;
};
typedef struct mount_entry mount_entry_t;

vector_t * inodeman_create(void);
void inodeman_destroy();

int path_to_inode(const char * path, CFS_t ** cfs, inode_t * ino);
int path_to_parent_and_name(const char * path, CFS_t ** cfs, inode_t * parent, char ** filename);

mount_table_t * get_mount_table();

#endif // __KUDOS_KFS_INODEMAN_H
