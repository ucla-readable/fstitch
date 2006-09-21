#include <inc/error.h>
#include <lib/assert.h>
#include <lib/jiffies.h>
#include <lib/stdio.h>
#include <lib/string.h>

#include <kfs/sched.h>
#include <kfs/ext2_super_wb.h>

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
	EXT2_Super_t super; /* In memory super block */
	EXT2_Super_t oldsuper;	//used to diff
	int dirty;
};

static const struct EXT2_Super * ext2_super_wb_read(EXT2mod_super_t * object)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	assert(&linfo->super); /* Should never be NULL */
	return &linfo->super;
}

//inodes is a delta:
static int ext2_super_wb_inodes(EXT2mod_super_t * object, int inodes)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	if(inodes) {
		linfo->dirty = 1;
		linfo->super.s_free_inodes_count += inodes;
	}
	return 0;
}

//blocks is a delta:
static int ext2_super_wb_blocks(EXT2mod_super_t * object, int blocks)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	if(blocks) {
		linfo->dirty = 1;
		linfo->super.s_free_blocks_count += blocks;
	}
	return 0;
}

//TODO this should get its own time!
static int ext2_super_wb_wtime(EXT2mod_super_t * object, int time)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	linfo->dirty = 1;
	linfo->super.s_wtime = time;;
	return 0;
}

static int ext2_super_wb_mount_time(EXT2mod_super_t * object, int mount_time)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	linfo->dirty = 1;
	linfo->super.s_mtime = mount_time;;
	return 0;
}

static int ext2_super_wb_sync(EXT2mod_super_t * object, chdesc_t ** head)
{
	
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);	
	int r;

	if (!head)
		return -E_INVAL;

	if (!linfo->dirty)
		return 0;

	r = chdesc_create_diff(linfo->super_block, linfo->global_info->ubd,
			       1024,
			12 * sizeof(uint32_t), &linfo->oldsuper, &linfo->super,
			head);
	if (r < 0)
		return r;
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block);
	if (r < 0)
		return r;
	linfo->oldsuper.s_free_blocks_count = linfo->super.s_free_blocks_count;
	linfo->oldsuper.s_free_inodes_count = linfo->super.s_free_inodes_count;
	linfo->oldsuper.s_wtime = linfo->super.s_wtime;
	//TODO sync mtime, wtime
	//linfo->oldsuper.s_free_blocks_count = linfo->super.s_free_blocks_count;
	//linfo->oldsuper.s_free_blocks_count = linfo->super.s_free_blocks_count;
	linfo->dirty = 0;
	return 0;
}


static void ext2_super_wb_sync_callback(void * arg)
{
	EXT2mod_super_t * object = (EXT2mod_super_t *) arg;
	int r;
	chdesc_t * head = NULL;

	r = ext2_super_wb_sync(object, &head);
	if (r < 0)
		printf("%s failed\n", __FUNCTION__);
}

static int ext2_super_wb_get_config(void * object, int level, char * string,
		size_t length)
{
	if (length >= 1)
		string[0] = 0;
		return 0;
}

static int ext2_super_wb_destroy(EXT2mod_super_t * obj)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(obj);
	int r;

	r = sched_unregister(ext2_super_wb_sync_callback, obj);
	if (r < 0)
		return r;

	bdesc_release(&linfo->super_block);
	free(OBJLOCAL(obj));
	memset(obj, 0, sizeof(*obj));
	free(obj);

	return 0;
}

static int ext2_super_wb_get_status(void * object, int level, char * string,
		size_t length)
{
	if (length >= 1)
		string[0] = 0;
	return 0;
}

EXT2mod_super_t * ext2_super_wb(struct lfs_info * info)
{
	EXT2mod_super_t * obj;
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

	/* the superblock is 1024 bytes from the start of the partition */
	linfo->super_block = CALL(info->ubd, read_block, 0, 1);
	if (!linfo->super_block)
	{
		printf("Unable to read superblock!\n");
		free(obj);
		free(linfo);
		return NULL;
	}

	bdesc_retain(linfo->super_block);
	memcpy(&linfo->super, linfo->super_block->ddesc->data + 1024, sizeof(struct EXT2_Super));
	memcpy(&linfo->oldsuper, linfo->super_block->ddesc->data + 1024, sizeof(struct EXT2_Super));	
	linfo->dirty = 0;

	EXT2_SUPER_INIT(obj, ext2_super_wb, linfo);
	r = sched_register(ext2_super_wb_sync_callback, obj, SYNC_PERIOD);
	assert(r >= 0);

	return obj;
}

