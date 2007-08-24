#ifndef __FSTITCH_FSCORE_MODMAN_BD_H
#define __FSTITCH_FSCORE_MODMAN_BD_H

#include <lib/vector.h>
#include <lib/hash_map.h>

#include <fscore/bd.h>
#include <fscore/cfs.h>
#include <fscore/lfs.h>

extern CFS_t * modman_devfs;

#define MODMAN_ENTRY_STRUCT(module, type, qualifier...) \
typedef struct modman_entry_##type { \
	qualifier module * type;         /* this module's address */ \
	qualifier int usage;             /* this module's usage count */ \
	const char * name;               /* this module's name */ \
	qualifier vector_t * users;      /* the users of this module - no type information though */ \
	qualifier vector_t * use_names;  /* the use names for each user, in the same order */ \
} modman_entry_##type##_t;

MODMAN_ENTRY_STRUCT(BD_t, bd, const);
MODMAN_ENTRY_STRUCT(CFS_t, cfs, const);
MODMAN_ENTRY_STRUCT(LFS_t, lfs, const);

#ifdef FSTITCHD
typedef hash_map_it_t modman_it_t;
#else
struct modman_it {
	vector_t * v; // vector_t of uint32_t ids
	size_t next;
};
typedef struct modman_it modman_it_t;
#endif

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
int modman_inc_bd(BD_t * bd, void * user, const char * name);
int modman_inc_cfs(CFS_t * cfs, void * user, const char * name);
int modman_inc_lfs(LFS_t * lfs, void * user, const char * name);

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

int modman_it_init_bd(modman_it_t * it);
int modman_it_init_cfs(modman_it_t * it);
int modman_it_init_lfs(modman_it_t * it);

BD_t * modman_it_next_bd(modman_it_t * it);
CFS_t * modman_it_next_cfs(modman_it_t * it);
LFS_t * modman_it_next_lfs(modman_it_t * it);

#ifdef FSTITCHD
#define modman_it_destroy(it)
#else
void modman_it_destroy(modman_it_t * it);
#endif

#endif /* __FSTITCH_FSCORE_MODMAN_BD_H */
