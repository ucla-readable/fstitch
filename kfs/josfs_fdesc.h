#ifndef __KUDOS_KFS_JOSFS_FDESC_H
#define __KUDOS_KFS_JOSFS_FDESC_H

struct jos_fdesc {
    struct bdesc * dirb;
    int index;
    struct JOS_File * file;
};

#endif /* __KUDOS_KFS_JOSFS_FDESC_H */
