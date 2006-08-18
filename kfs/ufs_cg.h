#ifndef __KUDOS_KFS_UFS_CG_H
#define __KUDOS_KFS_UFS_CG_H

#include <lib/types.h>

#include <kfs/oo.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>

#include <kfs/ufs_base.h>

struct UFSmod_cg;
typedef struct UFSmod_cg UFSmod_cg_t;

struct UFSmod_cg {
	OBJECT(UFSmod_cg_t);
	DECLARE(UFSmod_cg_t, uint32_t, get_cylstart, int32_t num);
	DECLARE(UFSmod_cg_t, const struct UFS_cg *, read, int32_t num);
	DECLARE(UFSmod_cg_t, int, write_time, int32_t num, int32_t time, chdesc_t ** head);
	DECLARE(UFSmod_cg_t, int, write_cs, int32_t num, const struct UFS_csum * sum, chdesc_t ** head);
	DECLARE(UFSmod_cg_t, int, write_rotor, int32_t num, int32_t rotor, chdesc_t ** head);
	DECLARE(UFSmod_cg_t, int, write_frotor, int32_t num, int32_t frotor, chdesc_t ** head);
	DECLARE(UFSmod_cg_t, int, write_irotor, int32_t num, int32_t irotor, chdesc_t ** head);
	DECLARE(UFSmod_cg_t, int, write_frsum, int32_t num, const int32_t * frsum, chdesc_t ** head);
	DECLARE(UFSmod_cg_t, int, sync, int32_t num, chdesc_t ** head);
};

#define UFS_CG_INIT(ufs, module, info) { \
	OBJ_INIT(ufs, module, info); \
	ASSIGN(ufs, module, get_cylstart); \
	ASSIGN(ufs, module, read); \
	ASSIGN(ufs, module, write_time); \
	ASSIGN(ufs, module, write_cs); \
	ASSIGN(ufs, module, write_rotor); \
	ASSIGN(ufs, module, write_frotor); \
	ASSIGN(ufs, module, write_irotor); \
	ASSIGN(ufs, module, write_frsum); \
	ASSIGN(ufs, module, sync); \
}

#endif /* __KUDOS_KFS_UFS_CG_H */
