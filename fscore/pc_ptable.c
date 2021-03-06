/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#include <lib/platform.h>
#include <lib/partition.h>
#include <lib/vector.h>

#include <fscore/bd.h>
#include <fscore/bdesc.h>
#include <fscore/pc_ptable.h>

#include <modules/partition_bd.h>

struct partition {
	uint32_t start, length;
	uint8_t type, boot;
};

struct ptable_info {
	BD_t * bd;
	int count;
	struct partition primary[4];
	vector_t * extended;
};

#define SECTSIZE 512

static void swizzle(uint32_t * x)
{
	uint32_t y;
	uint8_t * z;

	z = (uint8_t *) x;
	y = *x;
	z[0] = y & 0xFF;
	z[1] = (y >> 8) & 0xFF;
	z[2] = (y >> 16) & 0xFF;
	z[3] = (y >> 24) & 0xFF;
}

static void condense_ptable(const struct pc_ptable * ptable, struct partition * partition)
{
	int i;
	for(i = 0; i != 4; i++)
	{
		partition[i].boot = ptable[i].boot;
		partition[i].type = ptable[i].type;
		partition[i].start = ptable[i].lba_start;
		partition[i].length = ptable[i].lba_length;
		swizzle(&partition[i].start);
		swizzle(&partition[i].length);
	}
}

static int _detect_extended(struct ptable_info * info, uint32_t table_offset, uint32_t ext_offset)
{
	bdesc_t * table = CALL(info->bd, read_block, table_offset, 1, NULL);
	struct partition ptable[4];
	int i;
	
	if(!table)
		return -1;
	condense_ptable((const struct pc_ptable *) &bdesc_data(table)[PTABLE_OFFSET], ptable);
	
	for(i = 0; i != 4; i++)
	{
		if(ptable[i].type == PTABLE_DOS_EXT_TYPE ||
		   ptable[i].type == PTABLE_W95_EXT_TYPE ||
		   ptable[i].type == PTABLE_LINUX_EXT_TYPE)
		{
			if(_detect_extended(info, ext_offset + ptable[i].start, ext_offset))
				return -1;
		}
		else if(ptable[i].length)
		{
			struct partition * np = malloc(sizeof(*np));
			if(!np)
				return -1;
			*np = ptable[i];
			np->start += table_offset;
			if(vector_push_back(info->extended, np))
			{
				free(np);
				return -1;
			}
			info->count++;
		}
	}
	
	return 0;
}

static int detect_extended(struct ptable_info * info)
{
	int i;
	for(i = 0; i != 4; i++)
	{
		if(info->primary[i].type == PTABLE_DOS_EXT_TYPE ||
		   info->primary[i].type == PTABLE_W95_EXT_TYPE ||
		   info->primary[i].type == PTABLE_LINUX_EXT_TYPE)
		{
			if(_detect_extended(info, info->primary[i].start, info->primary[i].start))
				return -1;
		}
		else if(info->primary[i].length)
			info->count++;
	}
	return 0;
}

/* initialize the PC partition table reader */
void * pc_ptable_init(BD_t * bd)
{
	struct ptable_info * info;
	const struct pc_ptable * ptable;
	bdesc_t * mbr;
	
	/* make sure the block size is SECTSIZE */
	if(bd->blocksize != SECTSIZE)
		return NULL;
	
	info = malloc(sizeof(*info));
	if(!info)
		return NULL;
	
	info->extended = vector_create();
	if(!info->extended)
		goto error_extended;
	
	/* read the partition table */
	mbr = CALL(bd, read_block, 0, 1, NULL);
	if(!mbr)
		goto error_mbr;
	ptable = (const struct pc_ptable *) &bdesc_data(mbr)[PTABLE_OFFSET];
	
	/* FIXME: support EZDrive partition tables here!
	 * They have shadow partitions of type PTABLE_EZDRIVE_TYPE listed
	 * in sector 0, and the real partition table is in sector 1. */
	/* Note: we'd also have to support this in the bootloader. */
	
	if(bdesc_data(mbr)[PTABLE_MAGIC_OFFSET] != PTABLE_MAGIC[0] ||
	   bdesc_data(mbr)[PTABLE_MAGIC_OFFSET + 1] != PTABLE_MAGIC[1])
	{
		printf("No partition table found!\n");
		goto error_mbr;
	}
	
	info->bd = bd;
	info->count = 0;
	condense_ptable(ptable, info->primary);
	
	/* detect extended partitions */
	if(detect_extended(info))
		goto error_detect;
	
	return info;
	
error_detect:
	while(vector_size(info->extended))
	{
		free(vector_elt(info->extended, vector_size(info->extended) - 1));
		vector_pop_back(info->extended);
	}
error_mbr:
	vector_destroy(info->extended);
error_extended:
	free(info);
	return NULL;
}

/* These functions are not finished, and need adaptation to the new stuff above. */

/* count the partitions */
int pc_ptable_count(void * _info)
{
	/*struct ptable_info * info = (struct ptable_info *) _info;
	int i, count = 0;
	
	for(i = 0; i != 4; i++)
		if(info->primary[i].length)
			count++;
	
	return count;*/
	return ((struct ptable_info *) _info)->count;
}

/* get the partition type */
uint8_t pc_ptable_type(void * _info, int index)
{
	struct ptable_info * info = (struct ptable_info *) _info;
	if(index < 1 || index > 4)
		return 0;
	return info->primary[index - 1].type;
}

/* get a partition block device */
BD_t * pc_ptable_bd(void * _info, int index)
{
	struct ptable_info * info = (struct ptable_info *) _info;
	
	if(index < 1 || index > 4)
		return 0;
	
	index--;
	return partition_bd(info->bd, info->primary[index].start, info->primary[index].length);
}

/* free the partition table structures */
void pc_ptable_free(void * _info)
{
	struct ptable_info * info = (struct ptable_info *) _info;
	while(vector_size(info->extended))
	{
		free(vector_elt(info->extended, vector_size(info->extended) - 1));
		vector_pop_back(info->extended);
	}
	vector_destroy(info->extended);
	free(info);
}
