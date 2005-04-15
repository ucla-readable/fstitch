#include <inc/stdio.h>
#include <inc/types.h>
#include <inc/malloc.h>
#include <inc/string.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/modman.h>
#include <kfs/mirror_bd.h>

#define disk0_bad (info->bad_disk == 0)
#define disk1_bad (info->bad_disk == 1)
#define disk0_good (info->bad_disk != 0)
#define disk1_good (info->bad_disk != 1)
#define both_good (info->bad_disk == -1)

#define MIRROR_BD_MAGIC 0x888BDA1D

struct mirror_info {
	BD_t * bd[2];
	uint32_t numblocks;
	uint16_t blocksize, atomicsize;
	uint16_t stride; // Disk reads alternate every 512 * pow(2,stride) bytes
	int16_t bad_disk; // {none, disk 0, disk 1} => {-1, 0, 1}
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
			snprintf(string, length, "health: %s", (info->bad_disk == -1) ? "OK" : (info->bad_disk == 0) ? "Disk 0 FAILURE" : "Disk 1 FAILURE");
			break;
		case STATUS_BRIEF:
			snprintf(string, length, "%s", (info->bad_disk == -1) ? "OK" : (info->bad_disk == 0) ? "Disk 0 FAILURE" : "Disk 1 FAILURE");
			break;
		case STATUS_NORMAL:
		default:
			snprintf(string, length, "health: %s", (info->bad_disk == -1) ? "OK" : (info->bad_disk == 0) ? "Disk 0 FAILURE" : "Disk 1 FAILURE");
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

	if (disk != 0 && disk != 1) // Should never happen...
		return;

	if (info->bad_disk != -1)
		return;

	info->bad_disk = disk;

	if (info->bd[disk])
		modman_dec_bd(info->bd[disk], object);
	info->bd[disk] = NULL;
	printf("mirror_bd: disk %d is bad!!!\n", disk);

}

static bdesc_t * try_read(BD_t * object, uint32_t number, int disk)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);
	bdesc_t * bdesc;
	if (disk != 0 && disk != 1) // Should never happen...
		return NULL;

	bdesc = CALL(info->bd[disk], read_block, number);

	/* just be nice and retry same device*/
	if (!bdesc)
		bdesc = CALL(info->bd[disk], read_block, number);

	return bdesc;
}

static bdesc_t * mirror_bd_read_block(BD_t * object, uint32_t number)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);
	int diskno = (number >> info->stride) & 1;
	bdesc_t * bdesc = NULL;
	
	assert (disk0_good || disk1_good);

	/* make sure it's a valid block */
	if(number >= info->numblocks)
		return NULL;
	
	if (disk0_bad)
		bdesc = try_read(object, number, 1);
	else if (disk1_bad)
		bdesc = try_read(object, number, 0);
	else {
		bdesc = try_read(object, number, diskno);

		/* two strikes and you're out! */
		if (!bdesc) {
			bdesc = try_read(object, number, 1 - diskno);
			/* now we know disk[diskno] is 'bad' */
			if (bdesc)
				label_drive_bad(object, diskno);
		}
	}
	
	if(!bdesc)
		return NULL;
	
	/* ensure we can alter the structure without conflict */
	if(bdesc_alter(&bdesc))
	{
		bdesc_drop(&bdesc);
		return NULL;
	}
	
	/* adjust the block descriptor to match the md */
	bdesc->bd = object;
	
	return bdesc;
}

static int mirror_bd_write_block(BD_t * object, bdesc_t * block)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);
	uint32_t refs = block->refs;
	int value0 = -1, value1 = -1;
	
	assert (disk0_good || disk1_good);

	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->numblocks)
		return -E_INVAL;
	
	/* Important logic:
	 * If somebody has a reference to this bdesc (i.e. refs > 0), then that
	 * entity must be above us. This is because all entities that have
	 * references to a bdesc think it has the same BD (in this case, us),
	 * and everybody below us uses a different BD. So, if there is a
	 * reference to this bdesc, it will still exist after the call to
	 * write_block below. If not, the principle of caller-drop applies to
	 * the bdesc and we do not have to (nor can we) de-translate it.
	 * However, we must still set the translated flag, as it is the cue to
	 * bdesc_retain() to notify the dependency manager of the change. In the
	 * case where translated = 1 and refs = 0, bdesc_retain simply clears
	 * the translated flag. */
	block->translated++;

	if (disk1_bad) {
		block->bd = info->bd[0];
		value0 = CALL(block->bd, write_block, block);
	}
	else if (disk0_bad) {
		block->bd = info->bd[1];
		value1 = CALL(block->bd, write_block, block);
	}
	else {
		block->bd = info->bd[0];
		value0 = CALL(block->bd, write_block, block);
		block->bd = info->bd[1];
		value1 = CALL(block->bd, write_block, block);
	}
	
	if(refs)
	{
		block->bd = object;
		block->translated--;
	}
	
	if (disk1_bad)
		return value0;
	if (disk0_bad)
		return value1;

	if (value0 == value1) // Consensus
		return value0;
	// We're biased against disk 1
	if (value1 < 0) {
		label_drive_bad(object, 1);
		return value0;
	}
	if (value0 < 0) {
		label_drive_bad(object, 0);
		return value1;
	}
	else
		return value0|value1;
}

static int mirror_bd_sync(BD_t * object, bdesc_t * block)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(object);
	uint32_t refs, number;
	int value0 = -1, value1 = -1;
	
	assert (disk0_good || disk1_good);

	if(!block)
	{
		if (disk1_bad)
			return CALL(info->bd[0], sync, NULL);
		else if (disk0_bad)
			return CALL(info->bd[1], sync, NULL);

		value0 = CALL(info->bd[0], sync, NULL);
		value1 = CALL(info->bd[1], sync, NULL);

		if (value0 == value1) // Consensus
			return value0;
		// We're biased against disk 1 since we have no idea which disk really failed in the case of both returning errors
		if (value1 < 0) {
			label_drive_bad(object, 1);
			return value0;
		}
		if (value0 < 0) {
			label_drive_bad(object, 0);
			return value1;
		}
		else
			return value0|value1;
	}
	
	/* save reference count and number */
	refs = block->refs;
	number = block->number;
	
	/* make sure this is the right block device */
	if(block->bd != object)
		return -E_INVAL;
	
	/* make sure it's a whole block */
	if(block->ddesc->length != info->blocksize)
		return -E_INVAL;
	
	/* make sure it's a valid block */
	if(block->number >= info->numblocks)
		return -E_INVAL;
	
	block->translated++;

	if (disk1_bad) {
		block->bd = info->bd[0];
		value0 = CALL(block->bd, sync, block);
	}
	else if (disk0_bad) {
		block->bd = info->bd[1];
		value1 = CALL(block->bd, sync, block);
	}
	else {
		block->bd = info->bd[0];
		value0 = CALL(block->bd, sync, block);
		block->bd = info->bd[1];
		value1 = CALL(block->bd, sync, block);
	}
	
	if(refs)
	{
		block->bd = object;
		block->translated--;
	}

	if (disk1_bad)
		return value0;
	if (disk0_bad)
		return value1;

	if (value0 == value1) // Consensus
		return value0;
	// We're biased against disk 1
	if (value1 < 0) {
		label_drive_bad(object, 1);
		return value0;
	}
	if (value0 < 0) {
		label_drive_bad(object, 0);
		return value1;
	}
	else
		return value0|value1;
}

static int mirror_bd_destroy(BD_t * bd)
{
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(bd);
	int r = modman_rem_bd(bd);
	if(r < 0)
		return r;
	if (info->bd[1])
		modman_dec_bd(info->bd[1], bd);
	if (info->bd[0])
		modman_dec_bd(info->bd[0], bd);
	free(OBJLOCAL(bd));
	memset(bd, 0, sizeof(*bd));
	free(bd);
	return 0;
}

BD_t * mirror_bd(BD_t * disk0, BD_t * disk1, uint32_t stride)
{
	struct mirror_info * info;
	uint32_t numblocks0 = 0;
	uint32_t numblocks1 = 0;
	uint16_t blocksize;
	uint16_t blocksize0 = 0;
	uint16_t blocksize1 = 0;
	uint16_t atomicsize0 = 0;
	uint16_t atomicsize1 = 0;
	int16_t bad_disk = -1;
	BD_t * bd;

	if (!disk0 && !disk1)
		return NULL;

	if (disk0 == disk1)
		disk1 = NULL;

	if (!disk0)
		bad_disk = 0;
	else if (!disk1)
		bad_disk = 1;

	if (bad_disk != 0)
		numblocks0 = CALL(disk0, get_numblocks);
	if (bad_disk != 1)
		numblocks1 = CALL(disk1, get_numblocks);
	if (bad_disk != 0)
		blocksize0 = CALL(disk0, get_blocksize);
	if (bad_disk != 1)
		blocksize1 = CALL(disk1, get_blocksize);
	if (bad_disk != 0)
		atomicsize0 = CALL(disk0, get_atomicsize);
	if (bad_disk != 1)
		atomicsize1 = CALL(disk1, get_atomicsize);
	
	/* block sizes must be the same */
	if ((bad_disk == -1) && (blocksize0 != blocksize1))
		return NULL;
	else if (bad_disk == 1)
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
	OBJLOCAL(bd) = info;
	
	OBJFLAGS(bd) = 0;
	OBJMAGIC(bd) = MIRROR_BD_MAGIC;
	OBJASSIGN(bd, mirror_bd, get_config);
	OBJASSIGN(bd, mirror_bd, get_status);
	ASSIGN(bd, mirror_bd, get_numblocks);
	ASSIGN(bd, mirror_bd, get_blocksize);
	ASSIGN(bd, mirror_bd, get_atomicsize);
	ASSIGN(bd, mirror_bd, read_block);
	ASSIGN(bd, mirror_bd, write_block);
	ASSIGN(bd, mirror_bd, sync);
	DESTRUCTOR(bd, mirror_bd, destroy);
	
	info->bd[0] = disk0;
	info->bd[1] = disk1;
	/* we can use minimum number of blocks and atomic size safely */
	info->blocksize = blocksize;
	info->stride = stride;
	info->bad_disk = bad_disk;

	if (bad_disk == -1)
		info->numblocks = MIN(numblocks0, numblocks1);
	else if (bad_disk == 1)
		info->numblocks = numblocks0;
	else
		info->numblocks = numblocks1;

	if (both_good)
		info->atomicsize = MIN(atomicsize0, atomicsize1);
	else if (disk1_bad)
		info->atomicsize = atomicsize0;
	else
		info->atomicsize = atomicsize1;
	
	if(modman_add_anon_bd(bd, __FUNCTION__))
		goto error_add;
	if(disk0_good && modman_inc_bd(disk0, bd, "Disk 0") < 0)
		goto error_inc_1;
	if(disk1_good && modman_inc_bd(disk1, bd, "Disk 1") < 0)
		goto error_inc_2;
	
	return bd;
	
error_inc_2:
	if (disk0_good)
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
	int numblocks, blocksize, atomicsize, i, r, good_disk = 1 - info->bad_disk;
	bdesc_t * bdesc;

	if (OBJMAGIC(bd) != MIRROR_BD_MAGIC)
		return -E_INVAL;

	if (info->bad_disk == -1)
		return -E_INVAL;

	if (!newdevice || newdevice == info->bd[good_disk])
		return -E_INVAL;

	printf("mirror_bd: trying to replace disk %d\n", info->bad_disk);

	blocksize = CALL(newdevice, get_blocksize);
	if (blocksize != info->blocksize) {
		printf("mirror_bd: blocksize is different\n");
		return -E_INVAL;
	}

	atomicsize = CALL(newdevice, get_atomicsize);
	if (atomicsize < info->atomicsize) {
		printf("mirror_bd: atomic size too small\n");
		return -E_INVAL;
	}

	numblocks = CALL(newdevice, get_numblocks);
	if (numblocks < info->numblocks) {
		printf("mirror_bd: disk not big enough\n");
		return -E_INVAL;
	}

	if (info->bad_disk == 0)
		r = modman_inc_bd(newdevice, bd, "Disk 0");
	else
		r = modman_inc_bd(newdevice, bd, "Disk 1");

	if (r < 0)
		return r;

	printf("mirror_bd: disk looks good, syncing...\n");

	for (i = 0; i < info->numblocks; i++) {
		bdesc = CALL(info->bd[good_disk], read_block, i);
		if (!bdesc) {
			printf("mirror_bd: uh oh, erroring reading block %d on sync\n", i);
			return -E_UNSPECIFIED;
		}

		bdesc->bd = newdevice;
		r = CALL(newdevice, write_block, bdesc);
		if (r < 0) {
			printf("mirror_bd: uh oh, erroring writing block %d on sync\n", i);
			return r;
		}
	}

	info->bd[info->bad_disk] = newdevice;
	info->bad_disk = -1;

	printf("mirror_bd: sync done!\n");

	return 0;
}

int mirror_bd_remove_device(BD_t * bd, int diskno) {
	struct mirror_info * info = (struct mirror_info *) OBJLOCAL(bd);
	int r;

	if (OBJMAGIC(bd) != MIRROR_BD_MAGIC)
		return -E_INVAL;

	if (diskno < 0 || diskno > 1)
		return -E_INVAL;

	r = mirror_bd_sync(bd, NULL);
	if (r < 0)
		return r;

	if (info->bad_disk != -1)
		return -E_INVAL;

	info->bad_disk = diskno;
	modman_dec_bd(info->bd[diskno], bd);
	info->bd[diskno] = NULL;

	printf("mirror_bd: removed disk %d\n", diskno);

	return 0;
}

