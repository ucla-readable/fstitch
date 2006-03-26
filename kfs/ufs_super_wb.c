#include <inc/error.h>
#include <lib/assert.h>
#include <lib/jiffies.h>
#include <lib/stdio.h>
#include <lib/string.h>

#include <kfs/sched.h>
#include <kfs/ufs_super_wb.h>

#define WB_TIME     0
#define WB_CSTOTAL  1
#define WB_FMOD     2
#define WB_CLEAN    3
#define WB_RONLY    4
#define WB_FSMNT    5
#define WB_CGROTOR  6
#define WB_LAST     7

#define SYNC_PERIOD HZ

struct local_info
{
	struct lfs_info * global_info;
	bdesc_t * super_block;
	struct UFS_Super super; /* In memory super block */
	struct UFS_csum oldsum; /* On disk version of the summary */
	bool dirty[WB_LAST]; /* Keep track of what fields have been changed */
	bool syncing; /* Indicates whether to write to memory or disk */
};

static const struct UFS_Super * ufs_super_wb_read(UFSmod_super_t * object)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	assert(&linfo->super); /* Should never be NULL */
	return &linfo->super;
}

static int ufs_super_wb_write_time(UFSmod_super_t * object, int32_t time, chdesc_t ** head)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	int r;

	if (!linfo->syncing) {
		linfo->super.fs_time = time;
		linfo->dirty[WB_TIME] = 1;
		return 0;
	}

	if (!head)
		return -E_INVAL;
	if (!linfo->dirty[WB_TIME])
		return 0;

	r = chdesc_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_time,
			sizeof(linfo->super.fs_time), &linfo->super.fs_time, head);
	if (r < 0)
		return r;
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block);
	if (r < 0)
		return r;
	linfo->dirty[WB_TIME] = 0;

	return 0;
}

static int ufs_super_wb_write_cstotal(UFSmod_super_t * object, const struct UFS_csum * sum, chdesc_t ** head)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	int r;

	if (!linfo->syncing) {
		if (!sum)
			return -E_INVAL;
		memcpy(&linfo->super.fs_cstotal, sum, sizeof(struct UFS_csum));
		linfo->dirty[WB_CSTOTAL] = 1;
		return 0;
	}

	if (!head)
		return -E_INVAL;
	if (!linfo->dirty[WB_CSTOTAL])
		return 0;

	r = chdesc_create_diff(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_cstotal,
			sizeof(struct UFS_csum), &linfo->oldsum, &linfo->super.fs_cstotal,
			head);
	if (r < 0)
		return r;
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block);
	if (r < 0)
		return r;
	linfo->dirty[WB_CSTOTAL] = 0;
	/* Successfully wrote to disk, updating oldsum to reflect what should
	 * be on disk. */
	memcpy(&linfo->oldsum, &linfo->super.fs_cstotal, sizeof(struct UFS_csum));

	return 0;
}

static int ufs_super_wb_write_fmod(UFSmod_super_t * object, int8_t fmod, chdesc_t ** head)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	int r;

	if (!linfo->syncing) {
		linfo->super.fs_fmod = fmod;
		linfo->dirty[WB_FMOD] = 1;
		return 0;
	}

	if (!head)
		return -E_INVAL;
	if (!linfo->dirty[WB_FMOD])
		return 0;

	r = chdesc_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_fmod,
			sizeof(linfo->super.fs_fmod), &linfo->super.fs_fmod, head);
	if (r < 0)
		return r;
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block);
	if (r < 0)
		return r;
	linfo->dirty[WB_FMOD] = 0;

	return 0;
}

static int ufs_super_wb_write_clean(UFSmod_super_t * object, int8_t clean, chdesc_t ** head)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	int r;

	if (!linfo->syncing) {
		linfo->super.fs_clean = clean;
		linfo->dirty[WB_CLEAN] = 1;
		return 0;
	}

	if (!head)
		return -E_INVAL;
	if (!linfo->dirty[WB_CLEAN])
		return 0;

	r = chdesc_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_clean,
			sizeof(linfo->super.fs_clean), &linfo->super.fs_clean, head);
	if (r < 0)
		return r;
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block);
	if (r < 0)
		return r;
	linfo->dirty[WB_CLEAN] = 0;

	return 0;
}

static int ufs_super_wb_write_ronly(UFSmod_super_t * object, int8_t ronly, chdesc_t ** head)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	int r;

	if (!linfo->syncing) {
		linfo->super.fs_ronly = ronly;
		linfo->dirty[WB_RONLY] = 1;
		return 0;
	}

	if (!head)
		return -E_INVAL;
	if (!linfo->dirty[WB_RONLY])
		return 0;

	r = chdesc_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_ronly,
			sizeof(linfo->super.fs_ronly), &linfo->super.fs_ronly, head);
	if (r < 0)
		return r;
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block);
	if (r < 0)
		return r;
	linfo->dirty[WB_RONLY] = 0;

	return 0;
}

static int ufs_super_wb_write_fsmnt(UFSmod_super_t * object, const char * fsmnt, chdesc_t ** head)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	int r, len;

	if (!linfo->syncing) {
		len = strlen(fsmnt);
		if (len >= UFS_MAXMNTLEN)
			return -E_INVAL;
		strcpy(linfo->super.fs_fsmnt, fsmnt);
		linfo->dirty[WB_FSMNT] = 1;
		return 0;
	}

	if (!head)
		return -E_INVAL;
	if (!linfo->dirty[WB_FSMNT])
		return 0;

	r = chdesc_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_fsmnt,
			strlen(linfo->super.fs_fsmnt) + 1, &linfo->super.fs_fsmnt, head);
	if (r < 0)
		return r;
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block);
	if (r < 0)
		return r;
	linfo->dirty[WB_FSMNT] = 0;

	return 0;
}

static int ufs_super_wb_write_cgrotor(UFSmod_super_t * object, int32_t cgrotor, chdesc_t ** head)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	int r;

	if (!linfo->syncing) {
		linfo->super.fs_cgrotor = cgrotor;
		linfo->dirty[WB_CGROTOR] = 1;
		return 0;
	}

	if (!head)
		return -E_INVAL;
	if (!linfo->dirty[WB_CGROTOR])
		return 0;

	r = chdesc_create_byte(linfo->super_block, linfo->global_info->ubd,
			(uint16_t) &((struct UFS_Super *) NULL)->fs_cgrotor,
			sizeof(linfo->super.fs_cgrotor), &linfo->super.fs_cgrotor, head);
	if (r < 0)
		return r;
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block);
	if (r < 0)
		return r;
	linfo->dirty[WB_CGROTOR] = 0;

	return 0;
}

/* Writes all outstanding changes to disk. Changes are hooked up in
 * parallel. */
static int ufs_super_wb_sync(UFSmod_super_t * object, chdesc_t ** head)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	int r;
	uint32_t sync_count = 0;
	chdesc_t ** oldhead;
	chdesc_t * noophead;

	if (!head)
		return -E_INVAL;

	noophead = chdesc_create_noop(NULL, NULL);
	if (!noophead)
		return -E_NO_MEM;
	chdesc_claim_noop(noophead);

	linfo->syncing = 1;

	if (linfo->dirty[WB_TIME]) {
		oldhead = head;
		r = ufs_super_wb_write_time(object, 0, oldhead);
		if (r < 0)
			goto sync_failed;
		if (*oldhead) {
			r = chdesc_add_depend(noophead, *oldhead);
			if (r < 0)
				goto sync_failed;
		}
		sync_count++;
	}
	if (linfo->dirty[WB_CSTOTAL]) {
		oldhead = head;
		r = ufs_super_wb_write_cstotal(object, 0, oldhead);
		if (r < 0)
			goto sync_failed;
		if (*oldhead) {
			r = chdesc_add_depend(noophead, *oldhead);
			if (r < 0)
				goto sync_failed;
		}
		sync_count++;
	}
	if (linfo->dirty[WB_FMOD]) {
		oldhead = head;
		r = ufs_super_wb_write_fmod(object, 0, oldhead);
		if (*oldhead) {
			r = chdesc_add_depend(noophead, *oldhead);
			if (r < 0)
				goto sync_failed;
		}
		if (r < 0)
			goto sync_failed;
		sync_count++;
	}
	if (linfo->dirty[WB_CLEAN]) {
		oldhead = head;
		r = ufs_super_wb_write_clean(object, 0, oldhead);
		if (*oldhead) {
			r = chdesc_add_depend(noophead, *oldhead);
			if (r < 0)
				goto sync_failed;
		}
		if (r < 0)
			goto sync_failed;
		sync_count++;
	}
	if (linfo->dirty[WB_RONLY]) {
		oldhead = head;
		r = ufs_super_wb_write_ronly(object, 0, oldhead);
		if (*oldhead) {
			r = chdesc_add_depend(noophead, *oldhead);
			if (r < 0)
				goto sync_failed;
		}
		if (r < 0)
			goto sync_failed;
		sync_count++;
	}
	if (linfo->dirty[WB_FSMNT]) {
		oldhead = head;
		r = ufs_super_wb_write_fsmnt(object, 0, oldhead);
		if (*oldhead) {
			r = chdesc_add_depend(noophead, *oldhead);
			if (r < 0)
				goto sync_failed;
		}
		if (r < 0)
			goto sync_failed;
		sync_count++;
	}
	if (linfo->dirty[WB_CGROTOR]) {
		oldhead = head;
		r = ufs_super_wb_write_cgrotor(object, 0, oldhead);
		if (*oldhead) {
			r = chdesc_add_depend(noophead, *oldhead);
			if (r < 0)
				goto sync_failed;
		}
		if (r < 0)
			goto sync_failed;
		sync_count++;
	}

	r = 0;

sync_failed:
	if (sync_count)
		*head = noophead;
	chdesc_autorelease_noop(noophead);
	linfo->syncing = 0;
	return r;
}

static void ufs_super_wb_sync_callback(void * arg)
{
	UFSmod_super_t * object = (UFSmod_super_t *) arg;
	int r;
	chdesc_t * head = NULL;

	r = ufs_super_wb_sync(object, &head);
	if (r < 0)
		printf("%s failed\n", __FUNCTION__);
}

static int ufs_super_wb_get_config(void * object, int level, char * string,
		size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ufs_super_wb_get_status(void * object, int level, char * string,
		size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}

static int ufs_super_wb_destroy(UFSmod_super_t * obj)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(obj);

	bdesc_release(&linfo->super_block);
	free(OBJLOCAL(obj));
	memset(obj, 0, sizeof(*obj));
	free(obj);

	return 0;
}

UFSmod_super_t * ufs_super_wb(struct lfs_info * info)
{
	UFSmod_super_t * obj;
	struct local_info * linfo;
	int r;
   
	if (!info)
		return NULL;
	obj = malloc(sizeof(*obj));
	if (!obj)
		return NULL;
	linfo = malloc(sizeof(struct local_info));
	if (!linfo) {
		free(obj);
		return NULL;
	}
	linfo->global_info = info;

	/* the superblock is in sector 16 */
	linfo->super_block = CALL(info->ubd, read_block, 4, 1);
	if (!linfo->super_block)
	{
		printf("Unable to read superblock!\n");
		free(obj);
		free(linfo);
		return NULL;
	}

	bdesc_retain(linfo->super_block);
	memcpy(&linfo->super, linfo->super_block->ddesc->data, sizeof(struct UFS_Super));
	memcpy(&linfo->oldsum, &linfo->super.fs_cstotal, sizeof(struct UFS_csum));
	memset(&linfo->dirty, 0, sizeof(linfo->dirty));
	linfo->syncing = 0;

	UFS_SUPER_INIT(obj, ufs_super_wb, linfo);

	r = sched_register(ufs_super_wb_sync_callback, obj, SYNC_PERIOD);
	assert(r >= 0);

	return obj;
}

