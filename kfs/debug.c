#include <inc/lib.h>
#include <kfs/debug.h>

#if KFS_DEBUG

/* structure definitions */
struct param {
	const char * name;
	enum {
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
	uint32_t opcode;
	const char * name;
	const struct param ** params;
};

struct module {
	uint32_t module;
	const struct opcode ** opcodes;
};

/* data declarations */

/* all parameters */
static const struct param
	last_param = {NULL, 0},
	param_address = {"address", UHEX32},
	param_ddesc = {"ddesc", UHEX32},
	param_number = {"number", UINT32},
	param_ref_count = {"ref_count", UINT32},
	param_ar_count = {"ar_count", UINT32},
	param_dd_count = {"dd_count", UINT32},
	param_owner = {"owner", UHEX32},
	param_block = {"block", UHEX32},
	param_depth = {"depth", UINT32},
	param_offset = {"offset", UINT16},
	param_length = {"length", UINT16},
	param_xor = {"xor", UHEX32},
	param_recent = {"recent", UHEX32},
	param_original = {"original", UHEX32},
	param_target = {"target", UHEX32},
	param_flags = {"flags", UHEX32},
	param_slip_under = {"slip_under", BOOL},
	param_location = {"location", UHEX32};

/* parameter combinations */
static const struct param * params_bdesc_alloc[] = {
	&param_address,
	&param_ddesc,
	&param_number,
	&last_param
};
static const struct param * params_bdesc_retain_release[] = {
	&param_address,
	&param_ddesc,
	&param_ref_count,
	&param_ar_count,
	&param_dd_count,
	&last_param
};
static const struct param * params_bdesc_destroy[] = {
	&param_address,
	&param_ddesc,
	&last_param
};
static const struct param * params_bdesc_free_ddesc[] = {
	&param_address,
	&param_ddesc,
	&last_param
};
static const struct param * params_bdesc_ar_push_pop[] = {
	&param_depth,
	&last_param
};
static const struct param * params_chdesc_create_noop[] = {
	&param_address,
	&param_block,
	&param_owner,
	&last_param
};
static const struct param * params_chdesc_create_bit[] = {
	&param_address,
	&param_block,
	&param_owner,
	&param_offset,
	&param_xor,
	&last_param
};
static const struct param * params_chdesc_create_byte[] = {
	&param_address,
	&param_block,
	&param_owner,
	&param_offset,
	&param_length,
	&last_param
};
static const struct param * params_chdesc_connect[] = {
	&param_address,
	&param_target,
	&last_param
};
static const struct param * params_chdesc_flags[] = {
	&param_address,
	&param_flags,
	&last_param
};
static const struct param * params_chdesc_only[] = {
	&param_address,
	&last_param
};
static const struct param * params_chdesc_weak_retain_release[] = {
	&param_address,
	&param_location,
	&last_param
};
static const struct param * params_chdesc_overlap_attach[] = {
	&param_recent,
	&param_original,
	&last_param
};
static const struct param * params_chdesc_overlap_multiattach[] = {
	&param_address,
	&param_block,
	&param_slip_under,
	&last_param
};

#define OPCODE(number, params) {number, #number, params}

/* all opcodes */
static const struct opcode
	last_opcode = {0, NULL, NULL},
	opcode_bdesc_alloc = OPCODE(KDB_BDESC_ALLOC, params_bdesc_alloc),
	opcode_bdesc_alloc_wrap = OPCODE(KDB_BDESC_ALLOC_WRAP, params_bdesc_alloc),
	opcode_bdesc_retain = OPCODE(KDB_BDESC_RETAIN, params_bdesc_retain_release),
	opcode_bdesc_release = OPCODE(KDB_BDESC_RELEASE, params_bdesc_retain_release),
	opcode_bdesc_autorelease = OPCODE(KDB_BDESC_AUTORELEASE, params_bdesc_retain_release),
	opcode_bdesc_destroy = OPCODE(KDB_BDESC_DESTROY, params_bdesc_destroy),
	opcode_bdesc_free_ddesc = OPCODE(KDB_BDESC_FREE_DDESC, params_bdesc_free_ddesc),
	opcode_bdesc_ar_pool_push = OPCODE(KDB_BDESC_AR_POOL_PUSH, params_bdesc_ar_push_pop),
	opcode_bdesc_ar_pool_pop = OPCODE(KDB_BDESC_AR_POOL_POP, params_bdesc_ar_push_pop),
	opcode_chdesc_create_noop = OPCODE(KDB_CHDESC_CREATE_NOOP, params_chdesc_create_noop),
	opcode_chdesc_create_bit = OPCODE(KDB_CHDESC_CREATE_BIT, params_chdesc_create_bit),
	opcode_chdesc_create_byte = OPCODE(KDB_CHDESC_CREATE_BYTE, params_chdesc_create_byte),
	opcode_chdesc_add_dependency = OPCODE(KDB_CHDESC_ADD_DEPENDENCY, params_chdesc_connect),
	opcode_chdesc_add_dependent = OPCODE(KDB_CHDESC_ADD_DEPENDENT, params_chdesc_connect),
	opcode_chdesc_rem_dependency = OPCODE(KDB_CHDESC_REM_DEPENDENCY, params_chdesc_connect),
	opcode_chdesc_rem_dependent = OPCODE(KDB_CHDESC_REM_DEPENDENT, params_chdesc_connect),
	opcode_chdesc_set_flags = OPCODE(KDB_CHDESC_SET_FLAGS, params_chdesc_flags),
	opcode_chdesc_clear_flags = OPCODE(KDB_CHDESC_CLEAR_FLAGS, params_chdesc_flags),
	opcode_chdesc_apply = OPCODE(KDB_CHDESC_APPLY, params_chdesc_only),
	opcode_chdesc_rollback = OPCODE(KDB_CHDESC_ROLLBACK, params_chdesc_only),
	opcode_chdesc_weak_retain = OPCODE(KDB_CHDESC_WEAK_RETAIN, params_chdesc_weak_retain_release),
	opcode_chdesc_weak_forget = OPCODE(KDB_CHDESC_WEAK_FORGET, params_chdesc_weak_retain_release),
	opcode_chdesc_destroy = OPCODE(KDB_CHDESC_DESTROY, params_chdesc_only),
	opcode_chdesc_overlap_attach = OPCODE(KDB_CHDESC_OVERLAP_ATTACH, params_chdesc_overlap_attach),
	opcode_chdesc_overlap_multiattach = OPCODE(KDB_CHDESC_OVERLAP_MULTIATTACH, params_chdesc_overlap_multiattach),
	opcode_chdesc_weak_collect = OPCODE(KDB_CHDESC_WEAK_COLLECT, params_chdesc_only);

/* opcode combinations */
static const struct opcode * opcodes_bdesc[] = {
	&opcode_bdesc_alloc,
	&opcode_bdesc_alloc_wrap,
	&opcode_bdesc_retain,
	&opcode_bdesc_release,
	&opcode_bdesc_autorelease,
	&opcode_bdesc_destroy,
	&opcode_bdesc_free_ddesc,
	&opcode_bdesc_ar_pool_push,
	&opcode_bdesc_ar_pool_pop,
	&last_opcode
};
static const struct opcode * opcodes_chdesc_alter[] = {
	&opcode_chdesc_create_noop,
	&opcode_chdesc_create_bit,
	&opcode_chdesc_create_byte,
	&opcode_chdesc_add_dependency,
	&opcode_chdesc_add_dependent,
	&opcode_chdesc_rem_dependency,
	&opcode_chdesc_rem_dependent,
	&opcode_chdesc_set_flags,
	&opcode_chdesc_clear_flags,
	&opcode_chdesc_apply,
	&opcode_chdesc_rollback,
	&opcode_chdesc_weak_retain,
	&opcode_chdesc_weak_forget,
	&opcode_chdesc_destroy,
	&last_opcode
};
static const struct opcode * opcodes_chdesc_info[] = {
	&opcode_chdesc_overlap_attach,
	&opcode_chdesc_overlap_multiattach,
	&opcode_chdesc_weak_collect,
	&last_opcode
};

/* modules */
static const struct module modules[] = {
	 {KDB_MODULE_BDESC, opcodes_bdesc},
	 {KDB_MODULE_CHDESC_ALTER, opcodes_chdesc_alter},
	 {KDB_MODULE_CHDESC_INFO, opcodes_chdesc_info},
	 {0, NULL}
};
	
static int debug_socket[2];

int kfs_debug_init(const char * host, uint16_t port)
{
	struct ip_addr addr;
	int m, o, p, r;
	
	printf("Initializing KFS debugging interface...\n");
	r = gethostbyname(host, &addr);
	if(r < 0)
	{
		sleep(200);
		r = gethostbyname(host, &addr);
		if(r < 0)
			return r;
	}
	r = connect(addr, port, debug_socket);
	if(r < 0)
	{
		sleep(200);
		r = connect(addr, port, debug_socket);
		if(r < 0)
			return r;
	}
	
	for(m = 0; modules[m].opcodes; m++)
		for(o = 0; modules[m].opcodes[o]->params; o++)
		{
			fprintf(debug_socket[1], "[%04x:%04x] %s", modules[m].module, modules[m].opcodes[o]->opcode, modules[m].opcodes[o]->name);
			for(p = 0; modules[m].opcodes[o]->params[p]->name; p++)
				fprintf(debug_socket[1], "%s%s", p ? ", " : " (", modules[m].opcodes[o]->params[p]->name);
			fprintf(debug_socket[1], ")\n");
		}
	fprintf(debug_socket[1], "\n");
	
	printf("Debugging interface initialized OK\n");
	
	return 0;
}

int kfs_debug_send(const char * file, int line, const char * function, uint32_t module, uint32_t opcode, ...)
{
	int m, o, p, r = 0;
	va_list ap;
	va_start(ap, opcode);
	
	fprintf(debug_socket[1], "%s:%d in %s(), type [%04x:%04x] ", file, line, function, module, opcode);
	
	for(m = 0; !r && modules[m].opcodes; m++)
	{
		if(modules[m].module == module)
		{
			for(o = 0; !r && modules[m].opcodes[o]->params; o++)
			{
				if(modules[m].opcodes[o]->opcode == opcode)
				{
					for(p = 0; !r && modules[m].opcodes[o]->params[p]->name; p++)
					{
						if(p)
							fprintf(debug_socket[1], ", ");
						switch(modules[m].opcodes[o]->params[p]->type)
						{
							case INT32:
							{
								int32_t param = va_arg(ap, int32_t);
								fprintf(debug_socket[1], "%d", param);
								break;
							}
							case UINT32:
							{
								uint32_t param = va_arg(ap, uint32_t);
								fprintf(debug_socket[1], "%u", param);
								break;
							}
							case UHEX32:
							{
								uint32_t param = va_arg(ap, uint32_t);
								fprintf(debug_socket[1], "0x%08x", param);
								break;
							}
							case INT16:
							{
								int16_t param = va_arg(ap, int16_t);
								fprintf(debug_socket[1], "%d", param);
								break;
							}
							case UINT16:
							{
								uint16_t param = va_arg(ap, uint16_t);
								fprintf(debug_socket[1], "%u", param);
								break;
							}
							case UHEX16:
							{
								uint16_t param = va_arg(ap, uint16_t);
								fprintf(debug_socket[1], "0x%04x", param);
								break;
							}
							case BOOL:
							{
								bool param = va_arg(ap, bool);
								fprintf(debug_socket[1], param ? "true" : "false");
								break;
							}
							default:
								/* unknown type */
								fprintf(debug_socket[1], "!type");
								r = -E_INVAL;
								break;
						}
					}
					break;
				}
			}
			if(!r && !modules[m].opcodes[o]->params)
			{
				/* unknown opcode */
				fprintf(debug_socket[1], "!opcode");
				r = -E_INVAL;
			}
			break;
		}
	}
	if(!r && !modules[m].opcodes)
	{
		/* unknown module */
		fprintf(debug_socket[1], "!module");
		r = -E_INVAL;
	}
	
	fprintf(debug_socket[1], "\n");
	
	va_end(ap);
	
	/* for debugging the debugging interface... */
	if(r < 0)
	{
		printf("kfs_debug_send(%s, %d, %s(), 0x%04x, 0x%04x, ...) = %e\n", file, line, function, module, opcode, r);
		sys_print_backtrace();
		exit();
	}
	return r;
}

#endif