#include <inc/stdio.h>
#include <inc/error.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/barrier.h>
#include <kfs/blockman.h>
#include <kfs/mirror_bd.h>

#define disk0_bad (info->bad_disk == 0)
#define disk1_bad (info->bad_disk == 1)
#define disk0_good (info->bad_disk != 0)
#define disk1_good (info->bad_disk != 1)
#define both_good (info->bad_disk == -1)
#define disk_bad (info->bad_disk != -1)

/* The mirror device must be a barrier. We will use barrier_multiple_forward(). */

struct mirror_info {
	BD_t * bd[2];
	uint32_t numblocks;
	uint16_t blocksize, atomicsize;
	uint16_t level;
	uint8_t stride; // Disk reads alternate every 512 * pow(2,stride) bytes
	int8_t bad_disk; // {none, disk 0, disk 1} => {-1, 0, 1}
	blockman_t * blockman;
};

static int mirror_bd_get_config(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(bd);
	switch(level)
	{
		case CONFIG_VERBOSE:
			snprintf(string, length, "disks: 2, count: %d, blocksize: %d, stride: %d", info->numblocks, info->blocksize, info->stride);
			break;
		case CONFIG_BRIEF:
			snprintf(string, length, "disks: 2");
			break;
		case CONFIG_NORMAL:
		default:
			snprintf(string, length, "disks: 2, count: %d", info->numblocks);
	}
	return 0;
}

static int mirror_bd_get_status(void * object, int level, char * string, size_t length)
{
	BD_t * bd = (BD_t *) object;
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(bd);
	switch(level)
	{
		case STATUS_VERBOSE:
			snprintf(string, length, "health: %s", both_good ? "OK" : disk0_bad ? "Disk 0 FAILURE" : "Disk 1 FAILURE");
			break;
		case STATUS_BRIEF:
			snprintf(string, length, "%s", both_good ? "OK" : disk0_bad ? "Disk 0 FAILURE" : "Disk 1 FAILURE");
			break;
		case STATUS_NORMAL:
		default:
			snprintf(string, length, "health: %s", both_good ? "OK" : disk0_bad ? "Disk 0 FAILURE" : "Disk 1 FAILURE");
	}
	return 0;
}

static uint32_t mirror_bd_get_numblocks(BD_t * object)
{
	return ((struct mirror_info *) OBJLOCAL(object))->numblocks;
}

static uint16_t mirror_bd_get_blocksize(BD_t * object)
{
	return ((struct mirror_info *) OBJLOCAL(object))->blocksize;
}

static uint16_t mirror_bd_get_atomicsize(BD_t * object)
{
	return ((struct mirror_info *) OBJLOCAL(object))->atomicsize;
}

static void label_drive_bad(BD_t * object, int disk)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);

	if(disk != 0 && disk != 1) // Should never happen...
		return;

	if(disk_bad)
		return;

	info->bad_disk = disk;

	if(info->bd[disk])
		modman_dec_bd(info->bd[disk], object);
	info->bd[disk] = NULL;
	printf("mirror_bd: disk %d is bad!!!\n", disk);

}

static bdesc_t * try_read(BD_t * object, uint32_t number, int disk)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	if(disk != 0 && disk != 1) // Should never happen...
		return NULL;

	bdesc = CALL(info->bd[disk], read_block, number);

	/* just be nice and retry same device*/
	if(!bdesc)
		bdesc = CALL(info->bd[disk], read_block, number);

	return bdesc;
}

static bdesc_t * mirror_bd_read_block(BD_t * object, uint32_t number)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);
	int diskno = (number >> info->stride) & 1;
	bdesc_t * block;
	bdesc_t * orig;
	
	block = blockman_managed_lookup(info->blockman, number);
	if(block)
		return block;
	
	/* make sure it's a valid block */
	if(number >= info->numblocks)
		return NULL;
	
	block = bdesc_alloc(number, info->blocksize);
	if(!block)
		return NULL;
	bdesc_autorelease(block);
	
	if(disk0_bad)
		orig = try_read(object, number, 1);
	else if(disk1_bad)
		orig = try_read(object, number, 0);
	else
	{
		orig = try_read(object, number, diskno);

		/* two strikes and you're out! */
		if(!orig)
		{
			orig = try_read(object, number, 1 - diskno);
			/* now we know disk[diskno] is 'bad' */
			if(orig)
				label_drive_bad(object, diskno);
		}
	}
	
	if(!orig)
		return NULL;
	
	memcpy(block->ddesc->data, orig->ddesc->data, info->blocksize);
	
	if(blockman_managed_add(info->blockman, block) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	return block;
}

/* we are a barrier, so just synthesize it if it's not already in this zone */
static bdesc_t * mirror_bd_synthetic_read_block(BD_t * object, uint32_t number, bool * synthetic)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	
	bdesc = blockman_managed_lookup(info->blockman, number);
	if(bdesc)
	{
		*synthetic = 0;
		return bdesc;
	}
	
	/* make sure it's a valid block */
	if(number >= info->numblocks)
		return NULL;
	
	bdesc = bdesc_alloc(number, info->blocksize);
	if(!bdesc)
		return NULL;
	bdesc_autorelease(bdesc);
	
	if(blockman_managed_add(info->blockman, bdesc) < 0)
		/* kind of a waste of the read... but we have to do it */
		return NULL;
	
	*synthetic = 1;
	
	return bdesc;
}

static int mirror_bd_cancel_block(BD_t * object, uint32_t number)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);
	datadesc_t * ddesc = blockman_lookup(info->blockman, number);
	if(ddesc)
		blockman_remove(ddesc);
	return 0;
}

static int mirror_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);
	int value0 = -1, value1 = -1;
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->numblocks)
		return -E_INVAL;
	
	if(disk1_bad)
		value0 = barrier_simple_forward(info->bd[0], block->number, object, block);
	else if(disk0_bad)
		value1 = barrier_simple_forward(info->bd[1], block->number, object, block);
	else
	{
		multiple_forward_t forwards[2];
		forwards[0].target = info->bd[0];
		forwards[0].number = block->number;
		forwards[1].target = info->bd[1];
		forwards[1].number = block->number;
		/* barrier_multiple_forward can't tell us which disk has an issue */
		value0 = barrier_multiple_forward(forwards, 2, object, block);
		value1 = value0;
	}
	
	if(disk1_bad)
		return value0;
	if(disk0_bad)
		return value1;

	if(value0 == value1) // Consensus
		return value0;
	// We're biased against disk 1
	if(value1 < 0)
	{
		label_drive_bad(object, 1);
		return value0;
	}
	if(value0 < 0)
	{
		label_drive_bad(object, 0);
		return value1;
	}
	return value0|value1;
}

static int mirror_bd_sync(BD_t * object, uint32_t block, chdesc_t * ch)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);
	int value0 = -1, value1 = -1;
	
	if(block == SYNC_FULL_DEVICE)
	{
		if(disk1_bad)
			return CALL(info->bd[0], sync, SYNC_FULL_DEVICE, NULL);
		else if(disk0_bad)
			return CALL(info->bd[1], sync, SYNC_FULL_DEVICE, NULL);

		value0 = CALL(info->bd[0], sync, SYNC_FULL_DEVICE, NULL);
		value1 = CALL(info->bd[1], sync, SYNC_FULL_DEVICE, NULL);

		if(value0 == value1) // Consensus
			return value0;
		// We're biased against disk 1 since we have no idea which disk really failed in the case of both returning errors
		if(value1 < 0)
		{
			label_drive_bad(object, 1);
			return value0;
		}
		if(value0 < 0)
		{
			label_drive_bad(object, 0);
			return value1;
		}
		return value0|value1;
	}
	
	/* make sure it's a valid block */
	if(block >= info->numblocks)
		return -E_INVAL;
	
	if(disk1_bad)
		value0 = CALL(info->bd[0], sync, block, ch);
	else if(disk0_bad)
		value1 = CALL(info->bd[1], sync, block, ch);
	else
	{
		/* we can't sort out what change descriptors to
		 * actually use, so just sync the whole block */
		value0 = CALL(info->bd[0], sync, block, NULL);
		value1 = CALL(info->bd[1], sync, block, NULL);
	}
	
	if(disk1_bad)
		return value0;
	if(disk0_bad)
		return value1;

	if(value0 == value1) // Consensus
		return value0;
	// We're biased against disk 1
	if(value1 < 0)
	{
		label_drive_bad(object, 1);
		return value0;
	}
	if(value0 < 0)
	{
		label_drive_bad(object, 0);
		return value1;
	}
	return value0|value1;
}

static uint16_t mirror_bd_get_devlevel(BD_t * object)
{
	return ((struct mirror_info *) OBJLOCAL(object))->level;
}

static int mirror_bd_destroy(BD_t * bd)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(bd);
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	if(info->bd[1])
		modman_dec_bd(info->bd[1], bd);
	if(info->bd[0])
		modman_dec_bd(info->bd[0], bd);
	free(OBJLOCAL(bd));
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * mirror_bd(BD_t * disk0, BD_t * disk1, uint8_t stride)
{
	struct mirror_info * info;
	uint32_t numblocks0 = 0, numblocks1 = 0;
	uint16_t blocksize, blocksize0 = 0, blocksize1 = 0;
	uint16_t atomicsize0 = 0, atomicsize1 = 0;
	uint16_t devlevel0 = 0, devlevel1 = 0;
	int8_t bad_disk = -1;
	BD_t * bd;

	if(!disk0 && !disk1)
		return NULL;

	if(disk0 == disk1)
		disk1 = NULL;

	if(!disk0)
		bad_disk = 0;
	else if(!disk1)
		bad_disk = 1;

	if(bad_disk != 0)
	{
		numblocks0 = CALL(disk0, get_numblocks);
		blocksize0 = CALL(disk0, get_blocksize);
		atomicsize0 = CALL(disk0, get_atomicsize);
		devlevel0 = CALL(disk0, get_devlevel);
	}
	if(bad_disk != 1)
	{
		numblocks1 = CALL(disk1, get_numblocks);
		blocksize1 = CALL(disk1, get_blocksize);
		atomicsize1 = CALL(disk1, get_atomicsize);
		devlevel1 = CALL(disk1, get_devlevel);
	}
	
	/* block sizes must be the same */
	if((bad_disk == -1) && (blocksize0 != blocksize1))
		return NULL;
	else if(bad_disk == 1)
		blocksize = blocksize0;
	else
		blocksize = blocksize1;
	
	bd = malloc(sizeof(*bd));
	if(!bd)
		return NULL;
	
	info = malloc(sizeof(struct mirror_info));
	if(!info)
	{
		free(bd);
		return NULL;
	}
	
	info->blockman = blockman_create();
	if(!info->blockman)
	{
		free(info);
		free(bd);
		return NULL;
	}
	
	BD_INIT(bd, mirror_bd, info);
	OBJMAGIC(bd) = MIRROR_BD_MAGIC;
	
	info->bd[0] = disk0;
	info->bd[1] = disk1;
	info->blocksize = blocksize;
	info->stride = stride;
	info->bad_disk = bad_disk;

	/* we can use minimum number of blocks and atomic size safely */
	if(bad_disk == -1)
	{
		info->numblocks = MIN(numblocks0, numblocks1);
		info->atomicsize = MIN(atomicsize0, atomicsize1);
		info->level = MAX(devlevel0, devlevel1);
	}
	else if(bad_disk == 1)
	{
		info->numblocks = numblocks0;
		info->atomicsize = atomicsize0;
		info->level = devlevel0;
	}
	else
	{
		info->numblocks = numblocks1;
		info->atomicsize = atomicsize1;
		info->level = devlevel1;
	}

	if(modman_add_anon_bd(bd, __FUNCTION__))
		goto error_add;
	if(disk0_good && modman_inc_bd(disk0, bd, "Disk 0") < 0)
		goto error_inc_1;
	if(disk1_good && modman_inc_bd(disk1, bd, "Disk 1") < 0)
		goto error_inc_2;
	
	return bd;
	
error_inc_2:
	if(disk0_good)
		modman_dec_bd(disk0, bd);
error_inc_1:
	modman_rem_bd(bd);
error_add:
	DESTROY(bd);
	return NULL;
}

int mirror_bd_add_device(BD_t * bd, BD_t * newdevice)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(bd);
	uint32_t numblocks;
	uint16_t blocksize, atomicsize, devlevel;
	int8_t good_disk = 1 - info->bad_disk;
	int i, r;

	if(OBJMAGIC(bd) != MIRROR_BD_MAGIC)
		return -E_INVAL;

	if(both_good)
		return -E_INVAL;

	if(!newdevice || newdevice == info->bd[good_disk])
		return -E_INVAL;

	printf("mirror_bd: trying to replace disk %d\n", info->bad_disk);

	blocksize = CALL(newdevice, get_blocksize);
	if(blocksize != info->blocksize)
	{
		printf("mirror_bd: blocksize is different\n");
		return -E_INVAL;
	}

	atomicsize = CALL(newdevice, get_atomicsize);
	if(atomicsize < info->atomicsize)
	{
		printf("mirror_bd: atomic size too small\n");
		return -E_INVAL;
	}

	numblocks = CALL(newdevice, get_numblocks);
	if(numblocks < info->numblocks)
	{
		printf("mirror_bd: disk not big enough\n");
		return -E_INVAL;
	}

	devlevel = CALL(newdevice, get_devlevel);
	if(devlevel > info->level)
	{
		printf("mirror_bd: device level too large\n");
		return -E_INVAL;
	}

	if(disk0_bad)
		r = modman_inc_bd(newdevice, bd, "Disk 0");
	else
		r = modman_inc_bd(newdevice, bd, "Disk 1");

	if(r < 0)
		return r;

	printf("mirror_bd: disk looks good, syncing...\n");

	/* push a new autorelease pool */
	r = bdesc_autorelease_pool_push();
	if(r < 0)
	{
		modman_dec_bd(newdevice, bd);
		return r;
	}
	
	for(i = 0; i < info->numblocks; i++)
	{
		bool synthetic;
		bdesc_t * source;
		bdesc_t * destination;
		chdesc_t * head = NULL;
		chdesc_t * tail = NULL;
		
		/* periodically pop/push the autorelease pool */
		if(!(i & 255) && i)
		{
			bdesc_autorelease_pool_pop();
			r = bdesc_autorelease_pool_push();
			if(r < 0)
			{
				modman_dec_bd(newdevice, bd);
				return r;
			}
		}
		
		source = CALL(info->bd[good_disk], read_block, i);
		if(!source)
		{
			printf("mirror_bd: uh oh, erroring reading block %d on sync\n", i);
			modman_dec_bd(newdevice, bd);
			return -E_UNSPECIFIED;
		}

		destination = CALL(newdevice, synthetic_read_block, i, &synthetic);
		if(!destination)
		{
			printf("mirror_bd: uh oh, erroring getting block %d on sync\n", i);
			modman_dec_bd(newdevice, bd);
			return -E_UNSPECIFIED;
		}
		
		r = chdesc_create_full(destination, newdevice, source->ddesc->data, &head, &tail);
		if(r < 0)
		{
			if(synthetic)
				CALL(newdevice, cancel_block, i);
			modman_dec_bd(newdevice, bd);
			return r;
		}
		
		r = CALL(newdevice, write_block, destination);
		if(r < 0)
		{
			printf("mirror_bd: uh oh, erroring writing block %d on sync\n", i);
			modman_dec_bd(newdevice, bd);
			return r;
		}
	}
	
	/* pop the local autorelease pool */
	bdesc_autorelease_pool_pop();
	
	r = CALL(info->bd[good_disk], sync, SYNC_FULL_DEVICE, NULL);
	if(r < 0)
	{
		modman_dec_bd(newdevice, bd);
		return r;
	}
	r = CALL(newdevice, sync, SYNC_FULL_DEVICE, NULL);
	if(r < 0)
	{
		modman_dec_bd(newdevice, bd);
		return r;
	}

	info->bd[info->bad_disk] = newdevice;
	info->bad_disk = -1;

	printf("mirror_bd: sync done!\n");

	return 0;
}

int mirror_bd_remove_device(BD_t * bd, int diskno)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(bd);
	int r;

	if(OBJMAGIC(bd) != MIRROR_BD_MAGIC)
		return -E_INVAL;

	if(diskno < 0 || diskno > 1)
		return -E_INVAL;

	r = mirror_bd_sync(bd, SYNC_FULL_DEVICE, NULL);
	if(r < 0)
		return r;

	if(disk_bad)
		return -E_INVAL;

	info->bad_disk = diskno;
	modman_dec_bd(info->bd[diskno], bd);
	info->bd[diskno] = NULL;

	printf("mirror_bd: removed disk %d\n", diskno);

	return 0;
}

