#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/hash_map.h>

#include <kfs/bd.h>
#include <kfs/cfs.h>
#include <kfs/lfs.h>
#include <kfs/modman.h>

struct module {
	void * module;
	int usage;
	const char * name;
};

static hash_map_t * bd_map = NULL;
static hash_map_t * cfs_map = NULL;
static hash_map_t * lfs_map = NULL;

static int modman_add(hash_map_t * map, void * module, const char * name)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	int r;
	if(mod)
		return -E_BUSY;
	mod = malloc(sizeof(*mod));
	if(!mod)
		return -E_NO_MEM;
	mod->module = module;
	mod->usage = 0;
	mod->name = strdup(name);
	if(!mod->name)
	{
		free(mod);
		return -E_NO_MEM;
	}
	r = hash_map_insert(map, module, mod);
	if(r < 0)
	{
		free((char *) mod->name);
		free(mod);
	}
	return r;
}

static int modman_inc(hash_map_t * map, void * module)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	return ++mod->usage;
}

static int modman_dec(hash_map_t * map, void * module)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	if(!mod->usage)
		return -E_INVAL;
	return --mod->usage;
}

static int modman_rem(hash_map_t * map, void * module)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	if(mod->usage)
		return -E_INVAL;
	hash_map_erase(map, module);
	free((char *) mod->name);
	free(mod);
	return 0;
}

static int modman_query(hash_map_t * map, void * module)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	return mod->usage;
}

static const char * modman_name(hash_map_t * map, void * module)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	if(!mod)
		return NULL;
	return mod->name;
}

int modman_init(void)
{
	if(bd_map || cfs_map || lfs_map)
		return -E_BUSY;
	
	bd_map = hash_map_create();
	cfs_map = hash_map_create();
	lfs_map = hash_map_create();
	
	if(bd_map && cfs_map && lfs_map)
		return 0;
	
	if(bd_map)
		hash_map_destroy(bd_map);
	if(cfs_map)
		hash_map_destroy(cfs_map);
	if(lfs_map)
		hash_map_destroy(lfs_map);
	return -E_NO_MEM;
}

/* Generate all the modman_op_type() functions which are exposed to the rest of kfsd with some handy macros... */

typedef BD_t bd_t;
typedef CFS_t cfs_t;
typedef LFS_t lfs_t;

#define MODMAN_ADD(type) \
int modman_add_##type(type##_t * type, const char * name) \
{ \
	return modman_add(type##_map, type, name); \
}

#define MODMAN_OP(op, type) \
int modman_##op##_##type(type##_t * type) \
{ \
	return modman_##op(type##_map, type); \
}

#define MODMAN_NAME(type) \
const char * modman_name_##type(type##_t * type) \
{ \
	return modman_name(type##_map, type); \
}

MODMAN_ADD(bd);
MODMAN_ADD(cfs);
MODMAN_ADD(lfs);

MODMAN_OP(inc, bd);
MODMAN_OP(inc, cfs);
MODMAN_OP(inc, lfs);
MODMAN_OP(dec, bd);
MODMAN_OP(dec, cfs);
MODMAN_OP(dec, lfs);
MODMAN_OP(rem, bd);
MODMAN_OP(rem, cfs);
MODMAN_OP(rem, lfs);
MODMAN_OP(query, bd);
MODMAN_OP(query, cfs);
MODMAN_OP(query, lfs);

MODMAN_NAME(bd);
MODMAN_NAME(cfs);
MODMAN_NAME(lfs);
