#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/hash_map.h>

#include <kfs/bd.h>
#include <kfs/cfs.h>
#include <kfs/lfs.h>
#include <kfs/devfs_cfs.h>
#include <kfs/modman.h>

#define MODMAN_DEBUG 0

#if MODMAN_DEBUG
#define Dprintf(x...) printf(x)
#else
#define Dprintf(x...)
#endif

struct module {
	void * module;
	uint32_t usage;
	const char * name;
};

static hash_map_t * bd_map = NULL;
static hash_map_t * cfs_map = NULL;
static hash_map_t * lfs_map = NULL;

CFS_t * modman_devfs = NULL;

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
		return r;
	}
	
	/* this is a special hack to add the module to modman_devfs
	 * here in modman_add(), rather than the macro-generated
	 * function modman_add_bd() below */
	if(map == bd_map)
	{
		r = devfs_bd_add(modman_devfs, mod->name, (BD_t *) module);
		if(r < 0)
		{
			hash_map_erase(map, module);
			free((char *) mod->name);
			free(mod);
			return r;
		}
	}
	
	Dprintf("%s: new module %s\n", __FUNCTION__, mod->name);
	return 0;
}

static int modman_add_anon(hash_map_t * map, void * module, const char * function)
{
	char name[64];
	snprintf(name, 64, "%s-%08x", function, module);
	return modman_add(map, module, name);
}

static uint32_t modman_inc(hash_map_t * map, void * module)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	Dprintf("%s: increasing usage of %s to %d\n", __FUNCTION__, mod->name, mod->usage + 1);
	return ++mod->usage;
}

static uint32_t modman_dec(hash_map_t * map, void * module)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	if(!mod->usage)
		return -E_INVAL;
	Dprintf("%s: decreasing usage of %s to %d\n", __FUNCTION__, mod->name, mod->usage - 1);
	return --mod->usage;
}

static int modman_rem(hash_map_t * map, void * module)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	if(mod->usage)
		return -E_BUSY;
	Dprintf("%s: removing module %s to %d\n", __FUNCTION__, mod->name);
	hash_map_erase(map, module);
	free((char *) mod->name);
	free(mod);
	return 0;
}

static uint32_t modman_query(hash_map_t * map, void * module)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	Dprintf("%s: query usage count of %s (%d)\n", __FUNCTION__, mod->name, mod->usage);
	return mod->usage;
}

static const char * modman_name(hash_map_t * map, void * module)
{
	struct module * mod = (struct module *) hash_map_find_val(map, module);
	if(!mod)
		return NULL;
	Dprintf("%s: lookup module %s (by address 0x%08x)\n", __FUNCTION__, mod->name, module);
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
	{
		modman_devfs = devfs_cfs(NULL, NULL, 0);
		if(modman_devfs)
			return 0;
	}
	
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

#define MODMAN_ADD_ANON(type) \
int modman_add_anon_##type(type##_t * type, const char * function) \
{ \
	return modman_add_anon(type##_map, type, function); \
}

#define MODMAN_OP(value, op, type) \
value modman_##op##_##type(type##_t * type) \
{ \
	return modman_##op(type##_map, type); \
}

MODMAN_ADD(bd);
MODMAN_ADD(cfs);
MODMAN_ADD(lfs);
MODMAN_ADD_ANON(bd);
MODMAN_ADD_ANON(cfs);
MODMAN_ADD_ANON(lfs);

MODMAN_OP(uint32_t, inc, bd);
MODMAN_OP(uint32_t, inc, cfs);
MODMAN_OP(uint32_t, inc, lfs);
MODMAN_OP(uint32_t, dec, bd);
MODMAN_OP(uint32_t, dec, cfs);
MODMAN_OP(uint32_t, dec, lfs);
MODMAN_OP(int, rem, bd);
MODMAN_OP(int, rem, cfs);
MODMAN_OP(int, rem, lfs);
MODMAN_OP(uint32_t, query, bd);
MODMAN_OP(uint32_t, query, cfs);
MODMAN_OP(uint32_t, query, lfs);

MODMAN_OP(const char *, name, bd);
MODMAN_OP(const char *, name, cfs);
MODMAN_OP(const char *, name, lfs);
