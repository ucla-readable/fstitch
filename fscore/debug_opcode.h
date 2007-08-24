#ifndef FSTITCH_FSCORE_DEBUG_OPCODE_H
#define FSTITCH_FSCORE_DEBUG_OPCODE_H

#define DEBUG_OPCODE_REV           "$Rev$"

/* modules */
enum kdb_debug_module {
	KDB_MODULE_INFO =                  1,
	KDB_MODULE_BDESC =               100,
	KDB_MODULE_PATCH_ALTER =        200,
	KDB_MODULE_PATCH_INFO =         300,
	KDB_MODULE_CACHE =               400
};

/* opcodes */
enum kdb_debug_opcode {
	/* info (0xx) */
	KDB_INFO_MARK =                    0,
	KDB_INFO_BD_NAME =                 1,
	KDB_INFO_BDESC_NUMBER =            2,
	KDB_INFO_PATCH_LABEL =            3,
	
	/* bdesc (1xx) */
	KDB_BDESC_ALLOC =                101,
	KDB_BDESC_RETAIN =               102,
	KDB_BDESC_RELEASE =              103,
	KDB_BDESC_DESTROY =              104,
	KDB_BDESC_FREE_DDESC =           105,
	KDB_BDESC_AUTORELEASE =          106,
	KDB_BDESC_AR_RESET =             107,
	KDB_BDESC_AR_POOL_PUSH =         108,
	KDB_BDESC_AR_POOL_POP =          109,
	
	/* patch alter (2xx) */
	KDB_PATCH_CREATE_EMPTY =         201,
	KDB_PATCH_CREATE_BIT =          202,
	KDB_PATCH_CREATE_BYTE =         203,
	KDB_PATCH_CONVERT_EMPTY =        204,
	KDB_PATCH_CONVERT_BIT =         205,
	KDB_PATCH_CONVERT_BYTE =        206,
	KDB_PATCH_REWRITE_BYTE =        207,
	KDB_PATCH_APPLY =               208,
	KDB_PATCH_ROLLBACK =            209,
	KDB_PATCH_SET_FLAGS =           210,
	KDB_PATCH_CLEAR_FLAGS =         211,
	KDB_PATCH_DESTROY =             212,
	KDB_PATCH_ADD_BEFORE =          213,
	KDB_PATCH_ADD_AFTER =           214,
	KDB_PATCH_REM_BEFORE =          215,
	KDB_PATCH_REM_AFTER =           216,
	KDB_PATCH_WEAK_RETAIN =         217,
	KDB_PATCH_WEAK_FORGET =         218,
	KDB_PATCH_SET_OFFSET =          219,
	KDB_PATCH_SET_XOR =             220,
	KDB_PATCH_SET_LENGTH =          221,
	KDB_PATCH_SET_BLOCK =           222,
	KDB_PATCH_SET_OWNER =           223,
	KDB_PATCH_SET_FREE_PREV =       224,
	KDB_PATCH_SET_FREE_NEXT =       225,
	KDB_PATCH_SET_FREE_HEAD =       226,
	
	/* patch info (3xx) */
	KDB_PATCH_SATISFY =             301,
	KDB_PATCH_WEAK_COLLECT =        302,
	KDB_PATCH_OVERLAP_ATTACH =      303,
	KDB_PATCH_OVERLAP_MULTIATTACH = 304,
	
	/* cache (4xx) */
	KDB_CACHE_NOTIFY =               401,
	KDB_CACHE_FINDBLOCK =            402,
	KDB_CACHE_LOOKBLOCK =            403,
	KDB_CACHE_WRITEBLOCK =           404
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
	param_patch =      {"patch",      UHEX32},
	param_patchs =     {"patchs",     UHEX32},
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
	opcode_info_mark =                  OPCODE(KDB_INFO_MARK,                  params_info_mark),
	opcode_info_bd_name =               OPCODE(KDB_INFO_BD_NAME,               params_info_bd_name),
	opcode_info_bdesc_number =          OPCODE(KDB_INFO_BDESC_NUMBER,          params_info_bdesc_number),
	opcode_info_patch_label =          OPCODE(KDB_INFO_PATCH_LABEL,          params_info_patch_label),
	opcode_bdesc_alloc =                OPCODE(KDB_BDESC_ALLOC,                params_bdesc_alloc),
	opcode_bdesc_retain =               OPCODE(KDB_BDESC_RETAIN,               params_bdesc_retain_release),
	opcode_bdesc_release =              OPCODE(KDB_BDESC_RELEASE,              params_bdesc_retain_release),
	opcode_bdesc_destroy =              OPCODE(KDB_BDESC_DESTROY,              params_bdesc_destroy),
	opcode_bdesc_free_ddesc =           OPCODE(KDB_BDESC_FREE_DDESC,           params_bdesc_free_ddesc),
	opcode_bdesc_autorelease =          OPCODE(KDB_BDESC_AUTORELEASE,          params_bdesc_retain_release),
	opcode_bdesc_ar_reset =             OPCODE(KDB_BDESC_AR_RESET,             params_bdesc_retain_release),
	opcode_bdesc_ar_pool_push =         OPCODE(KDB_BDESC_AR_POOL_PUSH,         params_bdesc_ar_push_pop),
	opcode_bdesc_ar_pool_pop =          OPCODE(KDB_BDESC_AR_POOL_POP,          params_bdesc_ar_push_pop),
	opcode_patch_create_empty =         OPCODE(KDB_PATCH_CREATE_EMPTY,         params_patch_create_empty),
	opcode_patch_create_bit =          OPCODE(KDB_PATCH_CREATE_BIT,          params_patch_create_bit),
	opcode_patch_create_byte =         OPCODE(KDB_PATCH_CREATE_BYTE,         params_patch_create_byte),
	opcode_patch_convert_empty =        OPCODE(KDB_PATCH_CONVERT_EMPTY,        params_patch_only),
	opcode_patch_convert_bit =         OPCODE(KDB_PATCH_CONVERT_BIT,         params_patch_convert_bit),
	opcode_patch_convert_byte =        OPCODE(KDB_PATCH_CONVERT_BYTE,        params_patch_convert_byte),
	opcode_patch_rewrite_byte =        OPCODE(KDB_PATCH_REWRITE_BYTE,        params_patch_only),
	opcode_patch_apply =               OPCODE(KDB_PATCH_APPLY,               params_patch_only),
	opcode_patch_rollback =            OPCODE(KDB_PATCH_ROLLBACK,            params_patch_only),
	opcode_patch_set_flags =           OPCODE(KDB_PATCH_SET_FLAGS,           params_patch_flags),
	opcode_patch_clear_flags =         OPCODE(KDB_PATCH_CLEAR_FLAGS,         params_patch_flags),
	opcode_patch_destroy =             OPCODE(KDB_PATCH_DESTROY,             params_patch_only),
	opcode_patch_add_before =          OPCODE(KDB_PATCH_ADD_BEFORE,          params_patch_connect),
	opcode_patch_add_after =           OPCODE(KDB_PATCH_ADD_AFTER,           params_patch_connect),
	opcode_patch_rem_before =          OPCODE(KDB_PATCH_REM_BEFORE,          params_patch_connect),
	opcode_patch_rem_after =           OPCODE(KDB_PATCH_REM_AFTER,           params_patch_connect),
	opcode_patch_weak_retain =         OPCODE(KDB_PATCH_WEAK_RETAIN,         params_patch_weak_retain_release),
	opcode_patch_weak_forget =         OPCODE(KDB_PATCH_WEAK_FORGET,         params_patch_weak_retain_release),
	opcode_patch_set_offset =          OPCODE(KDB_PATCH_SET_OFFSET,          params_patch_set_offset),
	opcode_patch_set_xor =             OPCODE(KDB_PATCH_SET_XOR,             params_patch_set_xor),
	opcode_patch_set_length =          OPCODE(KDB_PATCH_SET_LENGTH,          params_patch_set_length),
	opcode_patch_set_block =           OPCODE(KDB_PATCH_SET_BLOCK,           params_patch_set_block),
	opcode_patch_set_owner =           OPCODE(KDB_PATCH_SET_OWNER,           params_patch_set_owner),
	opcode_patch_set_free_prev =       OPCODE(KDB_PATCH_SET_FREE_PREV,       params_patch_set_free_prev),
	opcode_patch_set_free_next =       OPCODE(KDB_PATCH_SET_FREE_NEXT,       params_patch_set_free_next),
	opcode_patch_set_free_head =       OPCODE(KDB_PATCH_SET_FREE_HEAD,       params_patch_only),
	opcode_patch_satisfy =             OPCODE(KDB_PATCH_SATISFY,             params_patch_only),
	opcode_patch_weak_collect =        OPCODE(KDB_PATCH_WEAK_COLLECT,        params_patch_only),
	opcode_patch_overlap_attach =      OPCODE(KDB_PATCH_OVERLAP_ATTACH,      params_patch_overlap_attach),
	opcode_patch_overlap_multiattach = OPCODE(KDB_PATCH_OVERLAP_MULTIATTACH, params_patch_overlap_multiattach),
	opcode_cache_notify =               OPCODE(KDB_CACHE_NOTIFY,               params_cache_only),
	opcode_cache_findblock =            OPCODE(KDB_CACHE_FINDBLOCK,            params_cache_only),
	opcode_cache_lookblock =            OPCODE(KDB_CACHE_LOOKBLOCK,            params_cache_block),
	opcode_cache_writeblock =           OPCODE(KDB_CACHE_WRITEBLOCK,           params_cache_block_flags),
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
	{KDB_MODULE_INFO, opcodes_info},
	{KDB_MODULE_BDESC, opcodes_bdesc},
	{KDB_MODULE_PATCH_ALTER, opcodes_patch_alter},
	{KDB_MODULE_PATCH_INFO, opcodes_patch_info},
	{KDB_MODULE_CACHE, opcodes_cache},
	{0, NULL}
};

#endif /* WANT_DEBUG_STRUCTURES */

#endif /* FSTITCH_FSCORE_DEBUG_OPCODE_H */
