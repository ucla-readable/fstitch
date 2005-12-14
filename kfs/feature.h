#ifndef __KUDOS_KFS_FEATURE_H
#define __KUDOS_KFS_FEATURE_H

#include <lib/types.h>

struct feature;
typedef struct feature feature_t;

struct feature {
	uint32_t id:30, optional:1, warn:1;
	const char * description;
};

extern const feature_t KFS_feature_size;
extern const feature_t KFS_feature_filetype;
extern const feature_t KFS_feature_nlinks;
extern const feature_t KFS_feature_freespace;
extern const feature_t KFS_feature_file_lfs;
extern const feature_t KFS_feature_file_lfs_name;
extern const feature_t KFS_feature_unixdir;

/* filetype values - large to avoid conflict with on-disk values */
#define TYPE_FILE    0x80
#define TYPE_DIR     0x81
#define TYPE_SYMLINK 0x82
#define TYPE_DEVICE  0x83
#define TYPE_INVAL   (-1)

#endif /* __KUDOS_KFS_FEATURE_H */
