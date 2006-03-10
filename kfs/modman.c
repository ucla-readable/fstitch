#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <lib/hash_map.h>
#include <lib/vector.h>
#include <inc/error.h>

#include <kfs/bd.h>
#include <kfs/cfs.h>
#include <kfs/lfs.h>
#include <kfs/debug.h>
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
	int r = -E_NO_MEM;
	
	if(mod)
		return -E_BUSY;
	
	mod = malloc(sizeof(*mod));
	if(!mod)
		return -E_NO_MEM;
	
	mod->module = module;
	mod->usage = 0;
	mod->name = strdup(name);
	
	if(!mod->name)
		goto error_name;
	
	mod->users = vector_create();
	if(!mod->users)
		goto error_users;
	
	mod->use_names = vector_create();
	if(!mod->use_names)
		goto error_use_names;
	
	r = hash_map_insert(map, module, mod);
	if(r < 0)
		goto error_insert;
	
	/* this is a cheezy hack to add BD modules to modman_devfs
	 * here in modman_add(), rather than the macro-generated
	 * function modman_add_bd() below */
	if(map == bd_map)
	{
		char * dev_name;
		r = devfs_bd_add(modman_devfs, mod->name, (BD_t *) module);
		if(r < 0)
			goto error_hack;
		KFS_DEBUG_SEND(KDB_MODULE_INFO, KDB_INFO_BD_NAME, module, name);
		/* usage count will have increased to 1, put it down to 0 again */
		mod->usage = 0;
		vector_pop_back(mod->users);
		dev_name = vector_elt(mod->use_names, vector_size(mod->use_names) - 1);
		if(dev_name)
			free(dev_name);
		vector_pop_back(mod->use_names);
		Dprintf("%s: resetting usage count of new module %s to 0\n", __FUNCTION__, mod->name);
	}
	
	Dprintf("%s: new module %s\n", __FUNCTION__, mod->name);
	return 0;
	
error_hack:
	hash_map_erase(map, module);
error_insert:
	vector_destroy(mod->use_names);
error_use_names:
	vector_destroy(mod->users);
error_users:
	free((char *) mod->name);
error_name:
	free(mod);
	return r;
}

static int modman_add_anon(hash_map_t * map, void * module, const char * prefix)
{
	char name[64];
	/* subtract 0x10000000 to make the generated names have fewer digits */
	snprintf(name, 64, "%s-%p", prefix, module - 0x10000000);
	return modman_add(map, module, name);
}

static int modman_inc(hash_map_t * map, void * module, void * user, const char * name)
{
	modman_entry_module_t * mod = (modman_entry_module_t *) hash_map_find_val(map, module);
	if(!mod)
		return -E_NOT_FOUND;
	if(user)
	{
		char * copy = NULL;
		int r;
		if(name)
		{
			copy = strdup(name);
			if(!copy)
				return -E_NO_MEM;
		}
		r = vector_push_back(mod->users, user);
		if(r < 0)
		{
			if(copy)
				free(copy);
			return r;
		}
		r = vector_push_back(mod->use_names, copy);
		if(r < 0)
		{
			vector_pop_back(mod->users);
			if(copy)
				free(copy);
			return -E_NO_MEM;
		}
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
		/* remove the last instance of this user from the vectors (more efficient than the first) */
		size_t last = vector_size(mod->users);
		while(last-- > 0)
			if(vector_elt(mod->users, last) == user)
			{
				char * name = (char *) vector_elt(mod->use_names, last);
				if(name)
					free(name);
				vector_erase(mod->use_names, last);
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
	vector_destroy(mod->use_names);
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

#define MODMAN_INC(type) \
int modman_inc_##type(type##_t * type, void * user, const char * name) \
{ \
	return modman_inc(type##_map, type, user, name); \
}

#define MODMAN_DEC(type) \
int modman_dec_##type(type##_t * type, void * user) \
{ \
	return modman_dec(type##_map, type, user); \
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

MODMAN_INC(bd);
MODMAN_INC(cfs);
MODMAN_INC(lfs);
MODMAN_DEC(bd);
MODMAN_DEC(cfs);
MODMAN_DEC(lfs);
MODMAN_OP(int, rem, bd);
MODMAN_OP(int, rem, cfs);
MODMAN_OP(int, rem, lfs);
MODMAN_OP_CAST(const modman_entry_bd_t *, lookup, bd);
MODMAN_OP_CAST(const modman_entry_cfs_t *, lookup, cfs);
MODMAN_OP_CAST(const modman_entry_lfs_t *, lookup, lfs);

MODMAN_OP(const char *, name, bd);
MODMAN_OP(const char *, name, cfs);
MODMAN_OP(const char *, name, lfs);

#define MODMAN_IT_INIT(type) \
int modman_it_init_##type(modman_it_t * it) \
{ \
	hash_map_it_init(it, type##_map); \
	return 0; \
}

#define MODMAN_IT_NEXT(type) \
type##_t * modman_it_next_##type(modman_it_t * it) \
{ \
	modman_entry_##type##_t * me = hash_map_val_next(it); \
	return me ? (type##_t *) me->type : NULL; \
}

MODMAN_IT_INIT(bd);
MODMAN_IT_INIT(cfs);
MODMAN_IT_INIT(lfs);

MODMAN_IT_NEXT(bd);
MODMAN_IT_NEXT(cfs);
MODMAN_IT_NEXT(lfs);
