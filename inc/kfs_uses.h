#ifndef KUDOS_INC_KFS_USES_H
#define KUDOS_INC_KFS_USES_H

#include <lib/hash_map.h>
#include <lib/vector.h>
#include <kfs/cfs.h>

struct kfs_node;
typedef struct kfs_node kfs_node_t;

typedef struct {
	kfs_node_t * node;
	const char * name;
} kfs_use_t;

struct kfs_node {
	enum {NCFS, NLFS, NBD} type;
	void * obj;
	const char * name;
	vector_t * uses; // vector of kfs_use_t*
};

// Return a hash_map of <CFS/LFS/BD_t *, kfs_node_t *> for kfsd's configuration
hash_map_t * kfs_uses();

// Return the root mount_selector_cfs
CFS_t * get_mount_selector();

#endif /* !KUDOS_INC_KFS_USES_H */
