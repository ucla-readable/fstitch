/* This file is part of Featherstitch. Featherstitch is copyright 2005-2007 The
 * Regents of the University of California. It is distributed under the terms of
 * version 2 of the GNU GPL. See the file LICENSE for details. */

#ifndef __FSTITCH_FSCORE_FUSE_SERVE_MOUNT_H
#define __FSTITCH_FSCORE_FUSE_SERVE_MOUNT_H

#include <fuse/fuse_lowlevel.h>
#include <lib/hash_map.h>
#include <fscore/cfs.h>

// Purpose:
// fuse_serve_mount provides an abstraction for mounting and unmounting
// fuse mountpoints


typedef struct mount {
	bool mounted; // struct is valid only when mounted is true

	hash_map_t * parents; // directory inode_t -> parent inode_t

	char * fstitch_path;
	CFS_t * cfs;
	inode_t root_ino;

	struct fuse_args args;
	char * mountpoint;
	int channel_fd;
	struct fuse_session * session;
	struct fuse_chan * channel;
} mount_t;

// Init fuse_serve_mount with main()'s argc and argv and fuse_serve's ops.
// Returns >= 0 on success, the returned value is the step_remove fd.
int fuse_serve_mount_init(int argc, char ** argv, struct fuse_lowlevel_ops * ops, size_t ops_len);

// fuse_serve calls this function when init's returned fd has activity
int fuse_serve_mount_step_remove(void);

// Perform an instant shutdown.
// Fuse is async told about the unmounts.
void fuse_serve_mount_instant_shutdown(void);
// Start a step-by-spte shutdown.
// All filesystems are unmounted from fuse when the mounts set is empty.
// Note: Calling this function shortly after an add or remove may not
// work out well; if you see an error message we'll remove this issue.
int fuse_serve_mount_start_shutdown(void);

// Set the root mountpoint.
// Can only be called before calling fuse_serve_mount_load_mounts().
int fuse_serve_mount_set_root(CFS_t * root);

// Return the buffer size for fuse channels
size_t fuse_serve_mount_chan_bufsize(void);

// Inform fuse_serve_mount that fuse requests for root are now being serviced;
// this allows submounts to start mounting.
// Can only be called after setting the root with fuse_serve_mount_set_root().
int fuse_serve_mount_load_mounts(void);

// Return the set of mounts; each entry is a mount_t*; terminated by null.
// You should assume this set can change after any call to fuse_serve_mount.
mount_t ** fuse_serve_mounts(void);

// Add a fuse mount at path for cfs.
// The mount (immediately visible in mounts upon return) is active only
// after .mounted == 1.
// Cannot be called once a shutdown has started.
int fuse_serve_mount_add(CFS_t * cfs, const char * path);
// Remove the fuse mount m.
// The mount will be removed from mounts only after a later call
// to fuser_serve_mounts_step_remove(); until then the mount must persist.
// Has no effect once a shutdown has started.
int fuse_serve_mount_remove(mount_t * m);

#endif // __FSTITCH_FSCORE_FUSE_SERVE_MOUNT_H
