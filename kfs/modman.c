#include <inc/lib.h>
#include <inc/malloc.h>
#include <inc/hash_map.h>
#include <inc/vector.h>

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

MODMAN_ENTRY_STRUCT(void, module);

static hash_map_t * bd_map = NULL;
static hash_map_t * cfs_map = NULL;
static hash_map_t * lfs_map = NULL;

CFS_t * modman_devfs = NULL;

static int modman_add(hash_map_t * map, void * module, const char * name)
{
	modman_entry_module_t * mod = (modman_entry_module_t *) hash_map_find_val(map, module);
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
	
	mod->users = vector_create();
	if(!mod->users)
	{
		free((char *) mod->name);
		free(mod);
		return -E_NO_MEM;
	}
	
	r = hash_map_insert(map, module, mod);
	if(r < 0)
	{
		vector_destroy(mod->users);
		free((char *) mod->name);
		free(mod);
		return r;
	}
	
	/* this is a cheezy hack to add BD modules to modman_devfs
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
		/* usage count will have increased to 1, put it down to 0 again */
		mod->usage = 0;
		Dprintf("%s: resetting usage count of new module %s to 0\n", __FUNCTION__, mod->name);
	}
	
	Dprintf("%s: new module %s\n", __FUNCTION__, mod->name);
	return 0;
}

static int modman_add_anon(hash_map_t * map, void * module, const char * prefix)
{
	char name[64];
	snprintf(name, 64, "%s-%08x", prefix, module);
	return modman_add(map, module, name);
}

static int modman_inc(hash_map_t * map, void * module, void * user)
{
	modman_entry_module_t * mod = (modman_entry_module_t *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	if(user)
	{
		int r = vector_push_back(mod->users, user);
		if(r < 0)
			return r;
	}
	Dprintf("%s: increasing usage of %s to %d by 0x%08x\n", __FUNCTION__, mod->name, mod->usage + 1, user);
	return ++mod->usage;
}

static int modman_dec(hash_map_t * map, void * module, void * user)
{
	modman_entry_module_t * mod = (modman_entry_module_t *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	if(!mod->usage)
		return -E_INVAL;
	if(user)
	{
		/* remove the last instance of this user from the vector (more efficient than the first) */
		size_t last = vector_size(mod->users);
		while(last-- > 0)
			if(vector_elt(mod->users, last) == user)
			{
				vector_erase(mod->users, last);
				break;
			}
	}
	Dprintf("%s: decreasing usage of %s to %d by 0x%08x\n", __FUNCTION__, mod->name, mod->usage - 1, user);
	return --mod->usage;
}

static int modman_rem(hash_map_t * map, void * module)
{
	modman_entry_module_t * mod = (modman_entry_module_t *) hash_map_find_val(map, module);
	
	if(!mod)
		return -E_NOT_FOUND;
	if(mod->usage)
		return -E_BUSY;
	
	/* this is a cheezy hack to remove BD modules from modman_devfs
	 * here in modman_rem(), rather than the macro-generated
	 * function modman_rem_bd() below */
	if(map == bd_map)
		devfs_bd_remove(modman_devfs, mod->name);
	
	Dprintf("%s: removing module %s\n", __FUNCTION__, mod->name);
	hash_map_erase(map, module);
	vector_destroy(mod->users);
	free((char *) mod->name);
	free(mod);
	return 0;
}

static const modman_entry_module_t * modman_lookup(hash_map_t * map, void * module)
{
	modman_entry_module_t * mod = (modman_entry_module_t *) hash_map_find_val(map, module);
	/* this conditional will be optimized away when not compiling with debugging */
	if(mod)
		Dprintf("%s: lookup module %s\n", __FUNCTION__, mod->name);
	return mod;
}

static const char * modman_name(hash_map_t * map, void * module)
{
	modman_entry_module_t * mod = (modman_entry_module_t *) hash_map_find_val(map, module);
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

#define MODMAN_ADD(type, postfix...) \
int modman_add##postfix##_##type(type##_t * type, const char * name) \
{ \
	return modman_add##postfix(type##_map, type, name); \
}

#define MODMAN_ADD_ANON(type) MODMAN_ADD(type, _anon)

#define MODMAN_COUNT(op, type) \
int modman_##op##_##type(type##_t * type, void * user) \
{ \
	return modman_##op(type##_map, type, user); \
}

#define MODMAN_OP(value, op, type, cast...) \
value modman_##op##_##type(type##_t * type) \
{ \
	return cast modman_##op(type##_map, type); \
}

#define MODMAN_OP_CAST(value, op, type) MODMAN_OP(value, op, type, (value))

MODMAN_ADD(bd);
MODMAN_ADD(cfs);
MODMAN_ADD(lfs);
MODMAN_ADD_ANON(bd);
MODMAN_ADD_ANON(cfs);
MODMAN_ADD_ANON(lfs);

MODMAN_COUNT(inc, bd);
MODMAN_COUNT(inc, cfs);
MODMAN_COUNT(inc, lfs);
MODMAN_COUNT(dec, bd);
MODMAN_COUNT(dec, cfs);
MODMAN_COUNT(dec, lfs);
MODMAN_OP(int, rem, bd);
MODMAN_OP(int, rem, cfs);
MODMAN_OP(int, rem, lfs);
MODMAN_OP_CAST(const modman_entry_bd_t *, lookup, bd);
MODMAN_OP_CAST(const modman_entry_cfs_t *, lookup, cfs);
MODMAN_OP_CAST(const modman_entry_lfs_t *, lookup, lfs);

MODMAN_OP(const char *, name, bd);
MODMAN_OP(const char *, name, cfs);
MODMAN_OP(const char *, name, lfs);

#define MODMAN_IT_CREATE(type) \
modman_it_t * modman_it_create_##type(void) \
{ \
	return hash_map_it_create(type##_map); \
}

#define MODMAN_IT_NEXT(type) \
type##_t * modman_it_next_##type(modman_it_t * it) \
{ \
	return (type##_t *) hash_map_val_next(type##_map, it); \
}

MODMAN_IT_CREATE(bd);
MODMAN_IT_CREATE(cfs);
MODMAN_IT_CREATE(lfs);

MODMAN_IT_NEXT(bd);
MODMAN_IT_NEXT(cfs);
MODMAN_IT_NEXT(lfs);
