#!/bin/sh

if [ $# -lt 1 ]; then
	echo "Usage: `basename \"$0\"` <MNT> [FUSE_OPTS...]"
	exit 1
fi

MNT="$1"
shift

# Run kfsd. If it exits non-zero (and so probably crashed) explicitly # unmount.
# Lazy unmount in case fusermount is called before kfsd's mount is removed.
# Set KFSD_WRAP to run kfsd within a wrapper, such as valgrind.
# TODO: from C we could use WIFEXITED() to know exactly when to fusermount.
$KFSD_WRAP ./obj/unix-user/kfs/kfsd "$MNT" -o allow_root $@ \
	|| if [ "$MNT" != "-h" ]; then fusermount -uz "$MNT"; fi
