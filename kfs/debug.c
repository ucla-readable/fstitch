#include <lib/assert.h>
#include <lib/jiffies.h>
#include <lib/sleep.h>
#include <lib/types.h>
#include <inc/error.h>
#include <lib/stdarg.h>
#include <lib/stdio.h>
#include <lib/svnrevtol.h>

#ifdef __KERNEL__
#include <linux/config.h>
#else
#include <lib/netclient.h>
#endif

/* htons and htonl */
#include <lib/string.h>
#if defined(KUDOS)
#include <inc/josnic.h>
#elif defined(UNIXUSER)
#include <netinet/in.h>
#endif

#include <kfs/chdesc.h>
#include <kfs/sched.h>
#include <kfs/debug.h>
#include <kfs/kfsd.h>

#if KFS_DEBUG

/* For a lean and mean debug output stream, set all of these to 1. */
#define KFS_DEBUG_BINARY 1
#define KFS_OMIT_FILE_FUNC 0
#define KFS_OMIT_BTRACE 0

#if !KFS_DEBUG_BINARY && defined(__KERNEL__)
#warning No kernel support for text debugging; using binary debugging
#undef KFS_DEBUG_BINARY
#define KFS_DEBUG_BINARY 1
#endif

#if !KFS_OMIT_BTRACE && !defined(__i386__)
#warning Debug backtraces only available on x86 platforms; disabling
#undef KFS_OMIT_BTRACE
#define KFS_OMIT_BTRACE 1
#endif

#if !KFS_OMIT_BTRACE && defined(__KERNEL__) && !defined(CONFIG_FRAME_POINTER)
#warning In the kernel, frame pointers are required for backtraces; disabling
#undef KFS_OMIT_BTRACE
#define KFS_OMIT_BTRACE 1
#endif

/* structure definitions */
struct param {
	const char * name;
	/* keep this in sync with the array below */
	enum {
		STRING = 0,
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
	uint16_t opcode;
	const char * name;
	const struct param ** params;
};

struct module {
	uint16_t module;
	const struct opcode ** opcodes;
};

/* data declarations */

/* keep this in sync with the enum above */
const uint8_t type_sizes[] = {-1, 4, 4, 4, 2, 2, 2, 1};

/* all parameters */
static const struct param
	param_ar_count =    {"ar_count",    UINT32},
	param_bd =          {"bd",          UHEX32},
	param_block =       {"block",       UHEX32},
	param_blocks =      {"blocks",      UHEX32},
	param_chdesc =      {"chdesc",      UHEX32},
	param_chdescs =     {"chdescs",     UHEX32},
	param_count =       {"count",       UINT32},
	param_dd_count =    {"dd_count",    UINT32},
	param_ddesc =       {"ddesc",       UHEX32},
	param_depth =       {"depth",       UINT32},
	param_destination = {"destination", UHEX32},
	param_flags =       {"flags",       UHEX32},
	param_free_next =   {"free_next",   UHEX32},
	param_free_prev =   {"free_prev",   UHEX32},
	param_head =        {"head",        UHEX32},
	param_label =       {"label",       STRING},
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
static const struct param * params_info_chdesc_label[] = {
	&param_chdesc,
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
	&param_dd_count,
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
static const struct param * params_chdesc_create_noop[] = {
	&param_chdesc,
	&param_block,
	&param_owner,
	&last_param
};
static const struct param * params_chdesc_create_bit[] = {
	&param_chdesc,
	&param_block,
	&param_owner,
	&param_offset,
	&param_xor,
	&last_param
};
static const struct param * params_chdesc_create_byte[] = {
	&param_chdesc,
	&param_block,
	&param_owner,
	&param_offset,
	&param_length,
	&last_param
};
static const struct param * params_chdesc_convert_bit[] = {
	&param_chdesc,
	&param_offset,
	&param_xor,
	&last_param
};
static const struct param * params_chdesc_convert_byte[] = {
	&param_chdesc,
	&param_offset,
	&param_length,
	&last_param
};
static const struct param * params_chdesc_connect[] = {
	&param_source,
	&param_target,
	&last_param
};
static const struct param * params_chdesc_flags[] = {
	&param_chdesc,
	&param_flags,
	&last_param
};
static const struct param * params_chdesc_only[] = {
	&param_chdesc,
	&last_param
};
static const struct param * params_chdesc_weak_retain_release[] = {
	&param_chdesc,
	&param_location,
	&last_param
};
static const struct param * params_chdesc_set_offset[] = {
	&param_chdesc,
	&param_offset,
	&last_param
};
static const struct param * params_chdesc_set_block[] = {
	&param_chdesc,
	&param_block,
	&last_param
};
static const struct param * params_chdesc_set_owner[] = {
	&param_chdesc,
	&param_owner,
	&last_param
};
static const struct param * params_chdesc_set_free_prev[] = {
	&param_chdesc,
	&param_free_prev,
	&last_param
};
static const struct param * params_chdesc_set_free_next[] = {
	&param_chdesc,
	&param_free_next,
	&last_param
};
static const struct param * params_chdesc_set_length[] = {
	&param_chdesc,
	&param_length,
	&last_param
};
static const struct param * params_chdesc_move[] = {
	&param_chdesc,
	&param_destination,
	&param_target,
	&param_offset,
	&last_param
};
static const struct param * params_chdesc_collection[] = {
	&param_count,
	&param_chdescs,
	&param_order,
	&last_param
};
static const struct param * params_chdesc_order_destroy[] = {
	&param_order,
	&last_param
};
static const struct param * params_chdesc_overlap_attach[] = {
	&param_recent,
	&param_original,
	&last_param
};
static const struct param * params_chdesc_overlap_multiattach[] = {
	&param_chdesc,
	&param_block,
	&last_param
};
static const struct param * params_chdesc_duplicate[] = {
	&param_original,
	&param_count,
	&param_blocks,
	&last_param
};
static const struct param * params_chdesc_split[] = {
	&param_original,
	&param_count,
	&last_param
};
static const struct param * params_chdesc_merge[] = {
	&param_count,
	&param_chdescs,
	&param_head,
	&last_param
};

#define OPCODE(number, params) {number, #number, params}

/* all opcodes */
static const struct opcode
	opcode_info_mark =                  OPCODE(KDB_INFO_MARK,                  params_info_mark),
	opcode_info_bd_name =               OPCODE(KDB_INFO_BD_NAME,               params_info_bd_name),
	opcode_info_bdesc_number =          OPCODE(KDB_INFO_BDESC_NUMBER,          params_info_bdesc_number),
	opcode_info_chdesc_label =          OPCODE(KDB_INFO_CHDESC_LABEL,          params_info_chdesc_label),
	opcode_bdesc_alloc =                OPCODE(KDB_BDESC_ALLOC,                params_bdesc_alloc),
	opcode_bdesc_alloc_wrap =           OPCODE(KDB_BDESC_ALLOC_WRAP,           params_bdesc_alloc),
	opcode_bdesc_retain =               OPCODE(KDB_BDESC_RETAIN,               params_bdesc_retain_release),
	opcode_bdesc_release =              OPCODE(KDB_BDESC_RELEASE,              params_bdesc_retain_release),
	opcode_bdesc_destroy =              OPCODE(KDB_BDESC_DESTROY,              params_bdesc_destroy),
	opcode_bdesc_free_ddesc =           OPCODE(KDB_BDESC_FREE_DDESC,           params_bdesc_free_ddesc),
	opcode_bdesc_autorelease =          OPCODE(KDB_BDESC_AUTORELEASE,          params_bdesc_retain_release),
	opcode_bdesc_ar_reset =             OPCODE(KDB_BDESC_AR_RESET,             params_bdesc_retain_release),
	opcode_bdesc_ar_pool_push =         OPCODE(KDB_BDESC_AR_POOL_PUSH,         params_bdesc_ar_push_pop),
	opcode_bdesc_ar_pool_pop =          OPCODE(KDB_BDESC_AR_POOL_POP,          params_bdesc_ar_push_pop),
	opcode_chdesc_create_noop =         OPCODE(KDB_CHDESC_CREATE_NOOP,         params_chdesc_create_noop),
	opcode_chdesc_create_bit =          OPCODE(KDB_CHDESC_CREATE_BIT,          params_chdesc_create_bit),
	opcode_chdesc_create_byte =         OPCODE(KDB_CHDESC_CREATE_BYTE,         params_chdesc_create_byte),
	opcode_chdesc_convert_noop =        OPCODE(KDB_CHDESC_CONVERT_NOOP,        params_chdesc_only),
	opcode_chdesc_convert_bit =         OPCODE(KDB_CHDESC_CONVERT_BIT,         params_chdesc_convert_bit),
	opcode_chdesc_convert_byte =        OPCODE(KDB_CHDESC_CONVERT_BYTE,        params_chdesc_convert_byte),
	opcode_chdesc_rewrite_byte =        OPCODE(KDB_CHDESC_REWRITE_BYTE,        params_chdesc_only),
	opcode_chdesc_apply =               OPCODE(KDB_CHDESC_APPLY,               params_chdesc_only),
	opcode_chdesc_rollback =            OPCODE(KDB_CHDESC_ROLLBACK,            params_chdesc_only),
	opcode_chdesc_set_flags =           OPCODE(KDB_CHDESC_SET_FLAGS,           params_chdesc_flags),
	opcode_chdesc_clear_flags =         OPCODE(KDB_CHDESC_CLEAR_FLAGS,         params_chdesc_flags),
	opcode_chdesc_destroy =             OPCODE(KDB_CHDESC_DESTROY,             params_chdesc_only),
	opcode_chdesc_add_before =          OPCODE(KDB_CHDESC_ADD_BEFORE,          params_chdesc_connect),
	opcode_chdesc_add_after =           OPCODE(KDB_CHDESC_ADD_AFTER,           params_chdesc_connect),
	opcode_chdesc_rem_before =          OPCODE(KDB_CHDESC_REM_BEFORE,          params_chdesc_connect),
	opcode_chdesc_rem_after =           OPCODE(KDB_CHDESC_REM_AFTER,           params_chdesc_connect),
	opcode_chdesc_weak_retain =         OPCODE(KDB_CHDESC_WEAK_RETAIN,         params_chdesc_weak_retain_release),
	opcode_chdesc_weak_forget =         OPCODE(KDB_CHDESC_WEAK_FORGET,         params_chdesc_weak_retain_release),
	opcode_chdesc_set_offset =          OPCODE(KDB_CHDESC_SET_OFFSET,          params_chdesc_set_offset),
	opcode_chdesc_set_length =          OPCODE(KDB_CHDESC_SET_LENGTH,          params_chdesc_set_length),
	opcode_chdesc_set_block =           OPCODE(KDB_CHDESC_SET_BLOCK,           params_chdesc_set_block),
	opcode_chdesc_set_owner =           OPCODE(KDB_CHDESC_SET_OWNER,           params_chdesc_set_owner),
	opcode_chdesc_set_free_prev =       OPCODE(KDB_CHDESC_SET_FREE_PREV,       params_chdesc_set_free_prev),
	opcode_chdesc_set_free_next =       OPCODE(KDB_CHDESC_SET_FREE_NEXT,       params_chdesc_set_free_next),
	opcode_chdesc_set_free_head =       OPCODE(KDB_CHDESC_SET_FREE_HEAD,       params_chdesc_only),
	opcode_chdesc_move =                OPCODE(KDB_CHDESC_MOVE,                params_chdesc_move),
	opcode_chdesc_satisfy =             OPCODE(KDB_CHDESC_SATISFY,             params_chdesc_only),
	opcode_chdesc_weak_collect =        OPCODE(KDB_CHDESC_WEAK_COLLECT,        params_chdesc_only),
	opcode_chdesc_rollback_collection = OPCODE(KDB_CHDESC_ROLLBACK_COLLECTION, params_chdesc_collection),
	opcode_chdesc_apply_collection =    OPCODE(KDB_CHDESC_APPLY_COLLECTION,    params_chdesc_collection),
	opcode_chdesc_order_destroy =       OPCODE(KDB_CHDESC_ORDER_DESTROY,       params_chdesc_order_destroy),
	opcode_chdesc_detach_befores =      OPCODE(KDB_CHDESC_DETACH_BEFORES,      params_chdesc_only),
	opcode_chdesc_detach_afters =       OPCODE(KDB_CHDESC_DETACH_AFTERS,       params_chdesc_only),
	opcode_chdesc_overlap_attach =      OPCODE(KDB_CHDESC_OVERLAP_ATTACH,      params_chdesc_overlap_attach),
	opcode_chdesc_overlap_multiattach = OPCODE(KDB_CHDESC_OVERLAP_MULTIATTACH, params_chdesc_overlap_multiattach),
	opcode_chdesc_duplicate =           OPCODE(KDB_CHDESC_DUPLICATE,           params_chdesc_duplicate),
	opcode_chdesc_split =               OPCODE(KDB_CHDESC_SPLIT,               params_chdesc_split),
	opcode_chdesc_merge =               OPCODE(KDB_CHDESC_MERGE,               params_chdesc_merge),
	last_opcode = {0, NULL, NULL};

/* opcode combinations */
static const struct opcode * opcodes_info[] = {
	&opcode_info_mark,
	&opcode_info_bd_name,
	&opcode_info_bdesc_number,
	&opcode_info_chdesc_label,
	&last_opcode
};
static const struct opcode * opcodes_bdesc[] = {
	&opcode_bdesc_alloc,
	&opcode_bdesc_alloc_wrap,
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
static const struct opcode * opcodes_chdesc_alter[] = {
	&opcode_chdesc_create_noop,
	&opcode_chdesc_create_bit,
	&opcode_chdesc_create_byte,
	&opcode_chdesc_convert_noop,
	&opcode_chdesc_convert_bit,
	&opcode_chdesc_convert_byte,
	&opcode_chdesc_rewrite_byte,
	&opcode_chdesc_apply,
	&opcode_chdesc_rollback,
	&opcode_chdesc_set_flags,
	&opcode_chdesc_clear_flags,
	&opcode_chdesc_destroy,
	&opcode_chdesc_add_before,
	&opcode_chdesc_add_after,
	&opcode_chdesc_rem_before,
	&opcode_chdesc_rem_after,
	&opcode_chdesc_weak_retain,
	&opcode_chdesc_weak_forget,
	&opcode_chdesc_set_offset,
	&opcode_chdesc_set_length,
	&opcode_chdesc_set_block,
	&opcode_chdesc_set_owner,
	&opcode_chdesc_set_free_prev,
	&opcode_chdesc_set_free_next,
	&opcode_chdesc_set_free_head,
	&last_opcode
};
static const struct opcode * opcodes_chdesc_info[] = {
	&opcode_chdesc_move,
	&opcode_chdesc_satisfy,
	&opcode_chdesc_weak_collect,
	&opcode_chdesc_rollback_collection,
	&opcode_chdesc_apply_collection,
	&opcode_chdesc_order_destroy,
	&opcode_chdesc_detach_befores,
	&opcode_chdesc_detach_afters,
	&opcode_chdesc_overlap_attach,
	&opcode_chdesc_overlap_multiattach,
	&opcode_chdesc_duplicate,
	&opcode_chdesc_split,
	&opcode_chdesc_merge,
	&last_opcode
};

/* modules */
static const struct module modules[] = {
	{KDB_MODULE_INFO, opcodes_info},
	{KDB_MODULE_BDESC, opcodes_bdesc},
	{KDB_MODULE_CHDESC_ALTER, opcodes_chdesc_alter},
	{KDB_MODULE_CHDESC_INFO, opcodes_chdesc_info},
	{0, NULL}
};
static bool modules_ignore[sizeof(modules) / sizeof(modules[0])] = {0};
	
static int debug_socket = -1;
static int debug_count = 0;

#if KFS_DEBUG_BINARY

#define LIT_8 (-1)
#define LIT_16 (-2)
#define LIT_32 (-4)
#define LIT_STR (-3)
#define END 0


#if defined(__KERNEL__)

#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/vmalloc.h>

#define DEBUG_PROC_FILENAME "kkfsd_debug"
#define DEBUG_PROC_SIZE (4 * 1024 * 1024)

static uint8_t * proc_buffer;
static off_t proc_buffer_rpos;
static off_t proc_buffer_wpos;

static int kfs_debug_proc_read(char * page, char ** start, off_t off, int count, int * eof, void * data)
{
	off_t size;
	while(proc_buffer_rpos == proc_buffer_wpos)
	{
		// buffer is empty, wait for writes
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ / 50);
		if(signal_pending(current))
			return -E_INTR;
	}
	size = proc_buffer_wpos - proc_buffer_rpos;
	if(size > count)
		size = count;
	for(count = 0; count < size; count++)
		*(page++) = proc_buffer[proc_buffer_rpos++ % DEBUG_PROC_SIZE];
	*start = (char *) count;
	return count;
}

static int proc_buffer_write(void * data, size_t len)
{
	uint8_t * buf = data;
	size_t i;
	for(i = 0; i < len; i++)
	{
		while(proc_buffer_wpos >= proc_buffer_rpos + DEBUG_PROC_SIZE)
		{
			// buffer is full, wait for reads
			current->state = TASK_INTERRUPTIBLE;
			schedule_timeout(HZ / 50);
		}
		proc_buffer[proc_buffer_wpos++ % DEBUG_PROC_SIZE] = buf[i];
	}
	return len;
}

static void kfs_debug_shutdown(void * ignore)
{
	remove_proc_entry(DEBUG_PROC_FILENAME, &proc_root);	
	vfree(proc_buffer);
	proc_buffer = NULL;
}

/* override write() and ignore the file descriptor */
#define write(fd, data, size) proc_buffer_write(data, size)

#endif


/* This function is used like a binary version of kdprintf(). It takes a file
 * descriptor, and then a series of pairs of (size, pointer) of data to write to
 * it. The list is terminated by a 0 size. Also accepted are the special sizes
 * -1, -2, and -4, which indicate that the data is to be extracted from the
 * stack as a uint8_t, uint16_t, or uint32_t, respectively, and changed to
 * network byte order. Finally, the special size -3 means to write a
 * null-terminated string, whose size will be determined with strlen(). The
 * total number of bytes written is returned, or a negative value on error when
 * no bytes have been written. Note that an error may cause the number of bytes
 * written to be smaller than requested. */
static int kfs_debug_write(int fd, ...)
{
	int bytes = 0;
	va_list ap;
	va_start(ap, fd);
	
	for(;;)
	{
		int result, size = va_arg(ap, int);
		
		if(size > 0)
		{
			void * data = va_arg(ap, void *);
			result = write(fd, data, size);
		}
		else if(size < 0)
		{
			/* negative size means on stack */
			size = -size;
			if(size == 1)
			{
				uint8_t data = va_arg(ap, uint8_t);
				result = write(fd, &data, 1);
			}
			else if(size == 2)
			{
				uint16_t data = htons(va_arg(ap, uint16_t));
				result = write(fd, &data, 2);
			}
			else if(size == 4)
			{
				uint32_t data = htonl(va_arg(ap, uint32_t));
				result = write(fd, &data, 4);
			}
			else if(size == 3)
			{
				/* string */
				char * string = va_arg(ap, char *);
				int length = strlen(string);
				size = length + 1;
				result = write(fd, string, size);
			}
			else
				/* restricted to 1, 2, and 4 bytes, or strings */
				return bytes ? bytes : -E_INVAL;
		}
		else
			break;
		
		if(result < 0)
			return bytes ? bytes : result;
		bytes += result;
		if(result != size)
			break;
	}
	
	return bytes;
}
#endif

static void kfs_debug_net_command(void * arg);

int kfs_debug_init(const char * host, uint16_t port)
{
#if !defined(__KERNEL__)
	struct ip_addr addr;
#endif
	int m, o, r;
	int32_t debug_rev, debug_opcode_rev;
	
	printf("Initializing KFS debugging interface...\n");
	
	r = sched_register(kfs_debug_net_command, NULL, HZ / 10);
	if(r < 0)
		return r;
	
#if defined(__KERNEL__)
	proc_buffer = vmalloc(DEBUG_PROC_SIZE);
	if(!proc_buffer)
		return -E_NO_MEM;
	proc_buffer_wpos = 0;
	proc_buffer_rpos = 0;
	
	if(!create_proc_read_entry(DEBUG_PROC_FILENAME, 0400, &proc_root, kfs_debug_proc_read, NULL))
	{
		kdprintf(STDERR_FILENO, "%s: unable to create proc entry\n", __FUNCTION__);
		return -E_UNSPECIFIED;
	}
	r = kfsd_register_shutdown_module(kfs_debug_shutdown, NULL, SHUTDOWN_POSTMODULES);
	if(r < 0)
	{
		kdprintf(STDERR_FILENO, "%s: unable to register shutdown callback\n", __FUNCTION__);
		remove_proc_entry(DEBUG_PROC_FILENAME, &proc_root);
		return r;
	}
#else
	r = kgethostbyname(host, &addr);
	if(r < 0)
	{
		jsleep(2 * HZ);
		r = kgethostbyname(host, &addr);
		if(r < 0)
			return r;
	}
	r = kconnect(addr, port, &debug_socket);
	if(r < 0)
	{
		jsleep(2 * HZ);
		r = kconnect(addr, port, &debug_socket);
		if(r < 0)
			return r;
	}
#endif
	
	debug_rev = svnrevtol("$Rev$");
	debug_opcode_rev = svnrevtol(DEBUG_OPCODE_REV);
	
#if KFS_DEBUG_BINARY
	kfs_debug_write(debug_socket, LIT_32, debug_rev, LIT_32, debug_opcode_rev, END);
	
	for(m = 0; modules[m].opcodes; m++)
		for(o = 0; modules[m].opcodes[o]->params; o++)
		{
			int p;
			kfs_debug_write(debug_socket, LIT_16, modules[m].module, LIT_16, modules[m].opcodes[o]->opcode, LIT_STR, modules[m].opcodes[o]->name, END);
			for(p = 0; modules[m].opcodes[o]->params[p]->name; p++)
			{
				uint8_t size = type_sizes[modules[m].opcodes[o]->params[p]->type];
				/* TODO: maybe write the logical data type here as well */
				kfs_debug_write(debug_socket, LIT_8, size, LIT_STR, modules[m].opcodes[o]->params[p]->name, END);
			}
			kfs_debug_write(debug_socket, LIT_8, 0, END);
		}
	kfs_debug_write(debug_socket, LIT_16, 0, END);
#else
	kdprintf(debug_socket, "DEBUG %d\n", debug_rev);
	kdprintf(debug_socket, "DEBUG_OPCODE %d\n", debug_opcode_rev);

	for(m = 0; modules[m].opcodes; m++)
		for(o = 0; modules[m].opcodes[o]->params; o++)
		{
			int p;
			kdprintf(debug_socket, "[%04x:%04x] %s", modules[m].module, modules[m].opcodes[o]->opcode, modules[m].opcodes[o]->name);
			for(p = 0; modules[m].opcodes[o]->params[p]->name; p++)
				kdprintf(debug_socket, "%s%s", p ? ", " : " (", modules[m].opcodes[o]->params[p]->name);
			kdprintf(debug_socket, ")\n");
		}
	kdprintf(debug_socket, "\n");
#endif
	
	printf("Debugging interface initialized OK\n");
	
	return 0;
}

void kfs_debug_command(uint16_t command, uint16_t module, const char * file, int line, const char * function)
{
	switch(command)
	{
		case KFS_DEBUG_MARK:
			printf("Sent mark [%04x] from %s() at %s:%d\n", module, function, file, line);
			kfs_debug_send(KDB_MODULE_INFO, KDB_INFO_MARK, file, line, function, module);
			break;
		case KFS_DEBUG_DISABLE:
		{
			int m;
			for(m = 0; modules[m].opcodes; m++)
				if(modules[m].module == module)
				{
					printf("Disabled debugging for module [%04x] from %s() at %s:%d\n", module, function, file, line);
					modules_ignore[m] = 1;
					break;
				}
			break;
		}
		case KFS_DEBUG_ENABLE:
		{
			int m;
			for(m = 0; modules[m].opcodes; m++)
				if(modules[m].module == module)
				{
					printf("Enabled debugging for module [%04x] from %s() at %s:%d\n", module, function, file, line);
					modules_ignore[m] = 0;
					break;
				}
			break;
		}
	}
}

#if defined(UNIXUSER)
#include <unistd.h>
#include <fcntl.h>
#endif

static void kfs_debug_net_command(void * arg)
{
	uint16_t command[2];
	int bytes;

#if defined(KUDOS)
	bytes = read_nb(debug_socket, &command, 4);
#elif defined(UNIXUSER)
	if (fcntl(debug_socket, F_SETFL, O_NONBLOCK))
		assert(0);
	bytes = read(debug_socket, &command, 4);
	if (fcntl(debug_socket, F_SETFL, 0)) // TODO: restore to original value
		assert(0);
#elif defined(__KERNEL__)
	// kkfsd does not currently support command reading
	bytes = 0;
#else
#error Unknown target
#endif
	if(bytes == 4)
		kfs_debug_command(ntohs(command[0]), ntohs(command[1]), "<net>", 0, "<net>");
}

#if !defined(__KERNEL__)
static void kfs_debug_wait(void)
{
	uint16_t command[2];
	int bytes = 0;
	while(bytes != 4)
		bytes = read(debug_socket, &command, 4);
	kfs_debug_command(ntohs(command[0]), ntohs(command[1]), "<net>", 0, "<net>");
}
#endif

#ifdef __i386__
#define x86_get_ebp(bp) { void * __bp; __asm__ __volatile__ ("movl %%ebp, %0" : "=r" (__bp) : ); bp = __bp; }
#endif

int kfs_debug_send(uint16_t module, uint16_t opcode, const char * file, int line, const char * function, ...)
{
	int m, o = 0, r = 0;
	va_list ap;
	va_start(ap, function);
	
	kfs_debug_net_command(NULL);
	
	/* look up the right module and opcode indices */
	for(m = 0; modules[m].opcodes; m++)
		if(modules[m].module == module)
		{
			if(modules_ignore[m])
				return 0;
			for(o = 0; modules[m].opcodes[o]->params; o++)
				if(modules[m].opcodes[o]->opcode == opcode)
					break;
			break;
		}
	
	debug_count++;
#if KFS_DEBUG_BINARY
#if KFS_OMIT_FILE_FUNC
	kfs_debug_write(debug_socket, LIT_STR, "", LIT_32, line, LIT_STR, "", LIT_16, module, LIT_16, opcode, END);
#else
	kfs_debug_write(debug_socket, LIT_STR, file, LIT_32, line, LIT_STR, function, LIT_16, module, LIT_16, opcode, END);
#endif
	
	if(!modules[m].opcodes)
	{
		/* unknown module */
		kfs_debug_write(debug_socket, LIT_8, 0, LIT_8, 1, END);
		r = -E_INVAL;
	}
	else if(!modules[m].opcodes[o]->params)
	{
		/* unknown opcode */
		kfs_debug_write(debug_socket, LIT_8, 0, LIT_8, 2, END);
		r = -E_INVAL;
	}
	else
	{
		int p;
		for(p = 0; !r && modules[m].opcodes[o]->params[p]->name; p++)
		{
			/* TODO: we don't actually have to write the size for each parameter... */
			uint8_t size = type_sizes[modules[m].opcodes[o]->params[p]->type];
			if(size == 4)
			{
				uint32_t param = va_arg(ap, uint32_t);
				kfs_debug_write(debug_socket, LIT_8, 4, LIT_32, param, END);
			}
			else if(size == 2)
			{
				uint16_t param = va_arg(ap, uint16_t);
				kfs_debug_write(debug_socket, LIT_8, 2, LIT_16, param, END);
			}
			else if(size == 1)
			{
				uint8_t param = va_arg(ap, uint8_t);
				kfs_debug_write(debug_socket, LIT_8, 1, LIT_8, param, END);
			}
			else if(size == (uint8_t) -1 && modules[m].opcodes[o]->params[p]->type == STRING)
			{
				char * param = va_arg(ap, char *);
				kfs_debug_write(debug_socket, LIT_8, -1, LIT_STR, param, END);
			}
			else
			{
				/* unknown type */
				kfs_debug_write(debug_socket, LIT_8, 0, LIT_8, 3, END);
				r = -E_INVAL;
			}
		}
	}
	
	/* TODO: not technically necessary, see above */
	kfs_debug_write(debug_socket, LIT_16, 0, END);
	
#if !KFS_OMIT_BTRACE
	{
		int preamble = 1;
		void ** ebp;
		void ** last_ebp = NULL;
		x86_get_ebp(ebp);
		while(ebp >= last_ebp)
		{
			if(!ebp[1])
				break;
			if(!preamble || ebp[1] == __builtin_return_address(0))
			{
				kfs_debug_write(debug_socket, LIT_32, ebp[1], END);
				preamble = 0;
			}
			last_ebp = ebp;
			ebp = *ebp;
		}
	}
#endif
	kfs_debug_write(debug_socket, LIT_32, 0, END);
#else
#if KFS_OMIT_FILE_FUNC
	kdprintf(debug_socket, "Line %d, type [%04x:%04x] ", line, module, opcode);
#else
	kdprintf(debug_socket, "%s:%d in %s(), type [%04x:%04x] ", file, line, function, module, opcode);
#endif

	if(!modules[m].opcodes)
	{
		/* unknown module */
		kdprintf(debug_socket, "!module");
		r = -E_INVAL;
	}
	else if(!modules[m].opcodes[o]->params)
	{
		/* unknown opcode */
		kdprintf(debug_socket, "!opcode");
		r = -E_INVAL;
	}
	else
	{
		int p;
		for(p = 0; !r && modules[m].opcodes[o]->params[p]->name; p++)
		{
			if(p)
				kdprintf(debug_socket, ", ");
			switch(modules[m].opcodes[o]->params[p]->type)
			{
				case STRING:
				{
					char * param = va_arg(ap, char *);
					kdprintf(debug_socket, "%s", param);
					break;
				}
				case INT32:
				{
					int32_t param = va_arg(ap, int32_t);
					kdprintf(debug_socket, "%d", param);
					break;
				}
				case UINT32:
				{
					uint32_t param = va_arg(ap, uint32_t);
					kdprintf(debug_socket, "%u", param);
					break;
				}
				case UHEX32:
				{
					uint32_t param = va_arg(ap, uint32_t);
					kdprintf(debug_socket, "0x%08x", param);
					break;
				}
				case INT16:
				{
					int16_t param = va_arg(ap, int16_t);
					kdprintf(debug_socket, "%d", param);
					break;
				}
				case UINT16:
				{
					uint16_t param = va_arg(ap, uint16_t);
					kdprintf(debug_socket, "%u", param);
					break;
				}
				case UHEX16:
				{
					uint16_t param = va_arg(ap, uint16_t);
					kdprintf(debug_socket, "0x%04x", param);
					break;
				}
				case BOOL:
				{
					bool param = va_arg(ap, bool);
					kdprintf(debug_socket, param ? "true" : "false");
					break;
				}
				default:
					/* unknown type */
					kdprintf(debug_socket, "!type");
					r = -E_INVAL;
					break;
			}
		}
	}
	
	kdprintf(debug_socket, "\n");
	
#if !KFS_OMIT_BTRACE
	{
		int frame = 0;
		void ** ebp;
		void ** last_ebp = NULL;
		x86_get_ebp(ebp);
		while(ebp >= last_ebp)
		{
			if(!ebp[1])
				break;
			if(frame || ebp[1] == __builtin_return_address(0))
				kdprintf(debug_socket, "  [%d]: 0x%08x", frame++, ebp[1]);
			last_ebp = ebp;
			ebp = *ebp;
		}
	}
	kdprintf(debug_socket, "\n");
#endif
#endif
	
	va_end(ap);
	
	/* for debugging the debugging interface... */
	if(r < 0)
	{
		printf("kfs_debug_send(%s, %d, %s(), 0x%04x, 0x%04x, ...) = %i\n", file, line, function, module, opcode, r);
#if defined(KUDOS)
		sys_print_backtrace();
		exit(0);
#elif defined(UNIXUSER) || defined(__KERNEL__)
		assert(0);
#else
#error Unknown target
#endif
	}
	return r;
}

void kfs_debug_dbwait(const char * function, bdesc_t * block)
{
#if defined(__KERNEL__)
	kdprintf(STDERR_FILENO, "%s() not supported for kernel, not waiting\n", __FUNCTION__);
#else
	if(block->ddesc->all_changes)
	{
		chdesc_t * chdesc;
		for(chdesc = block->ddesc->all_changes; chdesc; chdesc = chdesc->ddesc_next)
		{
			const uint16_t flags = chdesc->flags;
			if((flags & CHDESC_DBWAIT) && !(flags & CHDESC_ROLLBACK))
			{
				printf("%s(): waiting for debug mark... (%p has DBWAIT)\n", function, chdesc);
				kfs_debug_wait();
				break;
			}
		}
	}
#endif
}

int kfs_debug_count(void)
{
	return debug_count;
}

#endif
