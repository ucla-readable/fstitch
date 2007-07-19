#ifndef __KUDOS_KFS_UFS_SUPER_H
#define __KUDOS_KFS_UFS_SUPER_H

#include <kfs/oo.h>
#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/chdesc.h>

#include <kfs/ufs_base.h>

struct UFSmod_super;
typedef struct UFSmod_super UFSmod_super_t;

struct UFSmod_super {
	OBJECT(UFSmod_super_t);
	DECLARE(UFSmod_super_t, const struct UFS_Super *, read);
	DECLARE(UFSmod_super_t, int, write_time, int32_t time, chdesc_t ** head);
	DECLARE(UFSmod_super_t, int, write_cstotal, const struct UFS_csum * sum, chdesc_t ** head);
	DECLARE(UFSmod_super_t, int, write_fmod, int8_t fmod, chdesc_t ** head);
	DECLARE(UFSmod_super_t, int, write_clean, int8_t clean, chdesc_t ** head);
	DECLARE(UFSmod_super_t, int, write_ronly, int8_t ronly, chdesc_t ** head);
	DECLARE(UFSmod_super_t, int, write_fsmnt, const char * fsmnt, chdesc_t ** head);
	DECLARE(UFSmod_super_t, int, write_cgrotor, int32_t cgrotor, chdesc_t ** head);
	DECLARE(UFSmod_super_t, int, sync, chdesc_t ** head);
};

#define UFS_SUPER_INIT(ufs, module) { \
	OBJ_INIT(ufs, module); \
	ASSIGN(ufs, module, read); \
	ASSIGN(ufs, module, write_time); \
	ASSIGN(ufs, module, write_cstotal); \
	ASSIGN(ufs, module, write_fmod); \
	ASSIGN(ufs, module, write_clean); \
	ASSIGN(ufs, module, write_ronly); \
	ASSIGN(ufs, module, write_fsmnt); \
	ASSIGN(ufs, module, write_cgrotor); \
	ASSIGN(ufs, module, sync); \
}

#endif /* __KUDOS_KFS_UFS_SUPER_H */
