#ifndef __KUDOS_KFS_MODMAN_BD_H
#define __KUDOS_KFS_MODMAN_BD_H

#include <kfs/bd.h>
#include <kfs/cfs.h>
#include <kfs/lfs.h>

extern CFS_t * modman_devfs;

int modman_init(void);

/* Add a module to modman, and give it zero usage count. */
int modman_add_bd(BD_t * bd, const char * name);
int modman_add_cfs(CFS_t * cfs, const char * name);
int modman_add_lfs(LFS_t * lfs, const char * name);

/* Add an unnamed module to modman, and give it zero usage count. */
int modman_add_anon_bd(BD_t * bd, const char * function);
int modman_add_anon_cfs(CFS_t * cfs, const char * function);
int modman_add_anon_lfs(LFS_t * lfs, const char * function);

/* Increment the usage count and return the new value. */
uint32_t modman_inc_bd(BD_t * bd);
uint32_t modman_inc_cfs(CFS_t * cfs);
uint32_t modman_inc_lfs(LFS_t * lfs);

/* Decrement the usage count and return the new value. */
uint32_t modman_dec_bd(BD_t * bd);
uint32_t modman_dec_cfs(CFS_t * cfs);
uint32_t modman_dec_lfs(LFS_t * lfs);

/* Remove a module from modman, if it has zero usage count. */
int modman_rem_bd(BD_t * bd);
int modman_rem_cfs(CFS_t * cfs);
int modman_rem_lfs(LFS_t * lfs);

/* Return the current usage count without changing it. */
uint32_t modman_query_bd(BD_t * bd);
uint32_t modman_query_cfs(CFS_t * cfs);
uint32_t modman_query_lfs(LFS_t * lfs);

/* Get the module name given when the module was added to modman. */
const char * modman_name_bd(BD_t * bd);
const char * modman_name_cfs(CFS_t * cfs);
const char * modman_name_lfs(LFS_t * lfs);

#endif /* __KUDOS_KFS_MODMAN_BD_H */
