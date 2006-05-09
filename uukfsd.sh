#!/bin/sh

if [ $# -lt 1 ]; then
	echo "Usage: `basename \"$0\"` <MNT> [FUSE_OPTS...]"
	exit 1
fi

MNT="$1"
shift

KFSD=./obj/unix-user/kfs/kfsd
# '-o allow_root' so that the (suid) fusermount can mount nested mountpoints
KFSD_OPTS="-o allow_root"

# Run kfsd. If it exits non-zero (and so probably crashed) explicitly unmount.
# Lazy unmount in case fusermount is called before kfsd's mount is removed.
# Set KFSD_WRAP to run kfsd within a wrapper, such as gdb or valgrind.
# TODO: from C we could use WIFEXITED() to know exactly when to fusermount.
if [ "$KFSD_WRAP" == "gdb" ] && [ "$KFSD_WRAP_OPTS" == "" ]
then
	KFSD_WRAP_OPTS="--args"
	$KFSD_WRAP $KFSD_WRAP_OPTS "$KFSD" "$MNT" $KFSD_OPTS $@
	[ "$MNT" != "-h" ] && fusermount -uz "$MNT"
elif [ "$KFSD_WRAP" == "valgrind" ] && [ "$KFSD_WRAP_OPTS" == "" ]
then
	KFSD_WRAP_OPTS="--suppressions=conf/memcheck.supp --leak-check=full --show-reachable=yes --leak-resolution=high"
	$KFSD_WRAP $KFSD_WRAP_OPTS "$KFSD" "$MNT" $KFSD_OPTS $@ \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
else
	$KFSD_WRAP $KFSD_WRAP_OPTS "$KFSD" "$MNT" $KFSD_OPTS $@ \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
fi