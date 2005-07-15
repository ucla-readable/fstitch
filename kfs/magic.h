#ifndef __KUDOS_KFS_MAGIC_H
#define __KUDOS_KFS_MAGIC_H

/* "BDACCESS" */
#define DEVFS_MAGIC 0xBDACCE55

/* "FIDCLOSE" */
#define FIDCLOSER_MAGIC 0xF1DC105E

/* "FIDPROTR" */
#define FIDPROTECTOR_MAGIC 0xF1D78078

/* "JOSCFSMG" */
#define JOSFS_CFS_MAGIC 0x705CF535

/* use the same magic number for objects as in the FS */
#define JOSFS_MAGIC JOSFS_FS_MAGIC

/* "SAFEDATA" */
#define JOURNAL_MAGIC 0x5AFEDA7A

/* "JnlQ" */
#define JOURNAL_QUEUE_MAGIC 0x4A6E6C51

/* ??? */
#define MIRROR_BD_MAGIC 0x888BDA1D

/* "TBLCLASS" */
#define TABLE_CLASSIFIER_MAGIC 0x7B1C1A55

// "LEET0CFS" (because uhfs is leet)
#define UHFS_MAGIC 0x1EE70CF5

/* "WOLEDISC" */
#define WHOLEDISK_MAGIC 0x301ED15C

#endif /* __KUDOS_KFS_MAGIC_H */
