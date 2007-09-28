/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef FSTITCH_FSCORE_DEBUG_OPCODE_H
#define FSTITCH_FSCORE_DEBUG_OPCODE_H

#define DEBUG_SIG_MAGIC 0x40464442

/* modules */
enum kdb_debug_module {
	FDB_MODULE_INFO =                  1,
	FDB_MODULE_BDESC =               100,
	FDB_MODULE_PATCH_ALTER =         200,
	FDB_MODULE_PATCH_INFO =          300,
	FDB_MODULE_CACHE =               400
};

/* opcodes */
enum kdb_debug_opcode {
	/* info (0xx) */
	FDB_INFO_MARK =                    0,
	FDB_INFO_BD_NAME =                 1,
	FDB_INFO_BDESC_NUMBER =            2,
	FDB_INFO_PATCH_LABEL =             3,
	
	/* bdesc (1xx) */
	FDB_BDESC_ALLOC =                101,
	FDB_BDESC_RETAIN =               102,
	FDB_BDESC_RELEASE =              103,
	FDB_BDESC_DESTROY =              104,
	FDB_BDESC_FREE_DDESC =           105,
	FDB_BDESC_AUTORELEASE =          106,
	FDB_BDESC_AR_RESET =             107,
	FDB_BDESC_AR_POOL_PUSH =         108,
	FDB_BDESC_AR_POOL_POP =          109,
	
	/* patch alter (2xx) */
	FDB_PATCH_CREATE_EMPTY =         201,
	FDB_PATCH_CREATE_BIT =           202,
	FDB_PATCH_CREATE_BYTE =          203,
	FDB_PATCH_CONVERT_EMPTY =        204,
	FDB_PATCH_CONVERT_BIT =          205,
	FDB_PATCH_CONVERT_BYTE =         206,
	FDB_PATCH_REWRITE_BYTE =         207,
	FDB_PATCH_APPLY =                208,
	FDB_PATCH_ROLLBACK =             209,
	FDB_PATCH_SET_FLAGS =            210,
	FDB_PATCH_CLEAR_FLAGS =          211,
	FDB_PATCH_DESTROY =              212,
	FDB_PATCH_ADD_BEFORE =           213,
	FDB_PATCH_ADD_AFTER =            214,
	FDB_PATCH_REM_BEFORE =           215,
	FDB_PATCH_REM_AFTER =            216,
	FDB_PATCH_WEAK_RETAIN =          217,
	FDB_PATCH_WEAK_FORGET =          218,
	FDB_PATCH_SET_OFFSET =           219,
	FDB_PATCH_SET_XOR =              220,
	FDB_PATCH_SET_LENGTH =           221,
	FDB_PATCH_SET_BLOCK =            222,
	FDB_PATCH_SET_OWNER =            223,
	FDB_PATCH_SET_FREE_PREV =        224,
	FDB_PATCH_SET_FREE_NEXT =        225,
	FDB_PATCH_SET_FREE_HEAD =        226,
	
	/* patch info (3xx) */
	FDB_PATCH_SATISFY =              301,
	FDB_PATCH_WEAK_COLLECT =         302,
	FDB_PATCH_OVERLAP_ATTACH =       303,
	FDB_PATCH_OVERLAP_MULTIATTACH =  304,
	
	/* cache (4xx) */
	FDB_CACHE_NOTIFY =               401,
	FDB_CACHE_FINDBLOCK =            402,
	FDB_CACHE_LOOKBLOCK =            403,
	FDB_CACHE_WRITEBLOCK =           404
};

#ifdef WANT_DEBUG_STRUCTURES

/* structure definitions */
struct param {
	const char * name;
	/* keep this in sync with the array below */
	enum {
		STRING = 0,
		FORMAT, /* printf-style format string */
		INT32,
		UINT32,
		UHEX32,
		INT16,
		UINT16,
		UHEX16,
		BOOL
	} type;
};

struct opcode {
	enum kdb_debug_opcode opcode;
	const char * name;
	const struct param ** params;
};

struct module {
	enum kdb_debug_module module;
	const struct opcode ** opcodes;
};

/* data declarations */

/* keep this in sync with the enum above */
const uint8_t type_sizes[] = {-1, -1, 4, 4, 4, 2, 2, 2, 1};

/* all parameters */
static const struct param
	param_ar_count =    {"ar_count",    UINT32},
	param_bd =          {"bd",          UHEX32},
	param_block =       {"block",       UHEX32},
	param_blocks =      {"blocks",      UHEX32},
	param_cache =       {"cache",       UHEX32},
	param_patch =       {"patch",       UHEX32},
	param_patches =     {"patches",     UHEX32},
	param_count =       {"count",       UINT32},
	param_ddesc =       {"ddesc",       UHEX32},
	param_depth =       {"depth",       UINT32},
	param_flags =       {"flags",       UHEX32},
	param_flags16 =     {"flags16",     UHEX16},
	param_free_next =   {"free_next",   UHEX32},
	param_free_prev =   {"free_prev",   UHEX32},
	param_head =        {"head",        UHEX32},
	param_label =       {"label",       FORMAT},
	param_length =      {"length",      UINT16},
	param_location =    {"location",    UHEX32},
	param_module =      {"module",      UHEX16},
	param_name =        {"name",        STRING},
	param_number =      {"number",      UINT32},
	param_offset =      {"offset",      UINT16},
	param_order =       {"order",       UHEX32},
	param_original =    {"original",    UHEX32},
	param_owner =       {"owner",       UHEX32},
	param_recent =      {"recent",      UHEX32},
	param_ref_count =   {"ref_count",   UINT32},
	param_source =      {"source",      UHEX32},
	param_target =      {"target",      UHEX32},
	param_xor =         {"xor",         UHEX32},
	last_param = {NULL, 0};

/* parameter combinations */
static const struct param * params_info_mark[] = {
	&param_module,
	&last_param
};
static const struct param * params_info_bd_name[] = {
	&param_bd,
	&param_name,
	&last_param
};
static const struct param * params_info_bdesc_number[] = {
	&param_block,
	&param_number,
	&param_count, /* technically it's 16-bit here */
	&last_param
};
static const struct param * params_info_patch_label[] = {
	&param_patch,
	&param_label,
	&last_param
};
static const struct param * params_bdesc_alloc[] = {
	&param_block,
	&param_ddesc,
	&param_number,
	&param_count, /* technically it's 16-bit here */
	&last_param
};
static const struct param * params_bdesc_retain_release[] = {
	&param_block,
	&param_ddesc,
	&param_ref_count,
	&param_ar_count,
	&last_param
};
static const struct param * params_bdesc_destroy[] = {
	&param_block,
	&param_ddesc,
	&last_param
};
static const struct param * params_bdesc_free_ddesc[] = {
	&param_block,
	&param_ddesc,
	&last_param
};
static const struct param * params_bdesc_ar_push_pop[] = {
	&param_depth,
	&last_param
};
static const struct param * params_patch_create_empty[] = {
	&param_patch,
	&param_owner,
	&last_param
};
static const struct param * params_patch_create_bit[] = {
	&param_patch,
	&param_block,
	&param_owner,
	&param_offset,
	&param_xor,
	&last_param
};
static const struct param * params_patch_create_byte[] = {
	&param_patch,
	&param_block,
	&param_owner,
	&param_offset,
	&param_length,
	&last_param
};
static const struct param * params_patch_convert_bit[] = {
	&param_patch,
	&param_offset,
	&param_xor,
	&last_param
};
static const struct param * params_patch_convert_byte[] = {
	&param_patch,
	&param_offset,
	&param_length,
	&last_param
};
static const struct param * params_patch_connect[] = {
	&param_source,
	&param_target,
	&last_param
};
static const struct param * params_patch_flags[] = {
	&param_patch,
	&param_flags,
	&last_param
};
static const struct param * params_patch_only[] = {
	&param_patch,
	&last_param
};
static const struct param * params_patch_weak_retain_release[] = {
	&param_patch,
	&param_location,
	&last_param
};
static const struct param * params_patch_set_offset[] = {
	&param_patch,
	&param_offset,
	&last_param
};
static const struct param * params_patch_set_block[] = {
	&param_patch,
	&param_block,
	&last_param
};
static const struct param * params_patch_set_owner[] = {
	&param_patch,
	&param_owner,
	&last_param
};
static const struct param * params_patch_set_free_prev[] = {
	&param_patch,
	&param_free_prev,
	&last_param
};
static const struct param * params_patch_set_free_next[] = {
	&param_patch,
	&param_free_next,
	&last_param
};
static const struct param * params_patch_set_xor[] = {
	&param_patch,
	&param_xor,
	&last_param
};
static const struct param * params_patch_set_length[] = {
	&param_patch,
	&param_length,
	&last_param
};
static const struct param * params_patch_overlap_attach[] = {
	&param_recent,
	&param_original,
	&last_param
};
static const struct param * params_patch_overlap_multiattach[] = {
	&param_patch,
	&param_block,
	&last_param
};
static const struct param * params_cache_only[] = {
	&param_cache,
	&last_param
};
static const struct param * params_cache_block[] = {
	&param_cache,
	&param_block,
	&last_param
};
static const struct param * params_cache_block_flags[] = {
	&param_cache,
	&param_block,
	&param_flags16,
	&last_param
};

#define OPCODE(number, params) {number, #number, params}

/* all opcodes */
static const struct opcode
	opcode_info_mark =                  OPCODE(FDB_INFO_MARK,                  params_info_mark),
	opcode_info_bd_name =               OPCODE(FDB_INFO_BD_NAME,               params_info_bd_name),
	opcode_info_bdesc_number =          OPCODE(FDB_INFO_BDESC_NUMBER,          params_info_bdesc_number),
	opcode_info_patch_label =           OPCODE(FDB_INFO_PATCH_LABEL,           params_info_patch_label),
	opcode_bdesc_alloc =                OPCODE(FDB_BDESC_ALLOC,                params_bdesc_alloc),
	opcode_bdesc_retain =               OPCODE(FDB_BDESC_RETAIN,               params_bdesc_retain_release),
	opcode_bdesc_release =              OPCODE(FDB_BDESC_RELEASE,              params_bdesc_retain_release),
	opcode_bdesc_destroy =              OPCODE(FDB_BDESC_DESTROY,              params_bdesc_destroy),
	opcode_bdesc_free_ddesc =           OPCODE(FDB_BDESC_FREE_DDESC,           params_bdesc_free_ddesc),
	opcode_bdesc_autorelease =          OPCODE(FDB_BDESC_AUTORELEASE,          params_bdesc_retain_release),
	opcode_bdesc_ar_reset =             OPCODE(FDB_BDESC_AR_RESET,             params_bdesc_retain_release),
	opcode_bdesc_ar_pool_push =         OPCODE(FDB_BDESC_AR_POOL_PUSH,         params_bdesc_ar_push_pop),
	opcode_bdesc_ar_pool_pop =          OPCODE(FDB_BDESC_AR_POOL_POP,          params_bdesc_ar_push_pop),
	opcode_patch_create_empty =         OPCODE(FDB_PATCH_CREATE_EMPTY,         params_patch_create_empty),
	opcode_patch_create_bit =           OPCODE(FDB_PATCH_CREATE_BIT,           params_patch_create_bit),
	opcode_patch_create_byte =          OPCODE(FDB_PATCH_CREATE_BYTE,          params_patch_create_byte),
	opcode_patch_convert_empty =        OPCODE(FDB_PATCH_CONVERT_EMPTY,        params_patch_only),
	opcode_patch_convert_bit =          OPCODE(FDB_PATCH_CONVERT_BIT,          params_patch_convert_bit),
	opcode_patch_convert_byte =         OPCODE(FDB_PATCH_CONVERT_BYTE,         params_patch_convert_byte),
	opcode_patch_rewrite_byte =         OPCODE(FDB_PATCH_REWRITE_BYTE,         params_patch_only),
	opcode_patch_apply =                OPCODE(FDB_PATCH_APPLY,                params_patch_only),
	opcode_patch_rollback =             OPCODE(FDB_PATCH_ROLLBACK,             params_patch_only),
	opcode_patch_set_flags =            OPCODE(FDB_PATCH_SET_FLAGS,            params_patch_flags),
	opcode_patch_clear_flags =          OPCODE(FDB_PATCH_CLEAR_FLAGS,          params_patch_flags),
	opcode_patch_destroy =              OPCODE(FDB_PATCH_DESTROY,              params_patch_only),
	opcode_patch_add_before =           OPCODE(FDB_PATCH_ADD_BEFORE,           params_patch_connect),
	opcode_patch_add_after =            OPCODE(FDB_PATCH_ADD_AFTER,            params_patch_connect),
	opcode_patch_rem_before =           OPCODE(FDB_PATCH_REM_BEFORE,           params_patch_connect),
	opcode_patch_rem_after =            OPCODE(FDB_PATCH_REM_AFTER,            params_patch_connect),
	opcode_patch_weak_retain =          OPCODE(FDB_PATCH_WEAK_RETAIN,          params_patch_weak_retain_release),
	opcode_patch_weak_forget =          OPCODE(FDB_PATCH_WEAK_FORGET,          params_patch_weak_retain_release),
	opcode_patch_set_offset =           OPCODE(FDB_PATCH_SET_OFFSET,           params_patch_set_offset),
	opcode_patch_set_xor =              OPCODE(FDB_PATCH_SET_XOR,              params_patch_set_xor),
	opcode_patch_set_length =           OPCODE(FDB_PATCH_SET_LENGTH,           params_patch_set_length),
	opcode_patch_set_block =            OPCODE(FDB_PATCH_SET_BLOCK,            params_patch_set_block),
	opcode_patch_set_owner =            OPCODE(FDB_PATCH_SET_OWNER,            params_patch_set_owner),
	opcode_patch_set_free_prev =        OPCODE(FDB_PATCH_SET_FREE_PREV,        params_patch_set_free_prev),
	opcode_patch_set_free_next =        OPCODE(FDB_PATCH_SET_FREE_NEXT,        params_patch_set_free_next),
	opcode_patch_set_free_head =        OPCODE(FDB_PATCH_SET_FREE_HEAD,        params_patch_only),
	opcode_patch_satisfy =              OPCODE(FDB_PATCH_SATISFY,              params_patch_only),
	opcode_patch_weak_collect =         OPCODE(FDB_PATCH_WEAK_COLLECT,         params_patch_only),
	opcode_patch_overlap_attach =       OPCODE(FDB_PATCH_OVERLAP_ATTACH,       params_patch_overlap_attach),
	opcode_patch_overlap_multiattach =  OPCODE(FDB_PATCH_OVERLAP_MULTIATTACH,  params_patch_overlap_multiattach),
	opcode_cache_notify =               OPCODE(FDB_CACHE_NOTIFY,               params_cache_only),
	opcode_cache_findblock =            OPCODE(FDB_CACHE_FINDBLOCK,            params_cache_only),
	opcode_cache_lookblock =            OPCODE(FDB_CACHE_LOOKBLOCK,            params_cache_block),
	opcode_cache_writeblock =           OPCODE(FDB_CACHE_WRITEBLOCK,           params_cache_block_flags),
	last_opcode = {0, NULL, NULL};

/* opcode combinations */
static const struct opcode * opcodes_info[] = {
	&opcode_info_mark,
	&opcode_info_bd_name,
	&opcode_info_bdesc_number,
	&opcode_info_patch_label,
	&last_opcode
};
static const struct opcode * opcodes_bdesc[] = {
	&opcode_bdesc_alloc,
	&opcode_bdesc_retain,
	&opcode_bdesc_release,
	&opcode_bdesc_destroy,
	&opcode_bdesc_free_ddesc,
	&opcode_bdesc_autorelease,
	&opcode_bdesc_ar_reset,
	&opcode_bdesc_ar_pool_push,
	&opcode_bdesc_ar_pool_pop,
	&last_opcode
};
static const struct opcode * opcodes_patch_alter[] = {
	&opcode_patch_create_empty,
	&opcode_patch_create_bit,
	&opcode_patch_create_byte,
	&opcode_patch_convert_empty,
	&opcode_patch_convert_bit,
	&opcode_patch_convert_byte,
	&opcode_patch_rewrite_byte,
	&opcode_patch_apply,
	&opcode_patch_rollback,
	&opcode_patch_set_flags,
	&opcode_patch_clear_flags,
	&opcode_patch_destroy,
	&opcode_patch_add_before,
	&opcode_patch_add_after,
	&opcode_patch_rem_before,
	&opcode_patch_rem_after,
	&opcode_patch_weak_retain,
	&opcode_patch_weak_forget,
	&opcode_patch_set_offset,
	&opcode_patch_set_xor,
	&opcode_patch_set_length,
	&opcode_patch_set_block,
	&opcode_patch_set_owner,
	&opcode_patch_set_free_prev,
	&opcode_patch_set_free_next,
	&opcode_patch_set_free_head,
	&last_opcode
};
static const struct opcode * opcodes_patch_info[] = {
	&opcode_patch_satisfy,
	&opcode_patch_weak_collect,
	&opcode_patch_overlap_attach,
	&opcode_patch_overlap_multiattach,
	&last_opcode
};
static const struct opcode * opcodes_cache[] = {
	&opcode_cache_notify,
	&opcode_cache_findblock,
	&opcode_cache_lookblock,
	&opcode_cache_writeblock,
	&last_opcode
};

/* modules */
static const struct module modules[] = {
	{FDB_MODULE_INFO, opcodes_info},
	{FDB_MODULE_BDESC, opcodes_bdesc},
	{FDB_MODULE_PATCH_ALTER, opcodes_patch_alter},
	{FDB_MODULE_PATCH_INFO, opcodes_patch_info},
	{FDB_MODULE_CACHE, opcodes_cache},
	{0, NULL}
};

#endif /* WANT_DEBUG_STRUCTURES */

#endif /* FSTITCH_FSCORE_DEBUG_OPCODE_H */
