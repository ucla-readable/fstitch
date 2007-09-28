/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/jiffies.h>
#include <lib/vector.h>

#include <fscore/debug.h>
#include <fscore/sched.h>

#include <modules/ufs_super_wb.h>

#define WB_TIME     0
#define WB_CSTOTAL  1
#define WB_FMOD     2
#define WB_CLEAN    3
#define WB_RONLY    4
#define WB_FSMNT    5
#define WB_CGROTOR  6
#define WB_LAST     7

#define SYNC_PERIOD HZ

/* the superblock is in sector 16 */
#define SUPER_NUMBER	4

struct local_info
{
	UFSmod_super_t ufs;
	
	struct ufs_info * global_info;
	bdesc_t * super_block;
	struct UFS_Super super; /* In memory super block */
	struct UFS_csum oldsum; /* On disk version of the summary */
	bool dirty[WB_LAST]; /* Keep track of what fields have been changed */
	bool syncing; /* Indicates whether to write to memory or disk */
};

static const struct UFS_Super * ufs_super_wb_read(UFSmod_super_t * object)
{
	struct local_info * linfo = (struct local_info *) object;
	assert(&linfo->super); /* Should never be NULL */
	return &linfo->super;
}

static int ufs_super_wb_write_time(UFSmod_super_t * object, int32_t time, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (!linfo->syncing) {
		linfo->super.fs_time = time;
		linfo->dirty[WB_TIME] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->dirty[WB_TIME])
		return 0;

	r = patch_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_time,
			sizeof(linfo->super.fs_time), &linfo->super.fs_time, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "superblock timestamp");
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block, SUPER_NUMBER);
	if (r < 0)
		return r;
	linfo->dirty[WB_TIME] = 0;

	return 0;
}

static int ufs_super_wb_write_cstotal(UFSmod_super_t * object, const struct UFS_csum * sum, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (!linfo->syncing) {
		if (!sum)
			return -EINVAL;
		memcpy(&linfo->super.fs_cstotal, sum, sizeof(struct UFS_csum));
		linfo->dirty[WB_CSTOTAL] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->dirty[WB_CSTOTAL])
		return 0;

	r = patch_create_diff(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_cstotal,
			sizeof(struct UFS_csum), &linfo->oldsum, &linfo->super.fs_cstotal,
			head);
	if (r < 0)
		return r;
	/* patch_create_diff() returns 0 for "no change" */
	if (*head && r > 0)
	{
		FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "superblock CStotal");
		r = CALL(linfo->global_info->ubd, write_block, linfo->super_block, SUPER_NUMBER);
		if (r < 0)
			return r;
	}
	linfo->dirty[WB_CSTOTAL] = 0;
	/* Successfully wrote to disk, updating oldsum to reflect what should
	 * be on disk. */
	memcpy(&linfo->oldsum, &linfo->super.fs_cstotal, sizeof(struct UFS_csum));

	return 0;
}

static int ufs_super_wb_write_fmod(UFSmod_super_t * object, int8_t fmod, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (!linfo->syncing) {
		linfo->super.fs_fmod = fmod;
		linfo->dirty[WB_FMOD] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->dirty[WB_FMOD])
		return 0;

	r = patch_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_fmod,
			sizeof(linfo->super.fs_fmod), &linfo->super.fs_fmod, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "superblock fmod");
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block, SUPER_NUMBER);
	if (r < 0)
		return r;
	linfo->dirty[WB_FMOD] = 0;

	return 0;
}

static int ufs_super_wb_write_clean(UFSmod_super_t * object, int8_t clean, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (!linfo->syncing) {
		linfo->super.fs_clean = clean;
		linfo->dirty[WB_CLEAN] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->dirty[WB_CLEAN])
		return 0;

	r = patch_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_clean,
			sizeof(linfo->super.fs_clean), &linfo->super.fs_clean, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "superblock clean");
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block, SUPER_NUMBER);
	if (r < 0)
		return r;
	linfo->dirty[WB_CLEAN] = 0;

	return 0;
}

static int ufs_super_wb_write_ronly(UFSmod_super_t * object, int8_t ronly, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (!linfo->syncing) {
		linfo->super.fs_ronly = ronly;
		linfo->dirty[WB_RONLY] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->dirty[WB_RONLY])
		return 0;

	r = patch_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_ronly,
			sizeof(linfo->super.fs_ronly), &linfo->super.fs_ronly, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "superblock readonly");
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block, SUPER_NUMBER);
	if (r < 0)
		return r;
	linfo->dirty[WB_RONLY] = 0;

	return 0;
}

static int ufs_super_wb_write_fsmnt(UFSmod_super_t * object, const char * fsmnt, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r, len;

	if (!linfo->syncing) {
		len = strlen(fsmnt);
		if (len >= UFS_MAXMNTLEN)
			return -EINVAL;
		strcpy((char *) linfo->super.fs_fsmnt, fsmnt);
		linfo->dirty[WB_FSMNT] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->dirty[WB_FSMNT])
		return 0;

	r = patch_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_fsmnt,
			strlen((char *) linfo->super.fs_fsmnt) + 1, &linfo->super.fs_fsmnt, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "superblock FSmount");
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block, SUPER_NUMBER);
	if (r < 0)
		return r;
	linfo->dirty[WB_FSMNT] = 0;

	return 0;
}

static int ufs_super_wb_write_cgrotor(UFSmod_super_t * object, int32_t cgrotor, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	int r;

	if (!linfo->syncing) {
		linfo->super.fs_cgrotor = cgrotor;
		linfo->dirty[WB_CGROTOR] = 1;
		return 0;
	}

	if (!head)
		return -EINVAL;
	if (!linfo->dirty[WB_CGROTOR])
		return 0;

	r = patch_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_cgrotor,
			sizeof(linfo->super.fs_cgrotor), &linfo->super.fs_cgrotor, head);
	if (r < 0)
		return r;
	FSTITCH_DEBUG_SEND(FDB_MODULE_INFO, FDB_INFO_PATCH_LABEL, *head, "superblock CGrotor");
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block, SUPER_NUMBER);
	if (r < 0)
		return r;
	linfo->dirty[WB_CGROTOR] = 0;

	return 0;
}

/* Writes all outstanding changes to disk. Changes are hooked up in
 * parallel. */
static int ufs_super_wb_sync(UFSmod_super_t * object, patch_t ** head)
{
	struct local_info * linfo = (struct local_info *) object;
	patch_t ** oldhead;
	vector_t * oldheads;
	int r;

	if (!head)
		return -EINVAL;

	oldheads = vector_create();
	if (!oldheads)
		return -ENOMEM;

	linfo->syncing = 1;

	if (linfo->dirty[WB_TIME]) {
		oldhead = head;
		r = ufs_super_wb_write_time(object, 0, oldhead);
		if (r < 0)
			goto exit;
		if (*oldhead) {
			r = vector_push_back(oldheads, *oldhead);
			if (r < 0)
				goto exit;
		}
	}
	if (linfo->dirty[WB_CSTOTAL]) {
		oldhead = head;
		r = ufs_super_wb_write_cstotal(object, 0, oldhead);
		if (r < 0)
			goto exit;
		if (*oldhead) {
			r = vector_push_back(oldheads, *oldhead);
			if (r < 0)
				goto exit;
		}
	}
	if (linfo->dirty[WB_FMOD]) {
		oldhead = head;
		r = ufs_super_wb_write_fmod(object, 0, oldhead);
		if (*oldhead) {
			r = vector_push_back(oldheads, *oldhead);
			if (r < 0)
				goto exit;
		}
		if (r < 0)
			goto exit;
	}
	if (linfo->dirty[WB_CLEAN]) {
		oldhead = head;
		r = ufs_super_wb_write_clean(object, 0, oldhead);
		if (*oldhead) {
			r = vector_push_back(oldheads, *oldhead);
			if (r < 0)
				goto exit;
		}
		if (r < 0)
			goto exit;
	}
	if (linfo->dirty[WB_RONLY]) {
		oldhead = head;
		r = ufs_super_wb_write_ronly(object, 0, oldhead);
		if (*oldhead) {
			r = vector_push_back(oldheads, *oldhead);
			if (r < 0)
				goto exit;
		}
		if (r < 0)
			goto exit;
	}
	if (linfo->dirty[WB_FSMNT]) {
		oldhead = head;
		r = ufs_super_wb_write_fsmnt(object, 0, oldhead);
		if (*oldhead) {
			r = vector_push_back(oldheads, *oldhead);
			if (r < 0)
				goto exit;
		}
		if (r < 0)
			goto exit;
	}
	if (linfo->dirty[WB_CGROTOR]) {
		oldhead = head;
		r = ufs_super_wb_write_cgrotor(object, 0, oldhead);
		if (*oldhead) {
			r = vector_push_back(oldheads, *oldhead);
			if (r < 0)
				goto exit;
		}
		if (r < 0)
			goto exit;
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

static void ufs_super_wb_sync_callback(void * arg)
{
	UFSmod_super_t * object = (UFSmod_super_t *) arg;
	struct local_info * linfo = (struct local_info *) object;
	patch_t * head = linfo->global_info->write_head ? *linfo->global_info->write_head : NULL;
	int r;

	r = ufs_super_wb_sync(object, &head);
	if (r < 0)
		printf("%s failed\n", __FUNCTION__);
}

static int ufs_super_wb_destroy(UFSmod_super_t * obj)
{
	struct local_info * linfo = (struct local_info *) obj;
	int r;

	r = sched_unregister(ufs_super_wb_sync_callback, obj);
	if (r < 0)
		return r;

	bdesc_release(&linfo->super_block);
	memset(linfo, 0, sizeof(*linfo));
	free(linfo);

	return 0;
}

UFSmod_super_t * ufs_super_wb(struct ufs_info * info)
{
	UFSmod_super_t * obj;
	struct local_info * linfo;
	int r;
   
	if (!info)
		return NULL;
	linfo = malloc(sizeof(*linfo));
	if (!linfo)
		return NULL;
	obj = &linfo->ufs;
	linfo->global_info = info;

	/* the superblock is in sector 16 */
	linfo->super_block = CALL(info->ubd, read_block, SUPER_NUMBER, 1, NULL);
	if (!linfo->super_block)
	{
		printf("Unable to read superblock!\n");
		free(linfo);
		return NULL;
	}

	bdesc_retain(linfo->super_block);
	memcpy(&linfo->super, bdesc_data(linfo->super_block), sizeof(struct UFS_Super));
	memcpy(&linfo->oldsum, &linfo->super.fs_cstotal, sizeof(struct UFS_csum));
	memset(&linfo->dirty, 0, sizeof(linfo->dirty));
	linfo->syncing = 0;

	UFS_SUPER_INIT(obj, ufs_super_wb);

	r = sched_register(ufs_super_wb_sync_callback, obj, SYNC_PERIOD);
	assert(r >= 0);

	return obj;
}
