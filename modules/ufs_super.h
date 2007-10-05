/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_MODULES_UFS_SUPER_H
#define __FSTITCH_MODULES_UFS_SUPER_H

#include <fscore/oo.h>
#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/patch.h>

#include <modules/ufs_lfs.h>

struct UFSmod_super;
typedef struct UFSmod_super UFSmod_super_t;

struct UFSmod_super {
	OBJECT(UFSmod_super_t);
	DECLARE(UFSmod_super_t, const struct UFS_Super *, read);
	DECLARE(UFSmod_super_t, int, write_time, int32_t time, patch_t ** head);
	DECLARE(UFSmod_super_t, int, write_cstotal, const struct UFS_csum * sum, patch_t ** head);
	DECLARE(UFSmod_super_t, int, write_fmod, int8_t fmod, patch_t ** head);
	DECLARE(UFSmod_super_t, int, write_clean, int8_t clean, patch_t ** head);
	DECLARE(UFSmod_super_t, int, write_ronly, int8_t ronly, patch_t ** head);
	DECLARE(UFSmod_super_t, int, write_fsmnt, const char * fsmnt, patch_t ** head);
	DECLARE(UFSmod_super_t, int, write_cgrotor, int32_t cgrotor, patch_t ** head);
	DECLARE(UFSmod_super_t, int, sync, patch_t ** head);
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

#endif /* __FSTITCH_MODULES_UFS_SUPER_H */
