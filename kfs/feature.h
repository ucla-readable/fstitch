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
extern const feature_t KFS_feature_filetype;
extern const feature_t KFS_feature_nlinks;
extern const feature_t KFS_feature_freespace;
extern const feature_t KFS_feature_file_lfs;
extern const feature_t KFS_feature_unix_permissions;
extern const feature_t KFS_feature_blocksize;
extern const feature_t KFS_feature_devicesize;

/* filetype values - large to avoid conflict with on-disk values */
#define TYPE_FILE    0x80
#define TYPE_DIR     0x81
#define TYPE_SYMLINK 0x82
#define TYPE_DEVICE  0x83
#define TYPE_INVAL   (-1)

#endif /* __KUDOS_KFS_FEATURE_H */
