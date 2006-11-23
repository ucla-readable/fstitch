#ifndef __KUDOS_LIB_SERIAL_CFS_H
#define __KUDOS_LIB_SERIAL_CFS_H

#include <lib/types.h>
#include <lib/mmu.h>
#include <inc/env.h>

#include <kfs/opgroup.h>

#define SCFS_VAL 1

//
// CFS Methods

#define SCFS_OPEN 1
#define SCFS_CLOSE 2
#define SCFS_READ 3
#define SCFS_WRITE 4
#define SCFS_GETDIRENTRIES 5
#define SCFS_GETDIRENTRIES_RETURN 6
#define SCFS_TRUNCATE 7
#define SCFS_UNLINK 8
#define SCFS_LINK 9
#define SCFS_RENAME 10
#define SCFS_MKDIR 11
#define SCFS_RMDIR 12
#define SCFS_GET_NUM_FEATURES 13
#define SCFS_GET_FEATURE 14
#define SCFS_GET_METADATA 15
#define SCFS_SET_METADATA 16
#define SCFS_OPGROUP_SCOPE_CREATE 17
#define SCFS_OPGROUP_SCOPE_COPY 18
#define SCFS_OPGROUP_CREATE 19
#define SCFS_OPGROUP_ADD_DEPEND 20
#define SCFS_OPGROUP_ENGAGE 21
#define SCFS_OPGROUP_DISENGAGE 22
#define SCFS_OPGROUP_RELEASE 23
#define SCFS_OPGROUP_ABANDON 24
#define SCFS_SHUTDOWN 25
#define SCFS_DEBUG 26

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

struct Scfs_getdirentries {
	SCFS_TYPE;
	int fid;
	int nbytes;
	off_t basep;
};

struct Scfs_getdirentries_return {
	SCFS_TYPE;
	int nbytes_read;
	off_t basep;
	char buf[PGSIZE - sizeof(int) - sizeof(int) - sizeof(off_t)];
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

struct Scfs_opgroup_scope_create {
	SCFS_TYPE;
	uintptr_t scope_cappg_va;
};

struct Scfs_opgroup_scope_copy {
	SCFS_TYPE;
	envid_t child;
	uintptr_t child_scope_cappg_va;
};

struct Scfs_opgroup_create {
	SCFS_TYPE;
	int flags;
};

struct Scfs_opgroup_add_depend {
	SCFS_TYPE;
	opgroup_id_t after;
	opgroup_id_t before;
};

struct Scfs_opgroup_engage {
	SCFS_TYPE;
	opgroup_id_t opgroup;
};

struct Scfs_opgroup_disengage {
	SCFS_TYPE;
	opgroup_id_t opgroup;
};

struct Scfs_opgroup_release {
	SCFS_TYPE;
	opgroup_id_t opgroup;
};

struct Scfs_opgroup_abandon {
	SCFS_TYPE;
	opgroup_id_t opgroup;
};

struct Scfs_shutdown {
	SCFS_TYPE;
};

struct Scfs_debug {
	SCFS_TYPE;
};

//
// CFS "data-page-blobs"

struct Scfs_metadata {
	uint32_t id;
	size_t size;
	uint8_t data[PGSIZE - sizeof(size_t) - sizeof(uint32_t)];
};

#endif // not __KUDOS_LIB_SERIAL_CFS_H
