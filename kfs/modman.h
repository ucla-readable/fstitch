#ifndef __KUDOS_KFS_MODMAN_BD_H
#define __KUDOS_KFS_MODMAN_BD_H

#include <inc/vector.h>
#include <inc/hash_map.h>

#include <kfs/bd.h>
#include <kfs/cfs.h>
#include <kfs/lfs.h>

extern CFS_t * modman_devfs;

#define MODMAN_ENTRY_STRUCT(module, type, qualifier...) \
struct modman_entry_##type { \
	qualifier module * type; \
	qualifier int usage; \
	const char * name; \
	qualifier vector_t * users; \
}; \
typedef struct modman_entry_##type modman_entry_##type##_t;

MODMAN_ENTRY_STRUCT(BD_t, bd, const);
MODMAN_ENTRY_STRUCT(CFS_t, cfs, const);
MODMAN_ENTRY_STRUCT(LFS_t, lfs, const);

typedef hash_map_it_t modman_it_t;

int modman_init(void);

/* Add a module to modman, and give it zero usage count. */
int modman_add_bd(BD_t * bd, const char * name);
int modman_add_cfs(CFS_t * cfs, const char * name);
int modman_add_lfs(LFS_t * lfs, const char * name);

/* Add an unnamed module to modman, and give it zero usage count. */
int modman_add_anon_bd(BD_t * bd, const char * prefix);
int modman_add_anon_cfs(CFS_t * cfs, const char * prefix);
int modman_add_anon_lfs(LFS_t * lfs, const char * prefix);

/* Increment the usage count and return the new value. */
int modman_inc_bd(BD_t * bd, void * user);
int modman_inc_cfs(CFS_t * cfs, void * user);
int modman_inc_lfs(LFS_t * lfs, void * user);

/* Decrement the usage count and return the new value. */
int modman_dec_bd(BD_t * bd, void * user);
int modman_dec_cfs(CFS_t * cfs, void * user);
int modman_dec_lfs(LFS_t * lfs, void * user);

/* Remove a module from modman, if it has zero usage count. */
int modman_rem_bd(BD_t * bd);
int modman_rem_cfs(CFS_t * cfs);
int modman_rem_lfs(LFS_t * lfs);

/* Return the modman entry structure for the given module. */
const modman_entry_bd_t * modman_lookup_bd(BD_t * bd);
const modman_entry_cfs_t * modman_lookup_cfs(CFS_t * cfs);
const modman_entry_lfs_t * modman_lookup_lfs(LFS_t * lfs);

/* Get the module name given when the module was added to modman. */
const char * modman_name_bd(BD_t * bd);
const char * modman_name_cfs(CFS_t * cfs);
const char * modman_name_lfs(LFS_t * lfs);

modman_it_t * modman_it_create_bd(void);
modman_it_t * modman_it_create_cfs(void);
modman_it_t * modman_it_create_lfs(void);

BD_t * modman_it_next_bd(modman_it_t * it);
CFS_t * modman_it_next_cfs(modman_it_t * it);
LFS_t * modman_it_next_lfs(modman_it_t * it);

#define modman_it_destroy hash_map_it_destroy

#endif /* __KUDOS_KFS_MODMAN_BD_H */
