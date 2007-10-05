/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_MAGIC_H
#define __FSTITCH_FSCORE_MAGIC_H

/* "BDACCESS" */
#define DEVFS_MAGIC 0xBDACCE55

/* "JOSCFSMG" */
#define JOSFS_CFS_MAGIC 0x705CF535

/* use the same magic number for objects as in the FS */
#define JOSFS_MAGIC JOSFS_FS_MAGIC
#define WAFFLE_MAGIC WAFFLE_FS_MAGIC

/* "SAFEDATA" */
#define JOURNAL_MAGIC 0x5AFEDA7A

/* "FILEHIDE" */
#define FILE_HIDING_MAGIC 0xF11E41DE

/* "ICASE" */
#define ICASE_MAGIC 0x1CA5E000

/* "LEET0CFS" (because UHFS is leet) */
#define UHFS_MAGIC 0x1EE70CF5

/* "DATADELA" */
#define WB_CACHE_MAGIC 0xDA7ADE1A

/* "WOLEDISC" */
#define WHOLEDISK_MAGIC 0x301ED15C

#endif /* __FSTITCH_FSCORE_MAGIC_H */
