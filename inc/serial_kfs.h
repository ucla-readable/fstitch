#ifndef __KUDOS_INC_SERIAL_KFS_H
#define __KUDOS_INC_SERIAL_KFS_H

#include <inc/types.h>
#include <inc/mmu.h>

#define SKFS_VAL 2

//
// KFS Methods

#define SKFS_DESTROY_CFS 1
#define SKFS_DESTROY_LFS 2
#define SKFS_DESTROY_BD  3

#define SKFS_TABLE_CLASSIFIER_CFS 4
#define SKFS_TABLE_CLASSIFIER_CFS_ADD 5
#define SKFS_TABLE_CLASSIFIER_CFS_REMOVE 6


#define SKFS_TYPE int skfs_type

// SKFS_MAX_NAMELEN is the maxiumum length that fits in a method page, given
// where names are used and the common deonimator amount of space available
// in these pages.
#define SKFS_MAX_NAMELEN (PGSIZE - 3*sizeof(int))


// destructors

typedef struct {
	SKFS_TYPE;
	uint32_t cfs;
} Skfs_destroy_cfs_t;

typedef struct {
	SKFS_TYPE;
	uint32_t lfs;
} Skfs_destroy_lfs_t;

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
} Skfs_destroy_bd_t;


// table_classifier_cfs

typedef struct {
	SKFS_TYPE;
} Skfs_table_classifier_cfs_t;

typedef struct {
	SKFS_TYPE;
	uint32_t cfs;
	uint32_t path_cfs;
	char path[SKFS_MAX_NAMELEN];
} Skfs_table_classifier_cfs_add_t;

typedef struct {
	SKFS_TYPE;
	uint32_t cfs;
	char path[SKFS_MAX_NAMELEN];
} Skfs_table_classifier_cfs_remove_t;

#endif // __KUDOS_INC_SERIAL_KFS_H
