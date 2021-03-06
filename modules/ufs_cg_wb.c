/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/vector.h>

#include <fscore/debug.h>
#include <fscore/sched.h>

#include <modules/ufs_cg_wb.h>

#define WB_TIME     0
#define WB_CS       1
#define WB_ROTOR    2
#define WB_FROTOR   3
#define WB_IROTOR   4
#define WB_FRSUM    5
#define WB_LAST     6

#define SYNC_PERIOD HZ

struct cyl_info
{
	bdesc_t * cgblock;
	uint32_t cgblock_number;
	struct UFS_cg cgdata; /* In memory cylgrp */
	struct UFS_csum oldcgsum; /* On disk version of the summary */
	int32_t oldfrsum[UFS_MAXFRAG]; /* On disk version of the frsum */
	bool dirty[WB_LAST]; /* Keep track of what fields have been changed */
	uint32_t cylstart; /* Cylinder starting block number */
};

struct local_info
{
	UFSmod_cg_t ufsmod_cg;
	
	struct ufs_info * global_info;
	struct cyl_info * cg;
	int32_t ncg;
	bool syncing; /* Indicates whether to write to memory or disk */
};

const int frsum_size = sizeof(int32_t) * UFS_MAXFRAG;

static uint32_t ufs_cg_wb_get_cylstart(UFSmod_cg_t * object, int32_t num)
{
	struct local_info * linfo = (struct local_info *) object;

	if (num < 0 || num >= linfo->ncg)
		return 0; /* Is there a better error value? */

	return linfo->cg[num].cylstart;
}

static const struct UFS_cg * ufs_cg_wb_read(UFSmod_cg_t * object, int32_t num)
{
	struct local_info * linfo = (struct local_info *) object;

	if (num < 0 || num >= linfo->ncg)
		return NULL;

	assert(&linfo->cg[num].cgdata); /* Should never be NULL */
	return &linfo->cg[num].cgdata;
}

static int ufs_cg_wb_write_time(UFSmod_cg_t * object, int32_t num, int32_t time, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (num < 0 || num >= linfo->ncg)
		return -EINVAL;

	if (!linfo->syncing) {
		linfo->cg[num].cgdata.cg_time = time;
		linfo->cg[num].dirty[WB_TIME] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->cg[num].dirty[WB_TIME])
		return 0;

	r = patch_create_byte(linfo->cg[num].cgblock, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_cg *) NULL)->cg_time,
			sizeof(linfo->cg[num].cgdata.cg_time),
			&linfo->cg[num].cgdata.cg_time, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "cg timestamp");
	r = CALL(linfo->global_info->ubd, write_block, linfo->cg[num].cgblock, linfo->cg[num].cgblock_number);
	if (r < 0)
		return r;
	linfo->cg[num].dirty[WB_TIME] = 0;

	return 0;
}

static int ufs_cg_wb_write_cs(UFSmod_cg_t * object, int num, const struct UFS_csum * sum, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (num < 0 || num >= linfo->ncg)
		return -EINVAL;

	if (!linfo->syncing) {
		if (!sum)
			return -EINVAL;
		memcpy(&linfo->cg[num].cgdata.cg_cs, sum, sizeof(struct UFS_csum));
		linfo->cg[num].dirty[WB_CS] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->cg[num].dirty[WB_CS])
		return 0;

	r = patch_create_diff(linfo->cg[num].cgblock, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_cg *) NULL)->cg_cs,
			sizeof(struct UFS_csum), &linfo->cg[num].oldcgsum,
			&linfo->cg[num].cgdata.cg_cs, head);
	if (r < 0)
		return r;
	/* patch_create_diff() returns 0 for "no change" */
	if (*head && r > 0)
	{
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "cg checksum");
		r = CALL(linfo->global_info->ubd, write_block, linfo->cg[num].cgblock, linfo->cg[num].cgblock_number);
		if (r < 0)
			return r;
	}
	linfo->cg[num].dirty[WB_CS] = 0;
	/* Successfully wrote to disk, updating oldcgsum to reflect what should
	 * be on disk. */
	memcpy(&linfo->cg[num].oldcgsum, &linfo->cg[num].cgdata.cg_cs, sizeof(struct UFS_csum));

	return 0;
}

static int ufs_cg_wb_write_rotor(UFSmod_cg_t * object, int32_t num, int32_t rotor, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (num < 0 || num >= linfo->ncg)
		return -EINVAL;

	if (!linfo->syncing) {
		linfo->cg[num].cgdata.cg_rotor = rotor;
		linfo->cg[num].dirty[WB_ROTOR] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->cg[num].dirty[WB_ROTOR])
		return 0;

	r = patch_create_byte(linfo->cg[num].cgblock, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_cg *) NULL)->cg_rotor,
			sizeof(linfo->cg[num].cgdata.cg_rotor),
			&linfo->cg[num].cgdata.cg_rotor, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "cg rotor");
	r = CALL(linfo->global_info->ubd, write_block, linfo->cg[num].cgblock, linfo->cg[num].cgblock_number);
	if (r < 0)
		return r;
	linfo->cg[num].dirty[WB_ROTOR] = 0;

	return 0;
}

static int ufs_cg_wb_write_frotor(UFSmod_cg_t * object, int32_t num, int32_t frotor, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (num < 0 || num >= linfo->ncg)
		return -EINVAL;

	if (!linfo->syncing) {
		linfo->cg[num].cgdata.cg_frotor = frotor;
		linfo->cg[num].dirty[WB_FROTOR] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->cg[num].dirty[WB_FROTOR])
		return 0;

	r = patch_create_byte(linfo->cg[num].cgblock, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_cg *) NULL)->cg_frotor,
			sizeof(linfo->cg[num].cgdata.cg_frotor),
			&linfo->cg[num].cgdata.cg_frotor, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "cg frotor");
	r = CALL(linfo->global_info->ubd, write_block, linfo->cg[num].cgblock, linfo->cg[num].cgblock_number);
	if (r < 0)
		return r;
	linfo->cg[num].dirty[WB_FROTOR] = 0;

	return 0;
}

static int ufs_cg_wb_write_irotor(UFSmod_cg_t * object, int32_t num, int32_t irotor, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (num < 0 || num >= linfo->ncg)
		return -EINVAL;

	if (!linfo->syncing) {
		linfo->cg[num].cgdata.cg_irotor = irotor;
		linfo->cg[num].dirty[WB_IROTOR] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->cg[num].dirty[WB_IROTOR])
		return 0;

	r = patch_create_byte(linfo->cg[num].cgblock, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_cg *) NULL)->cg_irotor,
			sizeof(linfo->cg[num].cgdata.cg_irotor),
			&linfo->cg[num].cgdata.cg_irotor, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "cg irotor");
	r = CALL(linfo->global_info->ubd, write_block, linfo->cg[num].cgblock, linfo->cg[num].cgblock_number);
	if (r < 0)
		return r;
	linfo->cg[num].dirty[WB_IROTOR] = 0;

	return 0;
}

static int ufs_cg_wb_write_frsum(UFSmod_cg_t * object, int32_t num, const int32_t * frsum, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (num < 0 || num >= linfo->ncg)
		return -EINVAL;

	if (!linfo->syncing) {
		if (!frsum)
			return -EINVAL;
		memcpy(&linfo->cg[num].cgdata.cg_frsum, frsum, frsum_size);
		linfo->cg[num].dirty[WB_FRSUM] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->cg[num].dirty[WB_FRSUM])
		return 0;

	r = patch_create_diff(linfo->cg[num].cgblock, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_cg *) NULL)->cg_frsum, frsum_size,
			&linfo->cg[num].oldfrsum, &linfo->cg[num].cgdata.cg_frsum, head);
	if (r < 0)
		return r;
	/* patch_create_diff() returns 0 for "no change" */
	if(*head && r > 0)
	{
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "cg frsum");
		r = CALL(linfo->global_info->ubd, write_block, linfo->cg[num].cgblock, linfo->cg[num].cgblock_number);
		if (r < 0)
			return r;
	}
	linfo->cg[num].dirty[WB_FRSUM] = 0;
	/* Successfully wrote to disk, updating oldfrsum to reflect what should
	 * be on disk. */
	memcpy(&linfo->cg[num].oldfrsum, &linfo->cg[num].cgdata.cg_frsum, frsum_size);

	return 0;
}

/* Writes all outstanding changes to disk. Changes are hooked up in
 * parallel. */
static int ufs_cg_wb_sync(UFSmod_cg_t * object, int32_t num, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int i, r;
   	int begin, end;
	patch_t ** oldhead;
	vector_t * oldheads;

	if (!head)
		return -EINVAL;

	oldheads = vector_create();
	if (!oldheads)
		return -ENOMEM;

	if (num < 0 || num >= linfo->ncg) {
		begin = 0;
		end = linfo->ncg;
	}
	else {
		begin = num;
		end = num + 1;
	}

	linfo->syncing = 1;

	for (i = begin; i < end; i++) {
		if (linfo->cg[i].dirty[WB_TIME]) {
			oldhead = head;
			r = ufs_cg_wb_write_time(object, i, 0, oldhead);
			if (r < 0)
				goto exit;
			if (*oldhead) {
				r = vector_push_back(oldheads, *oldhead);
				if (r < 0)
					goto exit;
			}
		}
		if (linfo->cg[i].dirty[WB_CS]) {
			oldhead = head;
			r = ufs_cg_wb_write_cs(object, i, 0, oldhead);
			if (r < 0)
				goto exit;
			if (*oldhead) {
				r = vector_push_back(oldheads, *oldhead);
				if (r < 0)
					goto exit;
			}
		}
		if (linfo->cg[i].dirty[WB_ROTOR]) {
			oldhead = head;
			r = ufs_cg_wb_write_rotor(object, i, 0, oldhead);
			if (r < 0)
				goto exit;
			if (*oldhead) {
				r = vector_push_back(oldheads, *oldhead);
				if (r < 0)
					goto exit;
			}
		}
		if (linfo->cg[i].dirty[WB_FROTOR]) {
			oldhead = head;
			r = ufs_cg_wb_write_frotor(object, i, 0, oldhead);
			if (r < 0)
				goto exit;
			if (*oldhead) {
				r = vector_push_back(oldheads, *oldhead);
				if (r < 0)
					goto exit;
			}
		}
		if (linfo->cg[i].dirty[WB_IROTOR]) {
			oldhead = head;
			r = ufs_cg_wb_write_irotor(object, i, 0, oldhead);
			if (r < 0)
				goto exit;
			if (*oldhead) {
				r = vector_push_back(oldheads, *oldhead);
				if (r < 0)
					goto exit;
			}
		}
		if (linfo->cg[i].dirty[WB_FRSUM]) {
			oldhead = head;
			r = ufs_cg_wb_write_frsum(object, i, 0, oldhead);
			if (r < 0)
				goto exit;
			if (*oldhead) {
				r = vector_push_back(oldheads, *oldhead);
				if (r < 0)
					goto exit;
			}
		}
	}

	if (vector_size(oldheads))
	{
		r = patch_create_empty_array(NULL, head, vector_size(oldheads), (patch_t **) oldheads->elts);
		if (r < 0)
			goto exit;
	}
	r = 0;

exit:
	vector_destroy(oldheads);
	linfo->syncing = 0;
	return r;
}

static void ufs_cg_wb_sync_callback(void * arg)
{
	UFSmod_cg_t * object = (UFSmod_cg_t *) arg;
	struct local_info * linfo = (struct local_info *) object;
	patch_t * head = linfo->global_info->write_head ? *linfo->global_info->write_head : NULL;
	int r;

	r = ufs_cg_wb_sync(object, -1, &head);
	if (r < 0)
		printf("%s failed\n", __FUNCTION__);
}

static int ufs_cg_wb_destroy(UFSmod_cg_t * obj)
{
	struct local_info * linfo = (struct local_info *) obj;
	int i, r;

	r = sched_unregister(ufs_cg_wb_sync_callback, obj);
	if (r < 0)
		return r;

	for (i = 0; i < linfo->ncg; i++)
		bdesc_release(&linfo->cg[i].cgblock);

	sfree(linfo->cg, sizeof(struct cyl_info) * linfo->ncg);
	memset(linfo, 0, sizeof(*linfo));
	free(linfo);

	return 0;
}

UFSmod_cg_t * ufs_cg_wb(struct ufs_info * info)
{
	UFSmod_cg_t * obj;
	struct local_info * linfo;
	int i, r;
   
	if (!info)
		return NULL;
	linfo = malloc(sizeof(*linfo));
	if (!linfo)
		return NULL;
	obj = &linfo->ufsmod_cg;
	linfo->global_info = info;

	assert(info->parts.p_super);
	const struct UFS_Super * super = CALL(info->parts.p_super, read);
	linfo->ncg = super->fs_ncg;

	linfo->cg = smalloc(sizeof(struct cyl_info) * linfo->ncg);
	if (!linfo->cg) {
		free(linfo);
		return NULL;
	}

	for (i = 0; i < linfo->ncg; i++) {
		linfo->cg[i].cylstart = super->fs_fpg * i + super->fs_cgoffset * (i & ~super->fs_cgmask);
		linfo->cg[i].cgblock_number = linfo->cg[i].cylstart + super->fs_cblkno;
	}

	for (i = 0; i < linfo->ncg; i++) {
		linfo->cg[i].cgblock = CALL(info->ubd, read_block,
				linfo->cg[i].cgblock_number, 1, NULL);
		if (!linfo->cg[i].cgblock)
			goto read_block_failed;
		bdesc_retain(linfo->cg[i].cgblock);
	}

	for (i = 0; i < linfo->ncg; i++) {
		memcpy(&linfo->cg[i].cgdata, bdesc_data(linfo->cg[i].cgblock), sizeof(struct UFS_cg));
		memcpy(&linfo->cg[i].oldcgsum, &linfo->cg[i].cgdata.cg_cs, sizeof(struct UFS_csum));
		memcpy(&linfo->cg[i].oldfrsum, &linfo->cg[i].cgdata.cg_frsum, frsum_size);
		memset(&linfo->cg[i].dirty, 0, sizeof(linfo->cg[i].dirty));
	}
	linfo->syncing = 0;

	UFS_CG_INIT(obj, ufs_cg_wb);

	r = sched_register(ufs_cg_wb_sync_callback, obj, SYNC_PERIOD);
	assert(r >= 0);

	return obj;

read_block_failed:
	i--;
	while (i >= 0) {
		bdesc_release(&linfo->cg[i].cgblock);
		i--;
	}
	sfree(linfo->cg, sizeof(struct cyl_info) * linfo->ncg);
	free(linfo);
	return NULL;
}

