#ifndef __KUDOS_INC_SERIAL_KFS_H
#define __KUDOS_INC_SERIAL_KFS_H

#include <inc/types.h>
#include <inc/mmu.h>

#define SKFS_VAL 2

// Destructors

#define SKFS_DESTROY_CFS 1
#define SKFS_DESTROY_LFS 2
#define SKFS_DESTROY_BD  3

// OBJ

#define SKFS_REQUEST_FLAGS_MAGIC 4
#define SKFS_RETURN_FLAGS_MAGIC  5
#define SKFS_REQUEST_CONFIG_STATUS 6
#define SKFS_RETURN_CONFIG_STATUS  7

// CFS

#define SKFS_TABLE_CLASSIFIER_CFS 8
#define SKFS_TABLE_CLASSIFIER_CFS_ADD 9
#define SKFS_TABLE_CLASSIFIER_CFS_REMOVE 10

#define SKFS_UHFS 11

// LFS

#define SKFS_JOURNAL_LFS 12
#define SKFS_JOURNAL_LFS_MAX_BANDWIDTH 13

#define SKFS_JOSFS_BASE 14
#define SKFS_JOSFS_FSCK 15

#define SKFS_WHOLEDISK 16

// BD

#define SKFS_LOOP_BD 17
#define SKFS_NBD_BD 18
#define SKFS_MEM_BD 19
#define SKFS_JOURNAL_QUEUE_BD 20
#define SKFS_ORDER_PRESERVER_BD 21
#define SKFS_WB_CACHE_BD 22
#define SKFS_WT_CACHE_BD 23
#define SKFS_BLOCK_RESIZER_BD 24
#define SKFS_MD_BD 25
#define SKFS_MIRROR_BD 26
#define SKFS_MIRROR_BD_ADD 27
#define SKFS_MIRROR_BD_REMOVE 28
#define SKFS_PARTITION_BD 29
#define SKFS_PC_PTABLE_BD 30
#define SKFS_IDE_PIO_BD 31

// modman

#define SKFS_MODMAN_REQUEST_LOOKUP 32
#define SKFS_MODMAN_RETURN_LOOKUP  33
#define SKFS_MODMAN_RETURN_LOOKUP_USER 34
#define SKFS_MODMAN_REQUEST_ITS    35
#define SKFS_MODMAN_RETURN_IT      36

#define SKFS_PERF_TEST 37


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
// OBJ

typedef struct {
	SKFS_TYPE;
	uint32_t id;
} Skfs_request_flags_magic_t;

typedef struct {
	SKFS_TYPE;
	uint32_t id;
	uint32_t flags, magic;
} Skfs_return_flags_magic_t;

typedef struct {
	SKFS_TYPE;
	uint32_t id;
	int level;
	bool config_status; // 0 config, 1 status
} Skfs_request_config_status_t;

typedef struct {
	SKFS_TYPE;
	uint32_t id;
	int level;
	bool config_status; // 0 config, 1 status
	char string[SKFS_MAX_NAMELEN];
} Skfs_return_config_status_t;


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
} Skfs_josfs_base_t;

typedef struct {
	SKFS_TYPE;
	uint32_t lfs;
} Skfs_josfs_fsck_t;

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
	uint32_t blocks;
	uint16_t blocksize;
} Skfs_mem_bd_t;

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

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
	uint32_t newdevice;
} Skfs_mirror_bd_add_t;

typedef struct {
	SKFS_TYPE;
	uint32_t bd;
	int diskno;
} Skfs_mirror_bd_remove_t;

// TODO: partition_bd
// TODO: pc_ptable_bd

typedef struct {
	SKFS_TYPE;
	uint8_t controller;
	uint8_t disk;
	uint8_t readahead;
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


//
// perf testing

typedef struct {
	SKFS_TYPE;
	int cfs_bd; // 0 CFS, 1 BD
	int size;
	char file[100];
} Skfs_perf_test_t;

#endif // __KUDOS_INC_SERIAL_KFS_H
