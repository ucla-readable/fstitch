#ifndef __KUDOS_KFS_FEATURE_H
#define __KUDOS_KFS_FEATURE_H

#include <lib/types.h>

struct feature;
typedef struct feature feature_t;

struct feature {
	uint32_t id:30, optional:1, warn:1;
	const char * description;
};

/* these are defined in lib/kfs_feature.c */
extern const feature_t KFS_feature_size;
// TODO: filetype should perhaps be broken into two features:
// (1) filetype_create and (2) filetype_get.
// Then directories would support create() and all files get().
// For now, for symlinks, the symlink feature is checked to detect symlink creation support.
extern const feature_t KFS_feature_filetype;
extern const feature_t KFS_feature_nlinks;
extern const feature_t KFS_feature_freespace;
extern const feature_t KFS_feature_file_lfs;
extern const feature_t KFS_feature_uid;
extern const feature_t KFS_feature_gid;
extern const feature_t KFS_feature_unix_permissions;
extern const feature_t KFS_feature_blocksize;
extern const feature_t KFS_feature_devicesize;
extern const feature_t KFS_feature_mtime;
extern const feature_t KFS_feature_atime;
extern const feature_t KFS_feature_symlink;

// Get metadata associated with the opaque variable 'arg'.
// Returns:
// * >=0: fills 'data', return value is number of bytes filled
// * -E_NO_MEM: 'id' is supported, but 'size' is too small
// * -E_NOT_FOUND: 'id' is not supported
// * <0: implementation specific error
//
// Pros/cons for providing this interface in CFS/LFS as a function
// vs as an array of features:
// - function pro: general
// - array pro: no need to create temporary copies of feature data (eg symlink)
typedef int (*get_metadata_t)(void * arg, uint32_t id, size_t size, void * data);

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
#define TYPE_DEVICE   DT_REG /* KudOS has "device" type which is really a file to Linux */
#define TYPE_INVAL    DT_UNKNOWN

#endif

#endif /* __KUDOS_KFS_FEATURE_H */
