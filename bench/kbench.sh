#!/bin/sh
# kfstitchd benchmark automater

NRUNS=1
TIME_LOG=time.log
PROF_LOG=prof.log
DISK=${DISK:-/dev/sdb}
JOURNAL=${JOURNAL:-0}
KMNT=fstitch:/
MNT=/mnt/test
TARFILE=linux-2.6.15.tar
TAROUT=linux-2.6.15
VMLINUX=/lib/modules/`uname -r`/source/vmlinux

WRAP_PID=0

function start_fstitchd() {
	#mkfs.ext3 -b 4096 $DISK || exit 1
	#mount -t ext3 $DISK $MNT || exit 1
	#~frost/acctrd/acctrdctl -s $DISK || exit 1

	if [ "$NWBBLOCKS" == "" ]; then
		insmod fscore/kfstitchd.ko linux_device=$DISK use_journal=$JOURNAL || exit 1
	else
		insmod fscore/kfstitchd.ko linux_device=$DISK use_journal=$JOURNAL nwbblocks="$NWBBLOCKS" || exit 1
	fi
	if [ -f /proc/kfstitchd_debug ]
	then
		echo "Start reading from /proc/kfstitchd_debug, then press enter."
		read
	fi
	mount -t fstitch "$KMNT" "$MNT"
	if [ $? -ne 0 ]; then rmmod kfstitchd; exit 1; fi
	[ $PROFILE -eq 1 ] && (opcontrol --start; opcontrol --reset)
}

function stop_fstitchd() {
	[ $PROFILE -eq 1 ] && (opcontrol --stop; opcontrol --dump)
	umount "$MNT" || echo "umount "$MNT" failed: $?"
	#~frost/acctrd/acctrdctl -s $DISK || exit 1
	rmmod kfstitchd || echo "rmmod kfstitchd failed: $?"
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
	echo "Usage: `basename \"$0\"` <MAKE_USER> <ufs|ext2|ext2-small> [NRUNS=1]"
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

FSIMG=
if [ "$2" == "ufs" ]; then
	FSIMG=obj/images/ufs.img
elif [ "$2" == "ext2" ]; then
	FSIMG=obj/images/ext2-10G.img
elif [ "$2" == "ext2-small" ]; then
	FSIMG=obj/images/ext2.img
elif [ "$2" != "${2#=}" ]; then
	FSIMG="${2#=}"
else
	usage 2>&1
	exit 1
fi

[ -f "$FSIMG" ] || su "$REAL_USER" -c "make \"$FSIMG\"" || exit 1

if [ $# -ge 3 ]
then
	NRUNS=$3
fi

if [ $# -ge 4 ]
then
	usage
fi

# Try to load oprofile, then detect whether it worked
zgrep -q OPROFILE /proc/config.gz && modprobe oprofile
PROFILE=`lsmod | grep -q ^oprofile && echo 1 || echo 0`
grep -q idle=poll /proc/cmdline && TP=.2 || TP=1

echo "Using disk $DISK"

su "$REAL_USER" -c "touch \"$TIME_LOG\""
su "$REAL_USER" -c "touch \"$PROF_LOG\""
echo "==== `date`" >> "$TIME_LOG"

su "$REAL_USER" -c "make" || exit 1

if [ $PROFILE -eq 1 ]; then
	if [ ! $VMLINUX ]; then
		echo "vmlinux \"$VMLINUX\" does not exist, cannot profile" 1>&2
		exit 1
	fi
	#opcontrol --deinit
	opcontrol --init
	opcontrol --setup --separate=kernel --vmlinux=$VMLINUX
fi

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
	echo w | fdisk "$DISK" &> /dev/null || exit 1
	echo "."

	if [ $JOURNAL != 0 -a "${2#ext2}" != "$2" ]; then
		mount "${DISK}1" "$MNT" -t ext2
		if [ ! -f "$MNT/.journal" ]; then
			echo -n "Creating journal"
			dd if=/dev/zero of="$MNT/.journal" bs=1M count=64 2>/dev/null
			sync
			echo "."
		fi
		umount "$MNT"
	fi

	echo -n "Loading tar file into cache"
	cat "$TARFILE" > /dev/null || exit 1
	echo "."

	start_fstitchd

	time_test tar bash -c "time tar -C \"$MNT/\" -xf \"$TARFILE\"; time obj/util/fsync \"$MNT/\""
	time_test rm bash -c "time rm -rf \"$MNT/$TAROUT/\"; time obj/util/fsync \"$MNT/\""

	stop_fstitchd

	echo "==== `date`" >> "$PROF_LOG"
	[ $PROFILE -eq 1 ] && opreport -l -g -p fscore/ -t $TP | su $REAL_USER -c "tee -a \"$PROF_LOG\""
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
