#include <lib/error.h>
#include <lib/assert.h>
#include <lib/jiffies.h>
#include <lib/stdio.h>
#include <lib/string.h>
#include <lib/hash_map.h>

#include <kfs/sched.h>
#include <kfs/ext2_super_wb.h>

#define SYNC_PERIOD HZ

uint32_t EXT2_BLOCK_SIZE;
uint32_t EXT2_DESC_PER_BLOCK;

struct local_info
{
	LFS_t * global_lfs;
	struct ext2_info * global_info;
	bdesc_t * super_block;
	EXT2_Super_t super; /* In memory super block */
	EXT2_group_desc_t * groups;	
	bdesc_t * gdescs;
	int ngroups;
	int super_dirty;
	int gdesc_dirty;
};

static EXT2_Super_t * ext2_super_wb_read(EXT2mod_super_t * object)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	assert(&linfo->super); /* Should never be NULL */
	return &linfo->super;
}

static EXT2_group_desc_t * ext2_super_wb_read_gdescs(EXT2mod_super_t * object)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	assert(&linfo->groups); /* Should never be NULL */
	return linfo->groups;
}

//inodes is a delta:
static int ext2_super_wb_inodes(EXT2mod_super_t * object, int inodes)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	if(inodes) {
		linfo->super_dirty = 1;
		linfo->super.s_free_inodes_count += inodes;
	}
	return 0;
}

//blocks is a delta:
static int ext2_super_wb_blocks(EXT2mod_super_t * object, int blocks)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	if(blocks) {
		linfo->super_dirty = 1;
		linfo->super.s_free_blocks_count += blocks;
	}
	return 0;
}

//TODO this should get its own time!
static int ext2_super_wb_wtime(EXT2mod_super_t * object, int time)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	linfo->super_dirty = 1;
	linfo->super.s_wtime = time;;
	return 0;
}

static int ext2_super_wb_mount_time(EXT2mod_super_t * object, int mount_time)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	linfo->super_dirty = 1;
	linfo->super.s_mtime = mount_time;;
	return 0;
}

//the inputs here are deltas!
//remember, you dont need to diff!
static int ext2_super_wb_write_gdesc(EXT2mod_super_t * object, uint32_t group, int32_t blocks, int32_t inodes, int32_t dirs)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);
	linfo->gdesc_dirty = 1;
	linfo->groups[group].bg_free_blocks_count += blocks;
	linfo->groups[group].bg_free_inodes_count += inodes;
	linfo->groups[group].bg_used_dirs_count += dirs;
	return 0;
}

static int ext2_super_wb_sync(EXT2mod_super_t * object, chdesc_t ** head)
{
	
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);	
	int r;

	if (!head)
		return -E_INVAL;

	if (!linfo->super_dirty)
		return 0;

	r = chdesc_create_diff(linfo->super_block, linfo->global_info->ubd, 1024, 12 * sizeof(uint32_t), 
				linfo->super_block->ddesc->data + 1024, &linfo->super, head);
	if (r < 0)
		return r;
	r = CALL(linfo->global_info->ubd, write_block, linfo->super_block);
	if (r < 0)
		return r;
	linfo->super_dirty = 0;
	return 0;
}

static int ext2_super_wb_gdesc_sync(EXT2mod_super_t * object, chdesc_t ** head)
{
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);	
	int r;

	if (!head)
		return -E_INVAL;

	if (!linfo->gdesc_dirty)
		return 0;

	r = chdesc_create_diff(linfo->gdescs, linfo->global_info->ubd, 0, linfo->ngroups * sizeof(EXT2_group_desc_t), 
				linfo->gdescs->ddesc->data, linfo->groups, head);
	if (r < 0)
		return r;
	r = CALL(linfo->global_info->ubd, write_block, linfo->gdescs);
	if (r < 0)
		return r;

	linfo->gdesc_dirty = 0; 
	return 0;
}


static void ext2_super_wb_sync_callback(void * arg)
{
	EXT2mod_super_t * object = (EXT2mod_super_t *) arg;
	struct local_info * linfo = (struct local_info *) OBJLOCAL(object);	
	chdesc_t * head = CALL(linfo->global_lfs, get_write_head);
	int r;

	r = ext2_super_wb_sync(object, &head);
	if (r < 0)
		printf("%s failed\n", __FUNCTION__);
	r = ext2_super_wb_gdesc_sync(object, &head);
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
	bdesc_release(&linfo->gdescs);
	free(linfo->groups);
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

EXT2mod_super_t * ext2_super_wb(LFS_t * lfs, struct ext2_info * info)
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
	linfo->global_lfs = lfs;
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
	linfo->super_dirty = 0;

	//now load the gdescs
	uint32_t block;
	EXT2_BLOCK_SIZE = (1024 << linfo->super.s_log_block_size);
	EXT2_DESC_PER_BLOCK = EXT2_BLOCK_SIZE / sizeof(EXT2_group_desc_t);

	int ngroups = (linfo->super.s_blocks_count / linfo->super.s_blocks_per_group);
	if (linfo->super.s_blocks_count % linfo->super.s_blocks_per_group != 0)
		ngroups++;
	linfo->ngroups = ngroups;
	linfo->groups = calloc(ngroups, sizeof(EXT2_group_desc_t));
	if (!linfo->groups)
		goto wb_fail1;
	block = 1;
#warning FIXME counts other than 1 are incompatible with the journal_bd module
	linfo->gdescs = CALL(info->ubd, read_block, block, (ngroups / EXT2_DESC_PER_BLOCK) + 1);
	if(!linfo->gdescs)
		goto wb_fail1;
	if (memcpy(linfo->groups, linfo->gdescs->ddesc->data, sizeof(EXT2_group_desc_t) * ngroups) == NULL)
		goto wb_fail2;
	bdesc_retain(linfo->gdescs);
	linfo->gdesc_dirty = 0;

	EXT2_SUPER_INIT(obj, ext2_super_wb, linfo);
	r = sched_register(ext2_super_wb_sync_callback, obj, SYNC_PERIOD);
	assert(r >= 0);

	return obj;

 wb_fail2:
	bdesc_release(&linfo->super_block);
 wb_fail1:
	bdesc_release(&linfo->gdescs);
	free(obj);
	free(linfo);
	return NULL;
}
