#ifndef __KUDOS_INC_SERIAL_CFS_H
#define __KUDOS_INC_SERIAL_CFS_H

#include <inc/types.h>
#include <inc/mmu.h>

//
// CFS Methods

#define SCFS_OPEN 1
#define SCFS_CLOSE 2
#define SCFS_READ 3
#define SCFS_WRITE 4
#define SCFS_TRUNCATE 5
#define SCFS_UNLINK 6
#define SCFS_LINK 7
#define SCFS_RENAME 8
#define SCFS_MKDIR 9
#define SCFS_RMDIR 10
#define SCFS_GET_NUM_FEATURES 11
#define SCFS_GET_FEATURE 12
#define SCFS_GET_METADATA 13
#define SCFS_SET_METADATA 14
#define SCFS_SYNC 15
#define SCFS_SHUTDOWN 16

#define SCFS_TYPE int scfs_type

// SCFSMAXNAMELEN is the maxiumum length we can fit in a method page, given
// where names are used and the common denonimator amount of space available
// in these pages.
#define SCFSMAXNAMELEN ((PGSIZE - 2*sizeof(int)) / 2)


struct Scfs_open {
	SCFS_TYPE;
	int mode;
	char path[SCFSMAXNAMELEN];
};

struct Scfs_close {
	SCFS_TYPE;
	int fid;
};

struct Scfs_read {
	SCFS_TYPE;
	int fid;
	uint32_t offset;
	uint32_t size;
};

struct Scfs_write {
	SCFS_TYPE;
	int fid;
	uint32_t offset;
	uint32_t size;
	// data is sent as a separate page
};

struct Scfs_truncate {
	SCFS_TYPE;
	int fid;
	uint32_t size;
};

struct Scfs_unlink {
	SCFS_TYPE;
	char name[SCFSMAXNAMELEN];
};

struct Scfs_link {
	SCFS_TYPE;
	char oldname[SCFSMAXNAMELEN];
	char newname[SCFSMAXNAMELEN];
};

struct Scfs_rename {
	SCFS_TYPE;
	char oldname[SCFSMAXNAMELEN];
	char newname[SCFSMAXNAMELEN];
};

struct Scfs_mkdir {
	SCFS_TYPE;
	char path[SCFSMAXNAMELEN];
};

struct Scfs_rmdir {
	SCFS_TYPE;
	char path[SCFSMAXNAMELEN];
};

struct Scfs_get_num_features {
	SCFS_TYPE;
	char name[SCFSMAXNAMELEN];
};

struct Scfs_get_feature {
	SCFS_TYPE;
	size_t num;
	char name[SCFSMAXNAMELEN];
};

struct Scfs_get_metadata {
	SCFS_TYPE;
	uint32_t id;
	char name[SCFSMAXNAMELEN];
	
};

struct Scfs_set_metadata {
	SCFS_TYPE;
	char name[SCFSMAXNAMELEN];
	// Scfs_metadata is sent as a separate page
};

struct Scfs_sync {
	SCFS_TYPE;
	char name[SCFSMAXNAMELEN];
};

struct Scfs_shutdown {
	SCFS_TYPE;
};

//
// CFS "data-page-blobs"

struct Scfs_metadata {
	uint32_t id;
	size_t size;
	uint8_t data[PGSIZE - sizeof(int) - sizeof(uint32_t)];
};

#endif // not __KUDOS_INC_SERIAL_CFS_H
