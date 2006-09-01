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
	KFSD_WRAP_OPTS="-q --args"
	$KFSD_WRAP $KFSD_WRAP_OPTS "$KFSD" "$MNT" $KFSD_OPTS $@
	[ "$MNT" != "-h" ] && fusermount -uz "$MNT"
elif [ "$KFSD_WRAP" == "valgrind" ] && [ "$KFSD_WRAP_OPTS" == "" ]
then
	KFSD_WRAP_OPTS="--suppressions=conf/memcheck.supp --leak-check=full --show-reachable=yes --leak-resolution=high"
	$KFSD_WRAP $KFSD_WRAP_OPTS "$KFSD" "$MNT" $KFSD_OPTS $@ \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
elif [ "$KFSD_WRAP" == "cachegrind" ] && [ "$KFSD_WRAP_OPTS" == "" ]
then
	KFSD_WRAP_OPTS="--tool=cachegrind"
	valgrind $KFSD_WRAP_OPTS "$KFSD" "$MNT" $KFSD_OPTS $@ \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
elif [ "$KFSD_WRAP" == "massif" ] && [ "$KFSD_WRAP_OPTS" == "" ]
then
	KWO="--tool=massif"
	KWO="$KWO --depth=5"
	KWO="$KWO --alloc-fn=chain_elt_create --alloc-fn=hash_map_insert --alloc-fn=hash_map_resize --alloc-fn=hash_map_create_size --alloc-fn=hash_map_create"
	KWO="$KWO --alloc-fn=vector_create_elts --alloc-fn=vector_create_size"
	KWO="$KWO --alloc-fn=memdup"
	valgrind $KWO "$KFSD" "$MNT" $KFSD_OPTS $@ \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
elif [ "$KFSD_WRAP" == "time" ]
then
	LOG=time.log
	NAME="$KFSD_WRAP_OPTS"
	$KFSD_WRAP -f "kfsd-$NAME %e real %U user %S sys" -a -o "$LOG" "$KFSD" "$MNT" $KFSD_OPTS $@ \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
else
	$KFSD_WRAP $KFSD_WRAP_OPTS "$KFSD" "$MNT" $KFSD_OPTS $@ \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
fi
