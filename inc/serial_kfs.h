#ifndef __KUDOS_INC_SERIAL_KFS_H
#define __KUDOS_INC_SERIAL_KFS_H

#include <inc/types.h>
#include <inc/mmu.h>

#define SKFS_VAL 2

// Destructors

#define SKFS_DESTROY_CFS 1
#define SKFS_DESTROY_LFS 2
#define SKFS_DESTROY_BD  3

// CFS

#define SKFS_TABLE_CLASSIFIER_CFS 4
#define SKFS_TABLE_CLASSIFIER_CFS_ADD 5
#define SKFS_TABLE_CLASSIFIER_CFS_REMOVE 6

#define SKFS_UHFS 7

// LFS

#define SKFS_JOURNAL_LFS 8
#define SKFS_JOURNAL_LFS_MAX_BANDWIDTH 9

#define SKFS_JOSFS_BASE 10

#define SKFS_WHOLEDISK 11

// BD

#define SKFS_LOOP_BD 12
#define SKFS_NBD_BD 13
#define SKFS_JOURNAL_QUEUE_BD 14
#define SKFS_ORDER_PRESERVER_BD 15
#define SKFS_CHDESC_STRIPPER_BD 16
#define SKFS_WB_CACHE_BD 17
#define SKFS_WT_CACHE_BD 18
#define SKFS_BLOCK_RESIZER_BD 19
#define SKFS_MD_BD 20
#define SKFS_MIRROR_BD 21
#define SKFS_PARTITION_BD 22
#define SKFS_PC_PTABLE_BD 23
#define SKFS_IDE_PIO_BD 24

// modman

#define SKFS_MODMAN_REQUEST_LOOKUP 25
#define SKFS_MODMAN_RETURN_LOOKUP  26
#define SKFS_MODMAN_RETURN_LOOKUP_USER 27
#define SKFS_MODMAN_REQUEST_ITS    28
#define SKFS_MODMAN_RETURN_IT      29


#define SKFS_TYPE int skfs_type

// SKFS_MAX_NAMELEN is the maxiumum length that fits in a method page, given
// where names are used and the common deonimator amount of space available
// in these pages.
#define SKFS_MAX_NAMELEN (PGSIZE - 4*sizeof(uint32_t))


//
// Destructors

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


//
// CFS

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

// uhfs

typedef struct {
	SKFS_TYPE;
	uint32_t lfs;
} Skfs_uhfs_t;

// TODO: devfs_cfs
// TODO: josfs_cfs

// Not present: fidcloser
// Not present: fidprotector


//
// LFS

// journal_lfs

typedef struct {
	SKFS_TYPE;
	uint32_t journal_lfs;
	uint32_t fs_lfs;
	uint32_t fs_queue_bd;
} Skfs_journal_lfs_t;

typedef struct {
	SKFS_TYPE;
	uint32_t journal_lfs;
} Skfs_journal_lfs_max_bandwidth_t;

// josfs_base

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
	int do_fsck; // TODO: return success/failure?
} Skfs_josfs_base_t;

// wholedisk_lfs

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
} Skfs_wholedisk_t;


//
// BD

typedef struct {
	SKFS_TYPE;
	uint32_t lfs;
	char file[SKFS_MAX_NAMELEN];
} Skfs_loop_bd_t;

typedef struct {
	SKFS_TYPE;
	char address[SKFS_MAX_NAMELEN];
	uint16_t port;
} Skfs_nbd_bd_t;

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
} Skfs_journal_queue_bd_t;

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
} Skfs_order_preserver_bd_t;

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
} Skfs_chdesc_stripper_bd_t;

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
	uint32_t blocks;
} Skfs_wb_cache_bd_t;

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
	uint32_t blocks;
} Skfs_wt_cache_bd_t;

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
	uint16_t blocksize;
} Skfs_block_resizer_bd_t;

typedef struct {
	SKFS_TYPE;
	uint32_t disk0;
	uint32_t disk1;
} Skfs_md_bd_t;

typedef struct {
	SKFS_TYPE;
	uint32_t disk0;
	uint32_t disk1;
	uint32_t stride;
} Skfs_mirror_bd_t;

// TODO: partition_bd
// TODO: pc_ptable_bd

typedef struct {
	SKFS_TYPE;
	uint8_t controller;
	uint8_t disk;
} Skfs_ide_pio_bd_t;


//
// modman

// 'int type' fields are 0 CFS, 1 LFS, 2 BD

typedef struct {
	SKFS_TYPE;
	int type;
	uint32_t id;
} Skfs_modman_request_lookup_t;

typedef struct {
	SKFS_TYPE;
	int type;
	uint32_t id;
	int usage;
	char name[SKFS_MAX_NAMELEN];
} Skfs_modman_return_lookup_t;

typedef struct {
	SKFS_TYPE;
	int type;
	uint32_t id;
	char use_name[SKFS_MAX_NAMELEN];
} Skfs_modman_return_lookup_user_t;

typedef struct {
	SKFS_TYPE;
	int type;
} Skfs_modman_request_its_t;

typedef struct {
	SKFS_TYPE;
	int type;
	uint32_t id;
} Skfs_modman_return_it_t;


#endif // __KUDOS_INC_SERIAL_KFS_H