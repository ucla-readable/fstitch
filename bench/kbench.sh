#!/bin/sh
# kkkfsd benchmark automater

NRUNS=1
TIME_LOG=time.log
DISK=/dev/sdb
FSIMG=obj/unix-user/fs/ufs.img
KMNT=kfs:/
MNT=/mnt/test
TARFILE=linux-2.6.15.tar
TAROUT=linux-2.6.15

WRAP_PID=0

function start_kfsd() {
	if [ "$NWBBLOCKS" == "" ]; then
		insmod kfs/kkfsd.ko || exit 1
	else
		insmod kfs/kkfsd.ko nwbblocks="$NWBBLOCKS" || exit 1
	fi
	mount -t kfs "$KMNT" "$MNT"
	if [ $? -ne 0 ]; then rmmod kkfsd; exit 1; fi
}

function stop_kfsd() {
	umount "$MNT" || echo "umount "$MNT" failed: $?"
	rmmod kkfsd || echo "rmmod kkfsd failed: $?"
}

function time_test() {
	NAME="$1"
	shift
	echo "test: $NAME"
	/usr/bin/time -f "$NAME %e real %U user %S sys" -a -o "$TIME_LOG" "$@"
}

function sigma() {
	awk '{printf $1 "+"}' | sed 's/+$/\n/' | bc -lq
}

function extract_wall_time() {
	FILE="$1"
	MATCH="$2"
	FIELD=2
	grep "^$MATCH" "$FILE" | cut -d' ' -f $FIELD
}

function avg() {
	FILE="$1"
	MATCH="$2"
	FIELD="$3"
	N=`grep "^$MATCH" "$FILE" | wc -l`
	SUM=`extract_wall_time "$FILE" "$MATCH" | sigma`
	AVG=`echo "scale=2; $SUM/$N" | bc -lq`
	echo $AVG
}

function min_helper() {
	sort -n | head -1
}

function max_helper() {
	sort -n | tail -1
}

function min() {
	FILE="$1"
	MATCH="$2"
	FIELD="$3"
	MIN=`extract_wall_time "$FILE" "$MATCH" | min_helper`
	echo $MIN
}

function max() {
	FILE="$1"
	MATCH="$2"
	FIELD="$3"
	MAX=`extract_wall_time "$FILE" "$MATCH" | max_helper`
	echo $MAX
}

function usage() {
	echo "Usage: `basename \"$0\"` <MAKE_USER> [NRUNS=1]"
	echo "       where MAKE_USER is the name of the user for running make"
}


if [ "`whoami`" != "root" ]
then
	echo "Must run as root" 2>&1
	exit 1
fi

if [ $# -eq 0 ]
then
	usage 2>&1
	exit 1
fi

REAL_USER="$1"

if [ $# -ge 3 ] && [ "$2" == "-h" ]
then
	usage
elif [ $# -eq 2 ]
then
	NRUNS=$2
fi

su "$REAL_USER" -c "touch \"$TIME_LOG\""
echo "==== `date`" >> "$TIME_LOG"

su "$REAL_USER" -c "make BUILD=user fsclean && make BUILD=user \"$FSIMG\"" || exit 1
su "$REAL_USER" -c "make BUILD=user obj/unix-user/user/fsync" || exit 1
su "$REAL_USER" -c "make kernel" || exit 1

for i in `seq $NRUNS`
do
	echo "==== start run $i"

	mount | grep -q "$DISK"
	if [ $? -eq 0 ]; then
		echo "WARNING: $DISK already mounted. Exiting for safety." 2>&1
		exit 1
	fi

	echo -n "Copying disk image to disk"
	dd if="$FSIMG" of="$DISK" bs=512K 2>/dev/null || exit 1
	echo "."

	echo -n "Loading tar file into cache"
	cat "$TARFILE" > /dev/null || exit 1
	echo "."

	start_kfsd

	time_test tar bash -c "tar -C \"$MNT/\" -xf \"$TARFILE\"; echo syncing; obj/unix-user/user/fsync \"$MNT/\""
	time_test rm bash -c "rm -rf \"$MNT/$TAROUT/\"; echo syncing; obj/unix-user/user/fsync \"$MNT/\""

	stop_kfsd
done

echo "---- complete"
(
echo -n "avg-tar "; avg "$TIME_LOG" tar 2
echo -n "min-tar "; min "$TIME_LOG" tar 2
echo -n "max-tar "; max "$TIME_LOG" tar 2
echo -n "avg-rm "; avg "$TIME_LOG" rm 2
echo -n "min-rm "; min "$TIME_LOG" rm 2
echo -n "max-rm "; max "$TIME_LOG" rm 2
) | tee -a "$TIME_LOG"

#chown "$REAL_USER" "$TIME_LOG"
