#include <kfs/feature.h>

const feature_t KFS_feature_size = {id: 0x00000001, optional: 0, warn: 0, description: "File Size In Bytes"};
const feature_t KFS_feature_filetype = {id: 0x00000002, optional: 0, warn: 0, description: "File Type"};
const feature_t KFS_feature_nlinks = {id: 0x00000003, optional: 0, warn: 0, description: "Hard Link Count"};
const feature_t KFS_feature_freespace = {id: 0x00000004, optional: 0, warn: 0, description: "Free Space On Disk In Blocks"};
const feature_t KFS_feature_file_lfs = {id: 0x00000005, optional: 0, warn: 0, description: "File Toplevel LFS"};
const feature_t KFS_feature_unix_permissions = {id: 0x00000006, optional: 0, warn: 0, description: "Standard Unix Permissions"};
const feature_t KFS_feature_blocksize = {id: 0x00000007, optional: 0, warn: 0, description: "File System Block Size In Bytes"};
const feature_t KFS_feature_devicesize = {id: 0x00000008, optional: 0, warn: 0, description: "Device Size In Blocks"};
const feature_t KFS_feature_mtime = {id: 0x00000009, optional: 0, warn: 0, description: "File modification time"};
const feature_t KFS_feature_atime = {id: 0x00000009, optional: 0, warn: 0, description: "File access time"};
