#ifndef __KUDOS_KFS_MODMAN_BD_H
#define __KUDOS_KFS_MODMAN_BD_H

#include <kfs/bd.h>
#include <kfs/cfs.h>
#include <kfs/lfs.h>

int modman_init(void);

int modman_add_bd(BD_t * bd, const char * name);
int modman_add_cfs(CFS_t * cfs, const char * name);
int modman_add_lfs(LFS_t * lfs, const char * name);

int modman_inc_bd(BD_t * bd);
int modman_inc_cfs(CFS_t * cfs);
int modman_inc_lfs(LFS_t * lfs);

int modman_dec_bd(BD_t * bd);
int modman_dec_cfs(CFS_t * cfs);
int modman_dec_lfs(LFS_t * lfs);

int modman_rem_bd(BD_t * bd);
int modman_rem_cfs(CFS_t * cfs);
int modman_rem_lfs(LFS_t * lfs);

int modman_query_bd(BD_t * bd);
int modman_query_cfs(CFS_t * cfs);
int modman_query_lfs(LFS_t * lfs);

const char * modman_name_bd(BD_t * bd);
const char * modman_name_cfs(CFS_t * cfs);
const char * modman_name_lfs(LFS_t * lfs);

#endif /* __KUDOS_KFS_MODMAN_BD_H */
