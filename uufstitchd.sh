#!/bin/bash

if [ "$1" = '--gdb' -o "$1" = '--valgrind' -o "$1" = '--callgrind' -o "$1" = '--strace' ]; then
	FSTITCHD_WRAP=`echo "$1" | sed s/--//`
	shift
fi

while expr "$1" : "--simulate-cache.*"; do
	FSTITCHD_WRAP_OPTS="$FSTITCHD_WRAP_OPTS $1"
	shift
done

if [ $# -lt 1 ]; then
	echo "Usage: `basename \"$0\"` <MNT> [FUSE_OPTS...]"
	exit 1
fi

MNT="$1"
shift

FSTITCHD=./obj/unix-user/fstitchd
# -s because fstitchd is not multithread safe
# '-o allow_root' so that the (suid) fusermount can mount nested mountpoints
FSTITCHD_OPTS="-s -o allow_root"

# Run fstitchd. If it exits non-zero (and so probably crashed) explicitly unmount.
# Lazy unmount in case fusermount is called before fstitchd's mount is removed.
# Set FSTITCHD_WRAP to run fstitchd within a wrapper, such as gdb or valgrind.
# TODO: from C we could use WIFEXITED() to know exactly when to fusermount.
if [ "$FSTITCHD_WRAP" == "gdb" ] && [ "$FSTITCHD_WRAP_OPTS" == "" ]
then
	FSTITCHD_WRAP_OPTS="-q --args"
	$FSTITCHD_WRAP $FSTITCHD_WRAP_OPTS "$FSTITCHD" "$MNT" $FSTITCHD_OPTS "$@"
	[ "$MNT" != "-h" ] && fusermount -uz "$MNT"
elif [ "$FSTITCHD_WRAP" == "valgrind" ] && [ "$FSTITCHD_WRAP_OPTS" == "" ]
then
	FSTITCHD_WRAP_OPTS="--suppressions=conf/memcheck.supp --leak-check=full --show-reachable=yes --leak-resolution=high"
	$FSTITCHD_WRAP $FSTITCHD_WRAP_OPTS "$FSTITCHD" "$MNT" $FSTITCHD_OPTS "$@" \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
elif [ "$FSTITCHD_WRAP" == "cachegrind" -o "$FSTITCHD_WRAP" == callgrind ]
then
	valgrind --tool=$FSTITCHD_WRAP $FSTITCHD_WRAP_OPTS "$FSTITCHD" "$MNT" $FSTITCHD_OPTS "$@" \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
elif [ "$FSTITCHD_WRAP" == "massif" ] && [ "$FSTITCHD_WRAP_OPTS" == "" ]
then
	KWO="--tool=massif"
	KWO="$KWO --depth=5"
	KWO="$KWO --alloc-fn=chain_elt_create --alloc-fn=hash_map_insert --alloc-fn=hash_map_resize --alloc-fn=hash_map_create_size --alloc-fn=hash_map_create"
	KWO="$KWO --alloc-fn=vector_create_elts --alloc-fn=vector_create_size"
	KWO="$KWO --alloc-fn=memdup"
	valgrind $KWO "$FSTITCHD" "$MNT" $FSTITCHD_OPTS "$@" \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
elif [ "$FSTITCHD_WRAP" == "time" ]
then
	LOG=time.log
	NAME="$FSTITCHD_WRAP_OPTS"
	$FSTITCHD_WRAP -f "fstitchd-$NAME %e real %U user %S sys" -a -o "$LOG" "$FSTITCHD" "$MNT" $FSTITCHD_OPTS "$@" \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
else
	$FSTITCHD_WRAP $FSTITCHD_WRAP_OPTS "$FSTITCHD" "$MNT" $FSTITCHD_OPTS "$@" \
		|| ([ "$MNT" != "-h" ] && fusermount -uz "$MNT")
fi
