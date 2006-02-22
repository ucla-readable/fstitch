#include <kfs/feature.h>

const feature_t KFS_feature_size = {id: 0x00000001, optional: 0, warn: 0, description: "File size in bytes"};
const feature_t KFS_feature_filetype = {id: 0x00000002, optional: 0, warn: 0, description: "File type"};
const feature_t KFS_feature_nlinks = {id: 0x00000003, optional: 0, warn: 0, description: "Hard Link Count"};
const feature_t KFS_feature_freespace = {id: 0x00000004, optional: 0, warn: 0, description: "Free Space On Disk"};
const feature_t KFS_feature_file_lfs = {id: 0x00000005, optional: 0, warn: 0, description: "File Toplevel LFS"};
