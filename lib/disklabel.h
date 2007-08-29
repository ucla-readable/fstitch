/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef _DISKLABEL_H_
#define _DISKLABEL_H_

#define BSDLABEL_DISKMAGIC      ((uint32_t)0x82564557)
#define BSDLABEL_MAXLABELS      8
#define BSDLABEL_LABELSECTOR    1
#define BSDLABEL_LABELOFFSET    0
#define BSDLABEL_LABEL_RAWDISK  2

#define BSDLABEL_NDDATA 5
#define BSDLABEL_NSPARE 5

struct disklabel {
	uint32_t d_magic; 	     /* the magic number */
	uint16_t d_type;		     /* drive type */
	uint16_t d_subtype;	     /* controller/d_type specific */
	char     d_typename[16];	     /* type name, e.g. "eagle" */

	/*
	 * d_packname contains the pack identifier and is returned when
	 * the disklabel is read off the disk or in-core copy.
	 * d_boot0 and d_boot1 are the (optional) names of the
	 * primary (block 0) and secondary (block 1-15) bootstraps
	 * as found in /boot.  These are returned when using
	 * getdiskbyname(3) to retrieve the values from /etc/disktab.
	 */
	union {
		char    un_d_packname[16];      /* pack identifier */
		struct {
			char *un_d_boot0;	     /* primary bootstrap name */
			char *un_d_boot1;	     /* secondary bootstrap name */
		} un_b;
	} d_un;

	/* disk geometry: */
	uint32_t d_secsize;	     /* # of bytes per sector */
	uint32_t d_nsectors;	     /* # of data sectors per track */
	uint32_t d_ntracks;	     /* # of tracks per cylinder */
	uint32_t d_ncylinders;	     /* # of data cylinders per unit */
	uint32_t d_secpercyl;	     /* # of data sectors per cylinder */
	uint32_t d_secperunit;	     /* # of data sectors per unit */

	/*
	 * Spares (bad sector replacements) below are not counted in
	 * d_nsectors or d_secpercyl.  Spare sectors are assumed to
	 * be physical sectors which occupy space at the end of each
	 * track and/or cylinder.
	 */
	uint16_t d_sparespertrack;     /* # of spare sectors per track */
	uint16_t d_sparespercyl;	     /* # of spare sectors per cylinder */
	/*
	 * Alternate cylinders include maintenance, replacement, configuration
	 * description areas, etc.
	 */
	uint32_t d_acylinders;	     /* # of alt. cylinders per unit */

	/* hardware characteristics: */
	/*
	 * d_interleave, d_trackskew and d_cylskew describe perturbations
	 * in the media format used to compensate for a slow controller.
	 * Interleave is physical sector interleave, set up by the
	 * formatter or controller when formatting.  When interleaving is
	 * in use, logically adjacent sectors are not physically
	 * contiguous, but instead are separated by some number of
	 * sectors.  It is specified as the ratio of physical sectors
	 * traversed per logical sector.  Thus an interleave of 1:1
	 * implies contiguous layout, while 2:1 implies that logical
	 * sector 0 is separated by one sector from logical sector 1.
	 * d_trackskew is the offset of sector 0 on track N relative to
	 * sector 0 on track N-1 on the same cylinder.  Finally, d_cylskew
	 * is the offset of sector 0 on cylinder N relative to sector 0
	 * on cylinder N-1.
	 */
	uint16_t d_rpm;		     /* rotational speed */
	uint16_t d_interleave;	     /* hardware sector interleave */
	uint16_t d_trackskew;	     /* sector 0 skew, per track */
	uint16_t d_cylskew;	     /* sector 0 skew, per cylinder */
	uint32_t d_headswitch;	     /* head switch time, usec */
	uint32_t d_trkseek;	     /* track-to-track seek, usec */
	uint32_t d_flags; 	     /* generic flags */
	uint32_t d_drivedata[BSDLABEL_NDDATA];  /* drive-type specific information */
	uint32_t d_spare[BSDLABEL_NSPARE];      /* reserved for future use */
	uint32_t d_magic2;	     /* the magic number (again) */
	uint16_t d_checksum;	     /* xor of data incl. partitions */

	/* filesystem and partition information: */
	uint16_t d_npartitions;	     /* number of partitions in following */
	uint32_t d_bbsize;	     /* size of boot area at sn0, bytes */
	uint32_t d_sbsize;	     /* max size of fs superblock, bytes */
	struct bsd_partition {	     /* the partition table */
		uint32_t p_size;	     /* number of sectors in partition */
		uint32_t p_offset;     /* starting sector */
		uint32_t p_fsize;      /* filesystem basic fragment size */
		uint8_t p_fstype;      /* filesystem type, see below */
		uint8_t p_frag;	     /* filesystem fragments per block */
		union {
			uint16_t cpg;  /* UFS: FS cylinders per group */
			uint16_t sgs;  /* LFS: FS segment shift */
		} __partition_u1;
	} d_partitions[BSDLABEL_MAXLABELS];  /* actually may be more */
};

/* d_type values: */
#define BSDLABEL_DTYPE_SMD		     1		     /* SMD, XSMD; VAX hp/up */
#define BSDLABEL_DTYPE_MSCP 	     2		     /* MSCP */
#define BSDLABEL_DTYPE_DEC		     3		     /* other DEC (rk, rl) */
#define BSDLABEL_DTYPE_SCSI 	     4		     /* SCSI */
#define BSDLABEL_DTYPE_ESDI 	     5		     /* ESDI interface */
#define BSDLABEL_DTYPE_ST506	     6		     /* ST506 etc. */
#define BSDLABEL_DTYPE_HPIB 	     7		     /* CS/80 on HP-IB */
#define BSDLABEL_DTYPE_HPFL 	     8		     /* HP Fiber-link */
#define BSDLABEL_DTYPE_FLOPPY	     10 	     /* floppy */
#define BSDLABEL_DTYPE_CCD		     11 	     /* concatenated disk */
#define BSDLABEL_DTYPE_VINUM	     12 	     /* vinum volume */
#define BSDLABEL_DTYPE_DOC2K	     13 	     /* Msys DiskOnChip */

#ifdef DKTYPENAMES
static char *dktypenames[] = {
	"unknown",
	"SMD",
	"MSCP",
	"old DEC",
	"SCSI",
	"ESDI",
	"ST506",
	"HP-IB",
	"HP-FL",
	"type 9",
	"floppy",
	"CCD",
	"Vinum",
	"DOC2K",
	NULL
};
#define BSDLABEL_DKMAXTYPES  (sizeof(dktypenames) / sizeof(dktypenames[0]) - 1)
#endif

/*
 * Filesystem type and version.
 * Used to interpret other filesystem-specific
 * per-partition information.
 */
#define BSDLABEL_FS_UNUSED     0     /* unused */
#define BSDLABEL_FS_SWAP       1     /* swap */
#define BSDLABEL_FS_V6         2     /* Sixth Edition */
#define BSDLABEL_FS_V7         3     /* Seventh Edition */
#define BSDLABEL_FS_SYSV       4     /* System V */
#define BSDLABEL_FS_V71K       5     /* V7 with 1K blocks (4.1, 2.9) */
#define BSDLABEL_FS_V8         6     /* Eighth Edition, 4K blocks */
#define BSDLABEL_FS_BSDFFS     7     /* 4.2BSD fast file system */
#define BSDLABEL_FS_MSDOS      8     /* MSDOS file system */
#define BSDLABEL_FS_BSDLFS     9     /* 4.4BSD log-structured file system */
#define BSDLABEL_FS_OTHER      10    /* in use, but unknown/unsupported */
#define BSDLABEL_FS_HPFS       11    /* OS/2 high-performance file system */
#define BSDLABEL_FS_ISO9660    12    /* ISO 9660, normally CD-ROM */
#define BSDLABEL_FS_BOOT       13    /* partition contains bootstrap */
#define BSDLABEL_FS_VINUM      14    /* Vinum drive */

#ifdef  FSTYPENAMES
static char *fstypenames[] = {
	"unused",
	"swap",
	"Version 6",
	"Version 7",
	"System V",
	"4.1BSD",
	"Eighth Edition",
	"4.2BSD",
	"MSDOS",
	"4.4LFS",
	"unknown",
	"HPFS",
	"ISO9660",
	"boot",
	"vinum",
	NULL
};
#define BSDLABEL_FSMAXTYPES  (sizeof(fstypenames) / sizeof(fstypenames[0]) - 1)
#endif

/*
 * flags shared by various drives:
 */
#define    BSDLABEL_FLAG_REMOVABLE   0x01      /* removable media */
#define    BSDLABEL_FLAG_ECC         0x02      /* supports ECC */
#define    BSDLABEL_FLAG_BADSECT     0x04      /* supports bad sector forw. */
#define    BSDLABEL_FLAG_RAMDISK     0x08      /* disk emulator */
#define    BSDLABEL_FLAG_CHAIN       0x10      /* can do back-back transfers */

/*
 * Drive data for SMD.
 */
#define d_smdflags      d_drivedata[0]
#define d_mindist       d_drivedata[1]
#define d_maxdist       d_drivedata[2]
#define d_sdist         d_drivedata[3]

/*
 * Drive data for ST506.
 */
#define d_precompcyl    d_drivedata[0]
#define d_gap3          d_drivedata[1]      /* used only when formatting */

/*
 * Drive data for SCSI.
 */
#define d_blind         d_drivedata[0]

#endif

