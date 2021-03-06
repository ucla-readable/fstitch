/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_MODULES_UFS_CG_H
#define __FSTITCH_MODULES_UFS_CG_H

#include <fscore/oo.h>
#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/patch.h>

#include <modules/ufs_lfs.h>

struct UFSmod_cg;
typedef struct UFSmod_cg UFSmod_cg_t;

struct UFSmod_cg {
	OBJECT(UFSmod_cg_t);
	DECLARE(UFSmod_cg_t, uint32_t, get_cylstart, int32_t num);
	DECLARE(UFSmod_cg_t, const struct UFS_cg *, read, int32_t num);
	DECLARE(UFSmod_cg_t, int, write_time, int32_t num, int32_t time, patch_t ** head);
	DECLARE(UFSmod_cg_t, int, write_cs, int32_t num, const struct UFS_csum * sum, patch_t ** head);
	DECLARE(UFSmod_cg_t, int, write_rotor, int32_t num, int32_t rotor, patch_t ** head);
	DECLARE(UFSmod_cg_t, int, write_frotor, int32_t num, int32_t frotor, patch_t ** head);
	DECLARE(UFSmod_cg_t, int, write_irotor, int32_t num, int32_t irotor, patch_t ** head);
	DECLARE(UFSmod_cg_t, int, write_frsum, int32_t num, const int32_t * frsum, patch_t ** head);
	DECLARE(UFSmod_cg_t, int, sync, int32_t num, patch_t ** head);
};

#define UFS_CG_INIT(ufs, module) { \
	OBJ_INIT(ufs, module); \
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

#endif /* __FSTITCH_MODULES_UFS_CG_H */
