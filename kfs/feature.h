#ifndef __KUDOS_KFS_FEATURE_H
#define __KUDOS_KFS_FEATURE_H

typedef uint16_t feature_id_t;

#define KFS_FEATURE_NONE      0x00 /* Reserved: no feature */

#define KFS_FEATURE_SIZE      0x01 /* File size in bytes */
#define KFS_FEATURE_FILETYPE  0x02 /* File type */
#define KFS_FEATURE_NLINKS    0x03 /* Hard link count */
#define KFS_FEATURE_FREESPACE 0x04 /* Free space on disk (in blocks) */
#define KFS_FEATURE_FILE_LFS  0x05 /* File top-level LFS */
#define KFS_FEATURE_UID       0x06 /* Owner ID */
#define KFS_FEATURE_GID       0x07 /* Group ID */
#define KFS_FEATURE_UNIX_PERM 0x08 /* Standard UNIX permissions */
#define KFS_FEATURE_BLOCKSIZE 0x09 /* File system block size (in bytes) */
#define KFS_FEATURE_DEVSIZE   0x0A /* Device size (in blocks) */
#define KFS_FEATURE_MTIME     0x0B /* File modification time */
#define KFS_FEATURE_ATIME     0x0C /* File access time */
#define KFS_FEATURE_SYMLINK   0x0D /* Symbolic links */
#define KFS_FEATURE_DELETE    0x0E /* Delete full file in LFS */

// Get metadata associated with the opaque variable 'arg'.
// Returns:
// * >=0: fills 'data', return value is number of bytes filled
// * -ENOMEM: 'id' is supported, but 'size' is too small
// * -ENOENT: 'id' is not supported
// * <0: implementation specific error
//
// Pros/cons for providing this interface in CFS/LFS as a function
// vs as an array of features:
// - function pro: general
// - array pro: no need to create temporary copies of feature data (eg symlink)
typedef int (*get_metadata_t)(void * arg, feature_id_t id, size_t size, void * data);

struct metadata_set {
	get_metadata_t get;
	void * arg;
};
typedef struct metadata_set metadata_set_t;

#ifndef __KERNEL__

/* filetype values - large to avoid conflict with on-disk values */
#define TYPE_FILE    0x80
#define TYPE_DIR     0x81
#define TYPE_SYMLINK 0x82
#define TYPE_DEVICE  0x83
#define TYPE_INVAL   (-1)

#else

#include <linux/fs.h>

#define TYPE_FILE     DT_REG
#define TYPE_DIR      DT_DIR
#define TYPE_SYMLINK  DT_LNK
#define TYPE_DEVICE   DT_REG /* really just a file to Linux */
#define TYPE_INVAL    DT_UNKNOWN

#endif

#endif /* __KUDOS_KFS_FEATURE_H */
