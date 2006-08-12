#!/bin/sh
# uukfsd benchmark automater

NRUNS=1
TIME_LOG=time.log
MNT=mnt
KDIR=k1
TARFILE=~/amarok/amarok-1.4-beta3c.tar.bz2
TAROUT=amarok-1.4-beta3c

WRAP_PID=0

function start_kfsd() {
	NAME="$1"
	KFSD_WRAP=time KFSD_WRAP_OPTS="$NAME" ./uukfsd.sh "$MNT/" &
	WRAP_PID=$!
	sleep 2 # wait for kfsd to export mounts
}

function stop_kfsd() {
	killall kfsd # assumes only one running kfsd (we are benchmarking!)
	wait $WRAP_PID || exit $?
}

function time_test() {
	NAME="$1"
	shift
	/usr/bin/time -f "$NAME %e real %U user %S sys" -a -o "$TIME_LOG" "$@"
}

function sigma() {
	awk '{printf $1 "+"}' | sed 's/+$/\n/' | bc -lq
}

function avg() {
	FILE="$1"
	MATCH="$2"
	FIELD="$3"
	N=`grep "^$MATCH" "$FILE" | wc -l`
	SUM=`grep "^$MATCH" "$FILE" | cut -d' ' -f $FIELD | sigma`
	AVG=`echo "scale=2; $SUM/$N" | bc -lq`
	echo $AVG
}

if [ $# -ge 1 ] && [ "$1" == "-h" ]
then
	echo "Usage: `basename \"$0\"` [NRUNS=1]"
elif [ $# -eq 1 ]
then
	NRUNS=$1
fi

echo "==== `date`" >> "$TIME_LOG"

for i in `seq $NRUNS`
do
	echo "==== start run $i"
	make BUILD=user fsclean && make user || exit 1

	start_kfsd tar
	time_test tar bash -c "tar -C \"$MNT/$KDIR/\" -jxf \"$TARFILE\"; echo syncing; obj/unix-user/user/fsync \"$MNT/\""
	stop_kfsd

	start_kfsd rm
	time_test rm bash -c "rm -rf \"$MNT/$KDIR/$TAROUT/\"; echo syncing; obj/unix-user/user/fsync \"$MNT/\""
	stop_kfsd
done

echo -n "avg-tar "; avg "$TIME_LOG" tar 2
echo -n "avg-kfsd-tar "; avg "$TIME_LOG" kfsd-tar 4
echo -n "avg-rm "; avg "$TIME_LOG" rm 2
echo -n "avg-kfsd-rm "; avg "$TIME_LOG" kfsd-rm 4
