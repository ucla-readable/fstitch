#include <lib/platform.h>
#include <lib/vector.h>

#include <kfs/bd.h>
#include <kfs/bdesc.h>
#include <kfs/partition_bd.h>
#include <kfs/bsd_ptable.h>

#include <lib/disklabel.h>

struct bsdpart {
	uint32_t start, length;
	uint8_t type;
};

struct disklabel_info {
	BD_t * bd;
	int count;
	struct bsdpart parts[BSDLABEL_MAXLABELS];
};

#define SECTSIZE 512

void * bsd_ptable_init(BD_t * bd)
{
	struct disklabel_info * info;
	struct disklabel * label;
	bdesc_t * mbr;
	int i, j;
	uint32_t maxblocks;
	uint32_t offset;

	if(!bd)
		return NULL;

	/* make sure the block size is SECTSIZE */
	if (bd->blocksize != SECTSIZE)
		return NULL;

	info = malloc(sizeof(*info));
	if (!info)
		return NULL;

	/* read the partition table */
	mbr = CALL(bd, read_block, BSDLABEL_LABELSECTOR, 1, NULL);
	if (!mbr)
		goto bsd_init_error;
	label = (struct disklabel *) &bdesc_data(mbr)[BSDLABEL_LABELOFFSET];
	maxblocks = bd->numblocks;

	// TODO check d_checksum
	if (label->d_magic != BSDLABEL_DISKMAGIC ||
			label->d_magic2 != BSDLABEL_DISKMAGIC)
	{
		printf("Invalid BSD Partition Magic!\n");
		goto bsd_init_error;
	}

	if (label->d_secsize != SECTSIZE)
	{
		// Don't have enough info to check the other disk geometry parameters
		printf("Bad Disk Geometry!\n");
		goto bsd_init_error;
	}

	if (label->d_sparespertrack != 0 || label->d_sparespercyl != 0)
	{
		printf("Sorry, can't handle bad sectors!\n");
		goto bsd_init_error;
	}

	if (label->d_npartitions < 1 || label->d_npartitions > BSDLABEL_MAXLABELS)
	{
		printf("No BSD Partitions found!\n");
		goto bsd_init_error;
	}

	if (label->d_type >= BSDLABEL_DKMAXTYPES)
		printf("Warning, Unknown Disk Type!\n");

	// Let's look at the whole disk first
	if (label->d_partitions[BSDLABEL_LABEL_RAWDISK].p_size > maxblocks)
	{
		printf("Whole disk is larger than partition size!\n");
		goto bsd_init_error;
	}
	else if (label->d_partitions[BSDLABEL_LABEL_RAWDISK].p_size < maxblocks)
	{
		printf("Warning, Whole disk is smaller than partition size!\n");
		// Contain everything inside the boundaries of the "whole disk"
		maxblocks = label->d_partitions[BSDLABEL_LABEL_RAWDISK].p_size;
	}

	// p_offset is relative to the start of the disk, wherever that is
	offset = label->d_partitions[BSDLABEL_LABEL_RAWDISK].p_offset;

	for (i = 0; i < label->d_npartitions; i++)
	{
		info->parts[i].start = 0;
		info->parts[i].length = 0;
		info->parts[i].type = 0;

		if (label->d_partitions[i].p_size == 0)
		{
			if (label->d_partitions[i].p_fstype != BSDLABEL_FS_UNUSED)
			{
				printf("Size 0 partition claims to be in use!\n");
				goto bsd_init_error;
			}
			continue;
		}
		if (label->d_partitions[i].p_offset - offset < 0)
		{
			printf("BSD Partition %d has an invalid offset!\n", i);
			goto bsd_init_error;
		}
		if (label->d_partitions[i].p_offset - offset +
				label->d_partitions[i].p_size > maxblocks)
		{
			printf("BSD Partition %d is too big!\n", i);
			goto bsd_init_error;
		}
		if (label->d_partitions[i].p_fstype >= BSDLABEL_FSMAXTYPES)
			printf("Warning, partition %d has unknown type!\n", i);

		info->parts[i].start = label->d_partitions[i].p_offset - offset;
		info->parts[i].length = label->d_partitions[i].p_size;
		info->parts[i].type = label->d_partitions[i].p_fstype;
		printf("BSD Partition %d has %d blocks, type: %s\n", i,
		       info->parts[i].length, fstypenames[info->parts[i].type]);
	}

	// Check for overlap
	for (i = 0; i < label->d_npartitions; i++)
	{
		if (i == BSDLABEL_LABEL_RAWDISK)
			continue;

		for (j = i + 1 ; j < label->d_npartitions; j++)
		{
			if (j == BSDLABEL_LABEL_RAWDISK)
				continue;

			if ((info->parts[j].start > info->parts[i].start &&
			     info->parts[j].start < info->parts[i].length) ||
			    (info->parts[j].length > info->parts[i].start &&
			     info->parts[j].length < info->parts[i].length) ||
			    (info->parts[i].start > info->parts[j].start &&
			     info->parts[i].start < info->parts[j].length) ||
			    (info->parts[i].length > info->parts[j].start &&
			     info->parts[i].length < info->parts[j].length))
			{
				printf("Overlapping partitions detected!\n");
				goto bsd_init_error;
			}
		}
	}

	info->count = label->d_npartitions;
	info->bd = bd;

	return info;

bsd_init_error:
	free(info);
	return NULL;
}

int bsd_ptable_count(void * _info)
{
	struct disklabel_info * info = (struct disklabel_info *) _info;
	return info->count;
}

uint8_t bsd_ptable_type(void * _info, int index)
{
	struct disklabel_info * info = (struct disklabel_info *) _info;
	if (index < 1 || index > BSDLABEL_MAXLABELS)
		return 0;
	return info->parts[index-1].type;
}

BD_t * bsd_ptable_bd(void * _info, int index)
{
	struct disklabel_info * info = (struct disklabel_info *) _info;

	if (index < 1 || index > BSDLABEL_MAXLABELS)
		return NULL;

	index--;

	if (info->parts[index].length == 0)
		return NULL;

	return partition_bd(info->bd, info->parts[index].start, info->parts[index].length);
}

void bsd_ptable_free(void * _info)
{
	struct disklabel_info * info = (struct disklabel_info *) _info;
	free(info);
}
